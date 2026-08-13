#!/usr/bin/env python3
"""Minimal Harness V2 command-line entrypoint."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Sequence

import ci_validation
import cleanup
import delivery
import git_ops
import github_evidence
import model
import runtime
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
    ci_validation.CIValidationError,
    cleanup.CleanupError,
    delivery.DeliveryError,
    GateExecutionError,
    GatePlanError,
    github_evidence.GitHubEvidenceError,
    git_ops.GitError,
    model.ContractError,
    runtime.RuntimeViewError,
    tdd.TddError,
    workspace.WorkspaceError,
)


def _task_from_head(root: Path) -> str:
    message = git_ops.git_text(root, "show", "-s", "--format=%B", "HEAD")
    tasks = [
        line.removeprefix("Xnn-Task: ")
        for line in message.splitlines()
        if line.startswith("Xnn-Task: ")
    ]
    if len(tasks) != 1 or model.TASK_ID.fullmatch(tasks[0]) is None:
        raise delivery.DeliveryError(
            "HEAD must contain exactly one valid Xnn-Task trailer"
        )
    return tasks[0]


def _candidate_task_ids(
    contracts: model.ContractSet,
    remote: Optional[str],
    head: str = "HEAD",
) -> tuple[List[str], str]:
    view = runtime.RuntimeView(contracts, remote)
    base = view.integration_sha()
    candidate = git_ops.object_id(contracts.root, head)
    if candidate == base or not git_ops.is_ancestor(contracts.root, base, candidate):
        raise delivery.DeliveryError(
            "candidate HEAD is not ahead of integration branch {}".format(
                contracts.manifest["integration_branch"]
            )
        )
    task_ids: List[str] = []
    for commit in git_ops.commit_range(contracts.root, base, candidate):
        message = git_ops.git_text(
            contracts.root,
            "show",
            "-s",
            "--format=%B",
            commit,
        )
        tasks = [
            line.removeprefix("Xnn-Task: ")
            for line in message.splitlines()
            if line.startswith("Xnn-Task: ")
        ]
        lifecycle = [
            line.removeprefix("Xnn-Lifecycle: ")
            for line in message.splitlines()
            if line.startswith("Xnn-Lifecycle: ")
        ]
        if (
            len(tasks) != 1
            or tasks[0] not in contracts.tasks
            or lifecycle != ["delivery"]
        ):
            raise delivery.DeliveryError(
                "{} candidate commit has invalid task provenance".format(commit[:12])
            )
        task_ids.append(tasks[0])
    if len(task_ids) != len(set(task_ids)):
        raise delivery.DeliveryError("candidate contains duplicate task deliveries")
    return task_ids, base


def _verification_artifact(
    contracts: model.ContractSet,
    task_ids: Sequence[str],
    result: Any,
) -> Dict[str, Any]:
    gate_digests = [
        model.canonical_sha256(dict(item.attestation)) for item in result.results
    ]
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
    return {
        "schema_version": 1,
        "source_sha": git_ops.object_id(contracts.root, "HEAD"),
        "platform": platforms.pop(),
        "gate_ids": [item.gate_id for item in result.results],
        "gate_attestations": gate_digests,
        "criterion_ids": criterion_ids,
        "criterion_evidence": [
            ci_validation.criterion_evidence_digest(
                contracts,
                criterion_id,
                git_ops.object_id(contracts.root, "HEAD"),
                gate_digests,
            )
            for criterion_id in criterion_ids
        ],
    }


def _bootstrap_verification_artifact(
    contracts: model.ContractSet,
    result: Any,
    plan_digest: str,
) -> Dict[str, Any]:
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
        "gate_attestations": [
            model.canonical_sha256(dict(item.attestation)) for item in result.results
        ],
        "skipped": any(item.attestation["skipped"] for item in result.results),
    }


def _platform_matrix(
    contracts: model.ContractSet,
    task_ids: Sequence[str],
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


def _red_map(values: Sequence[str]) -> Mapping[str, str]:
    result: Dict[str, str] = {}
    for value in values:
        task_id, separator, red_sha = value.partition("=")
        if (
            separator != "="
            or model.TASK_ID.fullmatch(task_id) is None
            or len(red_sha) != 40
            or task_id in result
        ):
            raise delivery.DeliveryError(
                "--red must use unique XT-NNN=<40-char-sha> values"
            )
        result[task_id] = red_sha
    return result


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--remote", default="origin")
    parser.add_argument("--local", action="store_const", const=None, dest="remote")
    commands = parser.add_subparsers(dest="command", required=True)

    commands.add_parser("validate")
    commands.add_parser("list")
    branch_gc = commands.add_parser("branch-gc")
    branch_gc.add_argument("--execute", action="store_true")

    claim = commands.add_parser("claim")
    claim.add_argument("task_id")
    claim.add_argument("--path", type=Path)
    recover_claim = commands.add_parser("claim-recover")
    recover_claim.add_argument("task_id")

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
    submit.add_argument("--train-id", required=True)
    submit.add_argument("--red-sha")
    build = commands.add_parser("queue-build")
    build.add_argument("task_ids", nargs="+")
    build.add_argument("--train-id", required=True)
    build.add_argument("--red", action="append", default=[])
    build.add_argument("--base")
    reopen = commands.add_parser("queue-reopen")
    reopen.add_argument("task_id")
    reopen.add_argument("queue_ref")
    reopen.add_argument("--reason", required=True)
    drop = commands.add_parser("queue-drop")
    drop.add_argument("task_id")
    drop.add_argument("queue_ref")
    drop.add_argument("--reason", required=True)

    publish = commands.add_parser("publish")
    publish.add_argument("task_id")
    publish.add_argument("queue_ref")
    publish.add_argument("--repository")
    publish.add_argument("--workflow", default=".github/workflows/merge-queue.yml")
    recover = commands.add_parser("recover")
    recover.add_argument("task_id")
    recover.add_argument("queue_ref")
    recover.add_argument("--repository")
    recover.add_argument("--workflow", default=".github/workflows/merge-queue.yml")
    bootstrap_publish = commands.add_parser("bootstrap-publish")
    bootstrap_publish.add_argument("queue_ref")
    bootstrap_publish.add_argument("--repository")
    bootstrap_publish.add_argument(
        "--workflow",
        default=".github/workflows/merge-queue.yml",
    )
    return parser


def _repository(
    root: Path,
    remote: Optional[str],
    explicit: Optional[str],
) -> str:
    if explicit is not None:
        return explicit
    if remote is None:
        raise delivery.DeliveryError("publication requires a GitHub remote")
    remote_url = git_ops.git_text(root, "remote", "get-url", remote)
    return github_evidence.repository_slug(remote_url)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = _parser()
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        contracts = model.load_contracts(root)
        view = runtime.RuntimeView(contracts, args.remote)
        workspaces = workspace.WorkspaceManager(contracts, view)
        queue = delivery.QueueManager(
            contracts,
            view,
            workspaces,
            args.remote,
        )
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
            for task_id, snapshot in view.list().items():
                print(
                    "{}\t{}\t{}".format(
                        task_id,
                        snapshot.state,
                        contracts.tasks[task_id]["title"],
                    )
                )
            return 0
        if args.command == "branch-gc":
            value = cleanup.BranchCleanup(
                contracts,
                view,
                args.remote,
            ).run(execute=args.execute)
            print(json.dumps(value, sort_keys=True))
            return 0
        if args.command == "claim":
            claimed = workspaces.claim(args.task_id, args.path)
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
            claimed = workspaces.recover_claim(args.task_id)
            if claimed is None:
                print("{} returned to ready".format(args.task_id))
            else:
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
            value = tdd.TddManager(contracts, workspaces).record_red(args.task_id)
            print(
                json.dumps(
                    {
                        "task_id": args.task_id,
                        "red_sha": value["red_sha"],
                        "failure_fingerprint": value["failure_fingerprint"],
                    },
                    sort_keys=True,
                )
            )
            return 0
        if args.command == "verify":
            candidate_base = None
            if args.task_id is None and args.phase == "queue":
                task_ids, candidate_base = _candidate_task_ids(
                    contracts,
                    args.remote,
                )
            else:
                task_ids = [args.task_id or _task_from_head(root)]
            changed = None
            if candidate_base is not None:
                changed = git_ops.changed_paths(root, candidate_base, "HEAD")
            elif args.phase in {"review", "queue"}:
                parent = git_ops.run_git(
                    root,
                    "rev-parse",
                    "HEAD^",
                    check=False,
                )
                if parent.returncode == 0:
                    changed = git_ops.changed_paths(
                        root,
                        parent.stdout.decode("ascii").strip(),
                        "HEAD",
                    )
            if len(task_ids) == 1:
                plan = plan_gates(
                    contracts,
                    task_ids[0],
                    args.phase,
                    changed,
                )
            else:
                plan = plan_task_set(
                    contracts,
                    task_ids,
                    args.phase,
                    changed,
                )
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
                contracts,
                result,
                global_plan.digest,
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
            red_shas = {args.task_id: args.red_sha} if args.red_sha is not None else {}
            train = queue.build_train(
                [args.task_id],
                args.train_id,
                red_shas=red_shas,
            )
            entry = train.entries[0]
            print(
                json.dumps(
                    {
                        "task_id": entry.task_id,
                        "queue_ref": entry.queue_ref,
                        "candidate_sha": entry.candidate_sha,
                    },
                    sort_keys=True,
                )
            )
            return 0
        if args.command == "queue-build":
            train = queue.build_train(
                args.task_ids,
                args.train_id,
                red_shas=_red_map(args.red),
                base_sha=args.base,
            )
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
        if args.command in {"queue-reopen", "queue-drop"}:
            if not args.reason.strip():
                raise delivery.DeliveryError("queue reason must not be empty")
            entry = queue.entry_from_ref(args.task_id, args.queue_ref)
            if args.command == "queue-reopen":
                print(queue.reopen(entry))
            else:
                queue.drop(entry)
                print("{} dropped".format(args.queue_ref))
            return 0
        if args.command in {"publish", "recover"}:
            if args.remote is None:
                raise delivery.DeliveryError(
                    "publication requires an authoritative remote"
                )
            entry = queue.entry_from_ref(args.task_id, args.queue_ref)
            publisher = delivery.Publisher(
                contracts,
                view,
                args.remote,
                _repository(root, args.remote, args.repository),
                args.workflow,
            )
            if args.command == "recover":
                publisher.recover_cleanup(entry)
                print("{} cleanup recovered".format(args.queue_ref))
            else:
                print(json.dumps(publisher.publish(entry), sort_keys=True))
            return 0
        if args.command == "bootstrap-publish":
            if args.remote is None:
                raise delivery.DeliveryError(
                    "bootstrap publication requires an authoritative remote"
                )
            publisher = delivery.Publisher(
                contracts,
                view,
                args.remote,
                _repository(root, args.remote, args.repository),
                args.workflow,
            )
            print(
                json.dumps(
                    publisher.publish_bootstrap(args.queue_ref),
                    sort_keys=True,
                )
            )
            return 0
    except ERRORS as error:
        parser.error(str(error))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
