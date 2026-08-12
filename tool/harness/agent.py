#!/usr/bin/env python3
"""Single Harness V2 command-line entrypoint."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

import attestation
import approval
import bootstrap
import closure
import git_ops
import github_evidence
import merge_queue
import model
import state
import tdd
import workspace
from executor import GateExecutionError, GateExecutor
from gates import (
    GatePlanError,
    global_gate_plan,
    plan_for_platform,
    plan_gates,
    plan_task_set,
)


ROOT = Path(__file__).resolve().parents[2]
ERRORS = (
    attestation.AttestationError,
    approval.ApprovalError,
    bootstrap.BootstrapError,
    closure.ClosureError,
    GateExecutionError,
    GatePlanError,
    github_evidence.GitHubEvidenceError,
    git_ops.GitError,
    merge_queue.QueueError,
    model.ContractError,
    state.StateError,
    tdd.TddError,
    workspace.WorkspaceError,
)
BOOTSTRAP_JOBS = (
    "Candidate plan",
    "Harness V2",
    "Product gates (linux)",
    "Product gates (macos)",
    "Product gates (windows)",
    "Cutover security",
    "Candidate accepted",
)
BOOTSTRAP_ARTIFACTS = (
    "candidate-evidence-linux",
    "candidate-evidence-macos",
    "candidate-evidence-windows",
)


def _actor(root: Path, kind: str = "user") -> Dict[str, str]:
    actor = state.git_actor(root)
    actor["kind"] = kind
    return actor


def _store(
    contracts: model.ContractSet, remote: Optional[str], kind: str = "user"
) -> state.StateStore:
    return state.StateStore(
        contracts,
        remote=remote,
        actor=_actor(contracts.root, kind),
    )


def _task_from_head(root: Path) -> str:
    message = git_ops.git_text(root, "show", "-s", "--format=%B", "HEAD")
    task_ids = []
    for line in message.splitlines():
        if line.startswith("Xnn-Task: "):
            task_ids.append(line.split(": ", 1)[1])
    if len(task_ids) != 1 or model.TASK_ID.fullmatch(task_ids[0]) is None:
        raise merge_queue.QueueError(
            "HEAD must contain exactly one valid Xnn-Task trailer"
        )
    return task_ids[0]


def _candidate_task_ids(
    contracts: model.ContractSet,
    remote: Optional[str],
    head: str = "HEAD",
) -> tuple[List[str], str]:
    branch = contracts.manifest["integration_branch"]
    protected_ref = "refs/heads/{}".format(branch)
    if remote is not None:
        base = git_ops.fetch_remote_object(contracts.root, remote, protected_ref)
    else:
        base = git_ops.ref_sha(contracts.root, protected_ref)
    if base is None:
        raise merge_queue.QueueError(
            "integration branch {} is unavailable".format(branch)
        )
    candidate = git_ops.object_id(contracts.root, head)
    if candidate == base or not git_ops.is_ancestor(contracts.root, base, candidate):
        raise merge_queue.QueueError(
            "candidate HEAD is not ahead of integration branch {}".format(branch)
        )
    task_ids: List[str] = []
    for commit in git_ops.commit_range(contracts.root, base, candidate):
        message = git_ops.git_text(contracts.root, "show", "-s", "--format=%B", commit)
        trailers = [
            line.removeprefix("Xnn-Task: ")
            for line in message.splitlines()
            if line.startswith("Xnn-Task: ")
        ]
        if len(trailers) != 1 or trailers[0] not in contracts.tasks:
            raise merge_queue.QueueError(
                "{} candidate commit has invalid task provenance".format(commit[:12])
            )
        task_ids.append(trailers[0])
    if len(task_ids) != len(set(task_ids)):
        raise merge_queue.QueueError("candidate contains duplicate task deliveries")
    return task_ids, base


def _verification_artifact(
    contracts: model.ContractSet,
    task_ids: Sequence[str],
    result: Any,
) -> Dict[str, Any]:
    gate_digests = [
        model.canonical_sha256(dict(item.attestation)) for item in result.results
    ]
    gate_ids = [item.gate_id for item in result.results]
    platforms = {str(item.attestation["platform"]) for item in result.results}
    if len(platforms) != 1:
        raise GateExecutionError("Gate artifact contains mixed platforms")
    criterion_ids = sorted(
        {
            criterion_id
            for task_id in task_ids
            for criterion_id in contracts.tasks[task_id]["criteria"]
        }
    )
    criteria = [
        attestation.criterion_evidence_digest(
            contracts,
            criterion_id,
            git_ops.object_id(contracts.root, "HEAD"),
            gate_digests,
        )
        for criterion_id in criterion_ids
    ]
    return {
        "schema_version": 1,
        "source_sha": git_ops.object_id(contracts.root, "HEAD"),
        "platform": platforms.pop(),
        "gate_ids": gate_ids,
        "gate_attestations": gate_digests,
        "criterion_ids": criterion_ids,
        "criterion_evidence": criteria,
    }


def _bootstrap_verification_artifact(
    contracts: model.ContractSet, result: Any, plan_digest: str
) -> Dict[str, Any]:
    gate_attestations = [
        model.canonical_sha256(dict(item.attestation)) for item in result.results
    ]
    platforms = {str(item.attestation["platform"]) for item in result.results}
    if len(platforms) != 1:
        raise GateExecutionError("bootstrap artifact contains mixed platforms")
    return {
        "schema_version": 1,
        "kind": "bootstrap_cutover",
        "source_sha": git_ops.object_id(contracts.root, "HEAD"),
        "source_tree": git_ops.current_tree(contracts.root, "HEAD"),
        "platform": platforms.pop(),
        "plan_sha256": plan_digest,
        "gate_ids": [item.gate_id for item in result.results],
        "gate_attestations": gate_attestations,
        "skipped": any(item.attestation["skipped"] for item in result.results),
    }


def _platform_matrix(
    contracts: model.ContractSet, task_ids: Sequence[str]
) -> List[Dict[str, str]]:
    runners = {
        "linux": "ubuntu-latest",
        "macos": "macos-latest",
        "windows": "windows-2022",
    }
    return [
        {"runner": runners[platform], "label": platform}
        for platform in model.task_required_platforms(contracts, task_ids)
    ]


def _required_jobs(contracts: model.ContractSet, task_ids: Sequence[str]) -> List[str]:
    return [
        "Candidate plan",
        "Harness V2",
        *(
            "Product gates ({})".format(item["label"])
            for item in _platform_matrix(contracts, task_ids)
        ),
        "Candidate accepted",
    ]


def _required_artifacts(
    contracts: model.ContractSet, task_ids: Sequence[str]
) -> List[str]:
    return [
        "candidate-evidence-{}".format(item["label"])
        for item in _platform_matrix(contracts, task_ids)
    ]


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument(
        "--remote",
        default="origin",
        help="remote used for authoritative refs; use --local for no remote",
    )
    parser.add_argument("--local", action="store_const", const=None, dest="remote")
    commands = parser.add_subparsers(dest="command", required=True)

    commands.add_parser("validate")
    commands.add_parser("list")

    claim = commands.add_parser("claim")
    claim.add_argument("task_id")
    claim.add_argument("--path", type=Path)

    claim_recover = commands.add_parser("claim-recover")
    claim_recover.add_argument("task_id")

    red = commands.add_parser("tdd-red")
    red.add_argument("task_id")

    verify = commands.add_parser("verify")
    verify.add_argument("task_id", nargs="?")
    verify.add_argument("--phase", required=True)
    verify.add_argument("--platform", default="local")
    verify.add_argument("--output", type=Path)
    verify.add_argument("--no-cache", action="store_true")

    verify_all = commands.add_parser("verify-all")
    verify_all.add_argument("--platform", default="local")
    verify_all.add_argument("--output", type=Path)
    verify_all.add_argument("--no-cache", action="store_true")

    matrix = commands.add_parser("matrix")
    matrix.add_argument("task_id", nargs="?")

    submit = commands.add_parser("submit")
    submit.add_argument("task_id")
    submit.add_argument("--red-sha")

    build = commands.add_parser("queue-build")
    build.add_argument("task_ids", nargs="+")
    build.add_argument("--train-id", required=True)
    build.add_argument("--base")

    reopen = commands.add_parser("queue-reopen")
    reopen.add_argument("task_id")
    reopen.add_argument("queue_ref")
    reopen.add_argument("--reason", required=True)

    close_acceptance = commands.add_parser("acceptance-close")
    close_acceptance.add_argument("task_id")

    bootstrap_accept = commands.add_parser("bootstrap-accept")
    bootstrap_accept.add_argument("queue_ref")
    bootstrap_accept.add_argument(
        "--workflow", default=".github/workflows/merge-queue.yml"
    )
    bootstrap_accept.add_argument("--at", required=True)

    bootstrap_publish = commands.add_parser("bootstrap-publish")
    bootstrap_publish.add_argument("queue_ref")
    bootstrap_publish.add_argument(
        "--workflow", default=".github/workflows/merge-queue.yml"
    )

    collect = commands.add_parser("collect-evidence")
    collect.add_argument("task_id")
    collect.add_argument("queue_ref")
    collect.add_argument("--workflow", default=".github/workflows/merge-queue.yml")
    collect.add_argument("--artifact", action="append")
    collect.add_argument("--output", type=Path, required=True)

    publish = commands.add_parser("publish")
    publish.add_argument("task_id")
    publish.add_argument("queue_ref")
    publish.add_argument("--evidence", type=Path, required=True)
    publish.add_argument("--repository")
    publish.add_argument("--workflow", default=".github/workflows/merge-queue.yml")
    publish.add_argument("--required-job", action="append")
    publish.add_argument(
        "--required-artifact",
        action="append",
    )

    recover = commands.add_parser("recover")
    recover.add_argument("task_id")
    recover.add_argument("queue_ref")
    recover.add_argument("--repository")
    recover.add_argument("--workflow", default=".github/workflows/merge-queue.yml")
    recover.add_argument("--required-job", action="append")
    recover.add_argument(
        "--required-artifact",
        action="append",
    )

    approve = commands.add_parser("approve-plan")
    approve.add_argument("plan", type=Path)
    approve.add_argument("--at", required=True)
    return parser


def _publisher(
    contracts: model.ContractSet,
    store: state.StateStore,
    remote: Optional[str],
    repository: Optional[str],
    workflow: str,
    jobs: Sequence[str],
    artifacts: Sequence[str],
) -> merge_queue.Publisher:
    if remote is None:
        raise merge_queue.QueueError("publication requires an authoritative remote")
    if repository is None:
        remote_url = git_ops.git_text(contracts.root, "remote", "get-url", remote)
        repository = github_evidence.repository_slug(remote_url)
    return merge_queue.Publisher(
        contracts,
        store,
        remote,
        repository,
        workflow,
        jobs,
        artifacts,
        _actor(contracts.root, "queue-worker"),
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _parser().parse_args(argv)
    root = args.root.resolve()
    try:
        if args.command == "approve-plan":
            path = args.plan if args.plan.is_absolute() else root / args.plan
            digest = model.approve_plan(root, path, args.at)
            manifest = model.load_json(root / ".agents" / "manifest.json")
            plan = model.load_json(path)
            commit = approval.ApprovalStore(root, manifest, args.remote).write(
                plan, args.at
            )
            print(
                json.dumps(
                    {
                        "plan_id": plan["id"],
                        "content_sha256": digest,
                        "approval_commit": commit,
                    },
                    sort_keys=True,
                )
            )
            return 0
        contracts = model.load_contracts(root)
        if args.command == "validate":
            print(
                json.dumps(
                    {
                        "status": "valid",
                        "plans": len(contracts.plans),
                        "tasks": len(contracts.tasks),
                        "gates": len(contracts.gates),
                    },
                    sort_keys=True,
                )
            )
            return 0
        if args.command == "list":
            store = _store(contracts, args.remote)
            for task_id, snapshot in store.list().items():
                print(
                    "{}\t{}\t{}".format(
                        task_id,
                        snapshot.state,
                        contracts.tasks[task_id]["title"],
                    )
                )
            return 0
        if args.command == "claim":
            store = _store(contracts, args.remote)
            manager = workspace.WorkspaceManager(contracts, store)
            claimed = manager.claim(args.task_id, args.path)
            print(
                json.dumps(
                    {
                        "task_id": claimed.task_id,
                        "path": str(claimed.path),
                        "branch": claimed.branch,
                        "base_sha": claimed.base_sha,
                    },
                    sort_keys=True,
                )
            )
            return 0
        if args.command == "claim-recover":
            store = _store(contracts, args.remote, "recovery")
            manager = workspace.WorkspaceManager(contracts, store)
            recovered = manager.recover_claim(args.task_id)
            if recovered is None:
                print("{} returned to ready".format(args.task_id))
            else:
                print(
                    json.dumps(
                        {
                            "task_id": recovered.task_id,
                            "path": str(recovered.path),
                            "branch": recovered.branch,
                            "base_sha": recovered.base_sha,
                        },
                        sort_keys=True,
                    )
                )
            return 0
        if args.command == "tdd-red":
            store = _store(contracts, args.remote)
            manager = workspace.WorkspaceManager(contracts, store)
            value = tdd.TddManager(contracts, store, manager).record_red(args.task_id)
            print(model.canonical_sha256(value))
            return 0
        if args.command == "verify":
            candidate_base = None
            if args.task_id is None and args.phase == "queue":
                task_ids, candidate_base = _candidate_task_ids(contracts, args.remote)
            else:
                task_ids = [args.task_id or _task_from_head(root)]
            changed = None
            if candidate_base is not None:
                changed = git_ops.changed_paths(root, candidate_base, "HEAD")
            elif args.phase in {"review", "queue"}:
                parent_result = git_ops.run_git(root, "rev-parse", "HEAD^", check=False)
                if parent_result.returncode == 0:
                    parent = parent_result.stdout.decode("ascii").strip()
                    changed = git_ops.changed_paths(root, parent, "HEAD")
                else:
                    changed = sorted(
                        {
                            path
                            for task_id in task_ids
                            for path in contracts.tasks[task_id]["owned_paths"]
                        }
                    )
            if len(task_ids) == 1:
                plan = plan_gates(contracts, task_ids[0], args.phase, changed)
            else:
                plan = plan_task_set(contracts, task_ids, args.phase, changed)
            plan = plan_for_platform(contracts, plan, args.platform)
            result = GateExecutor(
                contracts,
                platform_label=args.platform,
                cache_enabled=not args.no_cache,
            ).execute(plan)
            result.require_success()
            artifact = _verification_artifact(contracts, task_ids, result)
            if args.output is not None:
                git_ops.atomic_write_json(args.output, artifact)
            print(json.dumps(artifact, sort_keys=True))
            return 0
        if args.command == "verify-all":
            global_plan = global_gate_plan(contracts)
            plan = plan_for_platform(contracts, global_plan, args.platform)
            result = GateExecutor(
                contracts,
                platform_label=args.platform,
                cache_enabled=not args.no_cache,
            ).execute(plan)
            result.require_success()
            artifact = _bootstrap_verification_artifact(
                contracts, result, global_plan.digest
            )
            if args.output is not None:
                git_ops.atomic_write_json(args.output, artifact)
            print(json.dumps(artifact, sort_keys=True))
            return 0
        if args.command == "matrix":
            if args.task_id is None:
                task_ids, _ = _candidate_task_ids(contracts, args.remote)
            else:
                task_ids = [args.task_id]
            print(
                json.dumps(
                    {"include": _platform_matrix(contracts, task_ids)},
                    separators=(",", ":"),
                )
            )
            return 0
        if args.command == "submit":
            store = _store(contracts, args.remote)
            manager = workspace.WorkspaceManager(contracts, store)
            submission = merge_queue.SubmissionManager(
                contracts,
                store,
                manager,
                _actor(root),
                remote=args.remote,
            ).submit(args.task_id, args.red_sha)
            print(
                json.dumps(
                    {
                        "task_id": submission.task_id,
                        "ref": submission.ref,
                        "commit": submission.commit,
                    },
                    sort_keys=True,
                )
            )
            return 0
        if args.command == "queue-build":
            store = _store(contracts, args.remote, "queue-worker")
            train = merge_queue.MergeQueue(
                contracts, store, remote=args.remote
            ).build_train(args.task_ids, args.train_id, args.base)
            print(
                json.dumps(
                    {
                        "train_id": train.train_id,
                        "base_sha": train.base_sha,
                        "entries": [
                            {
                                "task_id": entry.task_id,
                                "parent_sha": entry.parent_sha,
                                "candidate_sha": entry.candidate_sha,
                                "queue_ref": entry.queue_ref,
                            }
                            for entry in train.entries
                        ],
                    },
                    sort_keys=True,
                )
            )
            return 0
        if args.command == "queue-reopen":
            store = _store(contracts, args.remote, "recovery")
            queue = merge_queue.MergeQueue(contracts, store, remote=args.remote)
            entry = queue.entry_from_ref(args.task_id, args.queue_ref)
            path = queue.reopen(entry, args.reason)
            print(path)
            return 0
        if args.command == "acceptance-close":
            if args.remote is None:
                raise closure.ClosureError(
                    "acceptance closure requires an authoritative remote"
                )
            store = _store(contracts, args.remote, "queue-worker")
            manager = workspace.WorkspaceManager(contracts, store)
            value = closure.AcceptanceCloser(
                contracts, store, manager, args.remote
            ).close(args.task_id)
            print(model.canonical_sha256(value))
            return 0
        if args.command == "bootstrap-accept":
            if args.remote is None:
                raise bootstrap.BootstrapError(
                    "bootstrap acceptance requires an authoritative remote"
                )
            prefix = contracts.manifest["ref_namespaces"]["queue"] + "bootstrap/"
            if not args.queue_ref.startswith(prefix):
                raise bootstrap.BootstrapError(
                    "bootstrap queue ref must be under {}".format(prefix)
                )
            candidate_sha = git_ops.fetch_remote_object(
                root, args.remote, args.queue_ref
            )
            candidate_tree = git_ops.current_tree(root, candidate_sha)
            protected_ref = "refs/heads/{}".format(
                contracts.manifest["integration_branch"]
            )
            integration_base = git_ops.fetch_remote_object(
                root, args.remote, protected_ref
            )
            if not git_ops.is_ancestor(root, integration_base, candidate_sha):
                raise bootstrap.BootstrapError(
                    "bootstrap candidate is not based on the protected head"
                )
            remote_url = git_ops.git_text(root, "remote", "get-url", args.remote)
            repository = github_evidence.repository_slug(remote_url)
            workflow_blob = git_ops.object_id(
                root, "{}:{}".format(candidate_sha, args.workflow)
            )
            client = github_evidence.GitHubClient(
                repository, github_evidence.credential_token(root)
            )
            branch = args.queue_ref[len("refs/heads/") :]
            evidence = github_evidence.collect_bootstrap_workflow_evidence(
                client,
                workflow_path=args.workflow,
                workflow_blob=workflow_blob,
                branch=branch,
                candidate_sha=candidate_sha,
                candidate_tree=candidate_tree,
                required_artifacts=BOOTSTRAP_ARTIFACTS,
            )
            global_plan = global_gate_plan(contracts)
            expected_gates = {
                platform: plan_for_platform(contracts, global_plan, platform).leaves
                for platform in ("linux", "macos", "windows")
            }
            normalized = bootstrap.validate_workflow_evidence(
                evidence,
                repository=repository,
                workflow_path=args.workflow,
                workflow_blob=workflow_blob,
                candidate_sha=candidate_sha,
                candidate_tree=candidate_tree,
                candidate_branch=branch,
                required_jobs=BOOTSTRAP_JOBS,
                required_artifacts=BOOTSTRAP_ARTIFACTS,
                expected_plan_sha256=global_plan.digest,
                expected_gates=expected_gates,
            )
            owner = contracts.manifest["project_owner"]
            actual = state.git_actor(root)
            if actual["name"] != owner["name"] or actual["email"] != owner["email"]:
                raise bootstrap.BootstrapError(
                    "current Git identity is not the configured project owner"
                )
            actor = {
                "kind": "project-owner",
                "id": owner["id"],
                "name": owner["name"],
                "email": owner["email"],
            }
            value = bootstrap.create_attestation(
                candidate_sha=candidate_sha,
                candidate_tree=candidate_tree,
                integration_base=integration_base,
                workflow=normalized,
                required_jobs=BOOTSTRAP_JOBS,
                required_artifacts=BOOTSTRAP_ARTIFACTS,
                actor=actor,
                created_at=args.at,
            )
            store = bootstrap.AcceptanceStore(contracts, args.remote)
            commit = store.write(value)
            print(
                json.dumps(
                    {
                        "attestation_ref": store.ref(candidate_sha),
                        "attestation_commit": commit,
                        "attestation_sha256": model.canonical_sha256(value),
                        "run_id": normalized["run_id"],
                    },
                    sort_keys=True,
                )
            )
            return 0
        if args.command == "bootstrap-publish":
            if args.remote is None:
                raise bootstrap.BootstrapError(
                    "bootstrap publication requires an authoritative remote"
                )
            prefix = contracts.manifest["ref_namespaces"]["queue"] + "bootstrap/"
            if not args.queue_ref.startswith(prefix):
                raise bootstrap.BootstrapError(
                    "bootstrap queue ref must be under {}".format(prefix)
                )
            candidate_sha = git_ops.fetch_remote_object(
                root, args.remote, args.queue_ref
            )
            candidate_tree = git_ops.current_tree(root, candidate_sha)
            remote_url = git_ops.git_text(root, "remote", "get-url", args.remote)
            repository = github_evidence.repository_slug(remote_url)
            workflow_blob = git_ops.object_id(
                root, "{}:{}".format(candidate_sha, args.workflow)
            )
            branch = args.queue_ref[len("refs/heads/") :]
            global_plan = global_gate_plan(contracts)
            expected_gates = {
                platform: plan_for_platform(contracts, global_plan, platform).leaves
                for platform in ("linux", "macos", "windows")
            }
            store = bootstrap.AcceptanceStore(contracts, args.remote)
            value = store.read(candidate_sha)
            normalized = bootstrap.validate_workflow_evidence(
                value["workflow"],
                repository=repository,
                workflow_path=args.workflow,
                workflow_blob=workflow_blob,
                candidate_sha=candidate_sha,
                candidate_tree=candidate_tree,
                candidate_branch=branch,
                required_jobs=BOOTSTRAP_JOBS,
                required_artifacts=BOOTSTRAP_ARTIFACTS,
                expected_plan_sha256=global_plan.digest,
                expected_gates=expected_gates,
            )
            rebuilt = bootstrap.create_attestation(
                candidate_sha=candidate_sha,
                candidate_tree=candidate_tree,
                integration_base=value["integration_base"],
                workflow=normalized,
                required_jobs=BOOTSTRAP_JOBS,
                required_artifacts=BOOTSTRAP_ARTIFACTS,
                actor=value["created_by"],
                created_at=value["created_at"],
            )
            if rebuilt != value:
                raise bootstrap.BootstrapError(
                    "bootstrap acceptance does not match current contracts"
                )
            result = bootstrap.publish_candidate(
                contracts,
                args.remote,
                value,
            )
            print(
                json.dumps(
                    {
                        "candidate_sha": candidate_sha,
                        "protected_branch": contracts.manifest["integration_branch"],
                        "result": result,
                    },
                    sort_keys=True,
                )
            )
            return 0
        if args.command == "collect-evidence":
            if args.remote is None:
                raise github_evidence.GitHubEvidenceError(
                    "evidence collection requires a GitHub remote"
                )
            store = _store(contracts, args.remote, "queue-worker")
            entry = merge_queue.MergeQueue(
                contracts, store, remote=args.remote
            ).entry_from_ref(args.task_id, args.queue_ref)
            task_ids, _ = _candidate_task_ids(
                contracts, args.remote, entry.candidate_sha
            )
            required_artifacts = sorted(
                set(args.artifact or []) | set(_required_artifacts(contracts, task_ids))
            )
            remote_url = git_ops.git_text(root, "remote", "get-url", args.remote)
            repository = github_evidence.repository_slug(remote_url)
            client = github_evidence.GitHubClient(
                repository, github_evidence.credential_token(root)
            )
            evidence = github_evidence.collect_workflow_evidence(
                client,
                workflow_path=args.workflow,
                workflow_blob=git_ops.object_id(
                    root,
                    "{}:{}".format(entry.candidate_sha, args.workflow),
                ),
                branch=args.queue_ref[len("refs/heads/") :],
                candidate_sha=entry.candidate_sha,
                required_artifacts=required_artifacts,
            )
            git_ops.atomic_write_json(args.output, evidence)
            print(args.output)
            return 0
        if args.command in {"publish", "recover"}:
            store = _store(contracts, args.remote, "queue-worker")
            entry = merge_queue.MergeQueue(
                contracts, store, remote=args.remote
            ).entry_from_ref(args.task_id, args.queue_ref)
            task_ids, _ = _candidate_task_ids(
                contracts, args.remote, entry.candidate_sha
            )
            required_jobs = sorted(
                set(args.required_job or []) | set(_required_jobs(contracts, task_ids))
            )
            required_artifacts = sorted(
                set(args.required_artifact or [])
                | set(_required_artifacts(contracts, task_ids))
            )
            publisher = _publisher(
                contracts,
                store,
                args.remote,
                args.repository,
                args.workflow,
                required_jobs,
                required_artifacts,
            )
            if args.command == "recover":
                publisher.recover(entry)
                print("{} recovered".format(args.task_id))
                return 0
            evidence = model.load_json(args.evidence)
            gate_attestations: List[str] = []
            criterion_evidence: List[str] = []
            for artifact in evidence.get("artifacts", []):
                gate_attestations.extend(artifact.get("gate_attestations", []))
                criterion_evidence.extend(artifact.get("criterion_evidence", []))
            value = publisher.publish(
                entry,
                evidence,
                sorted(set(gate_attestations)),
                sorted(set(criterion_evidence)),
            )
            print(model.canonical_sha256(value))
            return 0
    except ERRORS as error:
        print("Harness V2 error:\n{}".format(error), file=sys.stderr)
        return 1
    raise AssertionError("unreachable command")


if __name__ == "__main__":
    raise SystemExit(main())
