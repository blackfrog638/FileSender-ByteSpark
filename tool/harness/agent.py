#!/usr/bin/env python3
"""Single Harness V2 command-line entrypoint."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

import attestation
import git_ops
import github_evidence
import merge_queue
import model
import state
import tdd
import workspace
from executor import GateExecutionError, GateExecutor
from gates import GatePlanError, plan_gates


ROOT = Path(__file__).resolve().parents[2]
ERRORS = (
    attestation.AttestationError,
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


def _verification_artifact(
    contracts: model.ContractSet,
    task_id: str,
    result: Any,
) -> Dict[str, Any]:
    gate_digests = [
        model.canonical_sha256(dict(item.attestation))
        for item in result.results
    ]
    criteria = []
    for criterion_id in contracts.tasks[task_id]["criteria"]:
        criteria.append(
            model.canonical_sha256(
                {
                    "criterion_id": criterion_id,
                    "source_sha": git_ops.object_id(contracts.root, "HEAD"),
                    "gate_attestations": gate_digests,
                }
            )
        )
    return {
        "schema_version": 1,
        "source_sha": git_ops.object_id(contracts.root, "HEAD"),
        "gate_attestations": gate_digests,
        "criterion_evidence": criteria,
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument(
        "--remote",
        default="origin",
        help="remote used for authoritative refs; use --local for no remote",
    )
    parser.add_argument(
        "--local", action="store_const", const=None, dest="remote"
    )
    commands = parser.add_subparsers(dest="command", required=True)

    commands.add_parser("validate")
    commands.add_parser("list")

    claim = commands.add_parser("claim")
    claim.add_argument("task_id")
    claim.add_argument("--path", type=Path)

    red = commands.add_parser("tdd-red")
    red.add_argument("task_id")

    verify = commands.add_parser("verify")
    verify.add_argument("task_id", nargs="?")
    verify.add_argument("--phase", required=True)
    verify.add_argument("--platform", default="local")
    verify.add_argument("--output", type=Path)
    verify.add_argument("--no-cache", action="store_true")

    matrix = commands.add_parser("matrix")
    matrix.add_argument("task_id", nargs="?")

    submit = commands.add_parser("submit")
    submit.add_argument("task_id")
    submit.add_argument("--red-sha")

    build = commands.add_parser("queue-build")
    build.add_argument("task_ids", nargs="+")
    build.add_argument("--train-id", required=True)
    build.add_argument("--base")

    collect = commands.add_parser("collect-evidence")
    collect.add_argument("task_id")
    collect.add_argument("queue_ref")
    collect.add_argument("--workflow", default=".github/workflows/merge-queue.yml")
    collect.add_argument(
        "--artifact", action="append"
    )
    collect.add_argument("--output", type=Path, required=True)

    publish = commands.add_parser("publish")
    publish.add_argument("task_id")
    publish.add_argument("queue_ref")
    publish.add_argument("--evidence", type=Path, required=True)
    publish.add_argument("--repository")
    publish.add_argument("--workflow", default=".github/workflows/merge-queue.yml")
    publish.add_argument(
        "--required-job", action="append", required=True
    )
    publish.add_argument(
        "--required-artifact",
        action="append",
    )

    recover = commands.add_parser("recover")
    recover.add_argument("task_id")
    recover.add_argument("queue_ref")
    recover.add_argument("--repository")
    recover.add_argument("--workflow", default=".github/workflows/merge-queue.yml")
    recover.add_argument(
        "--required-job", action="append", required=True
    )
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
        remote_url = git_ops.git_text(
            contracts.root, "remote", "get-url", remote
        )
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
            print(model.approve_plan(root, path, args.at))
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
        if args.command == "tdd-red":
            store = _store(contracts, args.remote)
            manager = workspace.WorkspaceManager(contracts, store)
            value = tdd.TddManager(
                contracts, store, manager
            ).record_red(args.task_id)
            print(model.canonical_sha256(value))
            return 0
        if args.command == "verify":
            task_id = args.task_id or _task_from_head(root)
            changed = None
            if args.phase in {"review", "queue"}:
                parent_result = git_ops.run_git(
                    root, "rev-parse", "HEAD^", check=False
                )
                if parent_result.returncode == 0:
                    parent = parent_result.stdout.decode("ascii").strip()
                    changed = git_ops.changed_paths(root, parent, "HEAD")
                else:
                    changed = list(contracts.tasks[task_id]["owned_paths"])
            plan = plan_gates(
                contracts, task_id, args.phase, changed
            )
            result = GateExecutor(
                contracts,
                platform_label=args.platform,
                cache_enabled=not args.no_cache,
            ).execute(plan)
            result.require_success()
            artifact = _verification_artifact(
                contracts, task_id, result
            )
            if args.output is not None:
                git_ops.atomic_write_json(args.output, artifact)
            print(json.dumps(artifact, sort_keys=True))
            return 0
        if args.command == "matrix":
            task_id = args.task_id or _task_from_head(root)
            task = contracts.tasks[task_id]
            cross_platform = (
                model.RISK_RANK[task["risk"]["platform"]]
                >= model.RISK_RANK["high"]
                or model.RISK_RANK[task["risk"]["compatibility"]]
                >= model.RISK_RANK["high"]
                or model.RISK_RANK[task["risk"]["security"]]
                >= model.RISK_RANK["critical"]
            )
            include = [
                {"runner": "ubuntu-latest", "label": "linux"}
            ]
            if cross_platform:
                include.extend(
                    [
                        {"runner": "macos-latest", "label": "macos"},
                        {"runner": "windows-2022", "label": "windows"},
                    ]
                )
            print(json.dumps({"include": include}, separators=(",", ":")))
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
        if args.command == "collect-evidence":
            if args.remote is None:
                raise github_evidence.GitHubEvidenceError(
                    "evidence collection requires a GitHub remote"
                )
            store = _store(contracts, args.remote, "queue-worker")
            entry = merge_queue.MergeQueue(
                contracts, store, remote=args.remote
            ).entry_from_ref(args.task_id, args.queue_ref)
            remote_url = git_ops.git_text(
                root, "remote", "get-url", args.remote
            )
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
                required_artifacts=args.artifact or ["candidate-evidence"],
            )
            git_ops.atomic_write_json(args.output, evidence)
            print(args.output)
            return 0
        if args.command in {"publish", "recover"}:
            store = _store(contracts, args.remote, "queue-worker")
            entry = merge_queue.MergeQueue(
                contracts, store, remote=args.remote
            ).entry_from_ref(args.task_id, args.queue_ref)
            publisher = _publisher(
                contracts,
                store,
                args.remote,
                args.repository,
                args.workflow,
                args.required_job,
                args.required_artifact or ["candidate-evidence"],
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
