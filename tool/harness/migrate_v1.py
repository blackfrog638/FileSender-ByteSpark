#!/usr/bin/env python3
"""Generate a read-only Harness V1 migration snapshot from an archive ref."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

import git_ops


class MigrationError(RuntimeError):
    """Raised when a legacy record cannot be represented safely."""


def _legacy_records(root: Path, source_ref: str) -> List[str]:
    output = git_ops.git_text(
        root,
        "ls-tree",
        "-r",
        "--name-only",
        source_ref,
        ".agents/records",
    )
    return [
        path
        for path in output.splitlines()
        if re.fullmatch(r"\.agents/records/XT-[0-9]{3,}\.json", path)
    ]


def _record(root: Path, source_ref: str, path: str) -> Dict[str, Any]:
    result = git_ops.run_git(
        root, "show", "{}:{}".format(source_ref, path)
    )
    try:
        value = json.loads(result.stdout.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise MigrationError("{} is invalid JSON".format(path)) from error
    if not isinstance(value, dict):
        raise MigrationError("{} must contain an object".format(path))
    return value


def generate(
    root: Path,
    source_ref: str,
    created_by: str,
    created_at: str,
) -> Dict[str, Any]:
    source_head = git_ops.object_id(root, source_ref)
    accepted = []
    deferred = []
    seen = set()
    for path in _legacy_records(root, source_ref):
        record = _record(root, source_ref, path)
        task_id = record.get("id")
        if not isinstance(task_id, str) or task_id in seen:
            raise MigrationError("{} has invalid or duplicate task id".format(path))
        seen.add(task_id)
        state = record.get("state")
        if state == "done":
            integration = record.get("integration")
            if not isinstance(integration, dict):
                raise MigrationError("{} done record lacks integration".format(task_id))
            delivery = integration.get("verified_sha") or integration.get("result")
            if not isinstance(delivery, str) or len(delivery) != 40:
                raise MigrationError("{} delivery SHA is invalid".format(task_id))
            acceptance = git_ops.git_text(
                root,
                "log",
                "-1",
                "--format=%H",
                source_ref,
                "--",
                path,
            )
            if len(acceptance) != 40:
                raise MigrationError(
                    "{} acceptance commit is unavailable".format(task_id)
                )
            accepted.append(
                {
                    "task_id": task_id,
                    "legacy_record_blob": git_ops.object_id(
                        root, "{}:{}".format(source_ref, path)
                    ),
                    "legacy_acceptance_sha": acceptance,
                    "delivery_sha": delivery,
                }
            )
        else:
            task_archive = "refs/heads/archive/harness-v1/task/{}".format(
                task_id
            )
            if git_ops.ref_sha(root, task_archive) is None:
                task_archive = "refs/heads/archive/harness-v1/final-local"
            deferred.append(
                {
                    "task_id": task_id,
                    "legacy_state": str(state),
                    "archive_ref": task_archive,
                }
            )
    return {
        "schema_version": 1,
        "source_ref": source_ref,
        "source_head": source_head,
        "accepted_tasks": accepted,
        "deferred_tasks": deferred,
        "created_by": created_by,
        "created_at": created_at,
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument(
        "--source-ref",
        default="refs/heads/archive/harness-v1/final-local",
    )
    parser.add_argument("--created-by", required=True)
    parser.add_argument("--created-at", required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _parser().parse_args(argv)
    try:
        snapshot = generate(
            args.root.resolve(),
            args.source_ref,
            args.created_by,
            args.created_at,
        )
        output = args.output
        if not output.is_absolute():
            output = args.root / output
        git_ops.atomic_write_json(output, snapshot)
        print(
            "accepted={} deferred={}".format(
                len(snapshot["accepted_tasks"]),
                len(snapshot["deferred_tasks"]),
            )
        )
        return 0
    except (MigrationError, git_ops.GitError) as error:
        print("Harness V1 migration error:\n{}".format(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
