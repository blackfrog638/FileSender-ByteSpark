#!/usr/bin/env python3

"""Detect active ownership conflicts and path-relevant stale task bases."""

from __future__ import annotations

import argparse
import fnmatch
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
ACTIVE_STATES = {"claimed", "in_progress", "blocked", "review", "integrated"}
MAGIC_PATTERN = re.compile(r"[*?\[]")
GLOBAL_STALE_PATTERNS = (
    "AGENTS.md",
    ".agents/manifest.yaml",
    ".agents/architecture/modules.json",
    ".agents/commit-identity.json",
    "tool/harness/**",
    ".githooks/**",
    ".github/workflows/**",
    "Makefile",
    "docs/commit-policy.md",
)


class TaskConflictError(RuntimeError):
    pass


def git(
    root: Path,
    *args: str,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(root), *args],
        check=check,
        capture_output=True,
        text=True,
    )


def git_text(root: Path, *args: str) -> str:
    return git(root, *args).stdout.strip()


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise TaskConflictError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise TaskConflictError(f"{path} must contain an object")
    return value


def load_tasks(root: Path) -> dict[str, dict[str, Any]]:
    document = load_json(root / ".agents" / "backlog.yaml")
    raw_tasks = document.get("tasks")
    if not isinstance(raw_tasks, list):
        raise TaskConflictError("backlog tasks must be an array")
    tasks: dict[str, dict[str, Any]] = {}
    for task in raw_tasks:
        if isinstance(task, dict) and isinstance(task.get("id"), str):
            tasks[task["id"]] = task
    return tasks


def load_record(root: Path, task_id: str) -> dict[str, Any]:
    return load_json(root / ".agents" / "records" / f"{task_id}.json")


def branch_record(root: Path, task_id: str) -> dict[str, Any] | None:
    result = git(
        root,
        "show",
        f"task/{task_id}:.agents/records/{task_id}.json",
        check=False,
    )
    if result.returncode != 0:
        return None
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise TaskConflictError(
            f"task/{task_id} contains an invalid task record: {error}"
        ) from error
    if not isinstance(value, dict):
        raise TaskConflictError(f"task/{task_id} record must be an object")
    return value


def effective_record(root: Path, task_id: str) -> dict[str, Any]:
    durable = load_record(root, task_id)
    if durable.get("state") in {"integrated", "done"}:
        return durable
    in_flight = branch_record(root, task_id)
    return in_flight if in_flight is not None else durable


def has_magic(pattern: str) -> bool:
    return MAGIC_PATTERN.search(pattern) is not None


def literal_prefix(pattern: str) -> tuple[str, ...]:
    prefix: list[str] = []
    for part in pattern.split("/"):
        if has_magic(part):
            break
        prefix.append(part)
    return tuple(prefix)


def prefixes_intersect(left: tuple[str, ...], right: tuple[str, ...]) -> bool:
    shared = min(len(left), len(right))
    return left[:shared] == right[:shared]


def segment_witness(segment: str) -> str:
    value = re.sub(r"\[!?(.)[^\]]*\]", r"\1", segment)
    value = value.replace("*", "x").replace("?", "x")
    return value or "x"


def pattern_witnesses(pattern: str) -> set[str]:
    candidates = [""]
    for segment in pattern.split("/"):
        if segment == "**":
            candidates = candidates + [
                f"{candidate}/x".strip("/") for candidate in candidates
            ]
            continue
        witness = segment_witness(segment)
        candidates = [
            f"{candidate}/{witness}".strip("/") for candidate in candidates
        ]
    return set(candidates)


def patterns_overlap(left: str, right: str) -> bool:
    left_magic = has_magic(left)
    right_magic = has_magic(right)
    if not left_magic and not right_magic:
        return left == right
    if not left_magic:
        return fnmatch.fnmatchcase(left, right)
    if not right_magic:
        return fnmatch.fnmatchcase(right, left)
    for witness in pattern_witnesses(left) | pattern_witnesses(right):
        if fnmatch.fnmatchcase(witness, left) and fnmatch.fnmatchcase(
            witness, right
        ):
            return True
    return prefixes_intersect(literal_prefix(left), literal_prefix(right))


def owned_paths(task: dict[str, Any], task_id: str) -> list[str]:
    value = task.get("owned_paths")
    if not isinstance(value, list) or not all(
        isinstance(path, str) and path for path in value
    ):
        raise TaskConflictError(f"{task_id}.owned_paths must be string paths")
    return value


def claim_conflicts(
    root: Path,
    task_id: str,
) -> list[tuple[str, str, str]]:
    tasks = load_tasks(root)
    target = tasks.get(task_id)
    if target is None:
        raise TaskConflictError(f"unknown task: {task_id}")
    target_paths = owned_paths(target, task_id)
    conflicts: list[tuple[str, str, str]] = []
    for other_id, other in sorted(tasks.items()):
        if other_id == task_id:
            continue
        record = effective_record(root, other_id)
        if record.get("state") not in ACTIVE_STATES:
            continue
        for target_path in target_paths:
            for other_path in owned_paths(other, other_id):
                if patterns_overlap(target_path, other_path):
                    conflicts.append((other_id, target_path, other_path))
    return conflicts


def check_claim(root: Path, task_id: str) -> None:
    conflicts = claim_conflicts(root, task_id)
    if conflicts:
        details = "\n".join(
            f"- {other}: {target_pattern} <-> {other_pattern}"
            for other, target_pattern, other_pattern in conflicts
        )
        raise TaskConflictError(
            f"{task_id} ownership conflicts with active tasks:\n{details}"
        )
    print(f"{task_id} has no active ownership conflicts.")


def matching_changed_paths(
    changed_paths: list[str],
    patterns: list[str],
) -> list[str]:
    return sorted(
        {
            path
            for path in changed_paths
            if any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)
        }
    )


def check_stale_base(
    root: Path,
    task_id: str,
    base: str,
    upstream: str,
) -> list[str]:
    tasks = load_tasks(root)
    task = tasks.get(task_id)
    if task is None:
        raise TaskConflictError(f"unknown task: {task_id}")
    base_result = git(root, "rev-parse", "--verify", f"{base}^{{commit}}", check=False)
    upstream_result = git(
        root,
        "rev-parse",
        "--verify",
        f"{upstream}^{{commit}}",
        check=False,
    )
    if base_result.returncode != 0:
        raise TaskConflictError(f"{task_id} base is unavailable: {base}")
    if upstream_result.returncode != 0:
        raise TaskConflictError(f"integration ref is unavailable: {upstream}")
    base_sha = base_result.stdout.strip()
    upstream_sha = upstream_result.stdout.strip()
    if (
        git(
            root,
            "merge-base",
            "--is-ancestor",
            base_sha,
            upstream_sha,
            check=False,
        ).returncode
        != 0
    ):
        raise TaskConflictError(
            f"{task_id} base {base_sha} has diverged from {upstream_sha}; "
            "rebase onto integration and update base_sha"
        )
    if base_sha == upstream_sha:
        print(f"{task_id} base is current at {base_sha}.")
        return []
    changed = git_text(
        root,
        "diff",
        "--name-only",
        "--no-renames",
        f"{base_sha}..{upstream_sha}",
        "--",
    ).splitlines()
    relevant = matching_changed_paths(
        changed,
        owned_paths(task, task_id) + list(GLOBAL_STALE_PATTERNS),
    )
    if relevant:
        details = "\n".join(f"- {path}" for path in relevant)
        raise TaskConflictError(
            f"{task_id} base is stale against {upstream_sha}; "
            f"relevant upstream paths changed:\n{details}\n"
            "Rebase onto integration, update base_sha, and repeat review."
        )
    print(
        f"{task_id} base is behind {upstream_sha}, "
        "but upstream paths are unrelated."
    )
    return []


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT)
    subparsers = parser.add_subparsers(dest="command", required=True)
    claim_parser = subparsers.add_parser("claim")
    claim_parser.add_argument("task_id")
    stale_parser = subparsers.add_parser("stale")
    stale_parser.add_argument("task_id")
    stale_parser.add_argument("base")
    stale_parser.add_argument("upstream")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        if args.command == "claim":
            check_claim(root, args.task_id)
        else:
            check_stale_base(root, args.task_id, args.base, args.upstream)
    except TaskConflictError as error:
        print(f"Task conflict error:\n{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
