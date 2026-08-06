#!/usr/bin/env python3

"""Deterministic task-governance validation and record updates."""

from __future__ import annotations

import argparse
import datetime as dt
import fnmatch
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
BACKLOG = ROOT / ".agents" / "backlog.yaml"
RECORDS = ROOT / ".agents" / "records"
TASKS = ROOT / ".agents" / "tasks"
VALID_STATES = {
    "ready",
    "claimed",
    "in_progress",
    "blocked",
    "review",
    "integrated",
    "done",
}
ACTIVE_STATES = {"claimed", "in_progress", "blocked", "review", "integrated"}
SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")


class GovernanceError(RuntimeError):
    pass


def git(
    *args: str,
    cwd: Path = ROOT,
    check: bool = True,
    input_bytes: bytes | None = None,
) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        ["git", "-C", str(cwd), *args],
        check=check,
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def git_text(*args: str, cwd: Path = ROOT, check: bool = True) -> str:
    result = git(*args, cwd=cwd, check=check)
    return result.stdout.decode("utf-8", errors="strict").strip()


def load_json(path: Path) -> dict[str, Any]:
    try:
        with path.open(encoding="utf-8") as source:
            value = json.load(source)
    except (OSError, json.JSONDecodeError) as error:
        raise GovernanceError(f"Cannot read {path.relative_to(ROOT)}: {error}") from error
    if not isinstance(value, dict):
        raise GovernanceError(f"{path.relative_to(ROOT)} must contain an object")
    return value


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.write_text(
        json.dumps(value, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )


def load_backlog() -> tuple[dict[str, Any], dict[str, dict[str, Any]]]:
    document = load_json(BACKLOG)
    tasks = document.get("tasks")
    if not isinstance(tasks, list):
        raise GovernanceError(".agents/backlog.yaml must contain a tasks array")
    by_id: dict[str, dict[str, Any]] = {}
    for task in tasks:
        if not isinstance(task, dict) or not isinstance(task.get("id"), str):
            raise GovernanceError("Every backlog task must be an object with an id")
        task_id = task["id"]
        if task_id in by_id:
            raise GovernanceError(f"Duplicate task id: {task_id}")
        by_id[task_id] = task
    return document, by_id


def record_path(task_id: str) -> Path:
    return RECORDS / f"{task_id}.json"


def load_record(task_id: str) -> dict[str, Any]:
    return load_json(record_path(task_id))


def task_spec_paths(task_id: str) -> list[Path]:
    return sorted(TASKS.glob(f"{task_id}-*.md"))


def commit_exists(commit: str) -> bool:
    if not SHA_PATTERN.fullmatch(commit):
        return False
    return git("cat-file", "-e", f"{commit}^{{commit}}", check=False).returncode == 0


def stable_patch_id(commit: str) -> str:
    shown = git("show", "--pretty=format:", "--binary", commit)
    result = git("patch-id", "--stable", input_bytes=shown.stdout)
    output = result.stdout.decode("ascii").strip()
    if not output:
        raise GovernanceError(f"Commit {commit} has no patch ID")
    return output.split()[0]


def is_ancestor(ancestor: str, descendant: str) -> bool:
    return (
        git(
            "merge-base",
            "--is-ancestor",
            ancestor,
            descendant,
            check=False,
        ).returncode
        == 0
    )


def require_string(
    errors: list[str],
    value: Any,
    label: str,
    *,
    allow_empty: bool = False,
) -> str:
    if not isinstance(value, str) or (not allow_empty and not value.strip()):
        errors.append(f"{label} must be a non-empty string")
        return ""
    return value


def validate_impact(
    errors: list[str],
    task_id: str,
    name: str,
    impact: Any,
) -> None:
    label = f"{task_id}.impacts.{name}"
    if not isinstance(impact, dict):
        errors.append(f"{label} must be an object")
        return

    status = require_string(errors, impact.get("status"), f"{label}.status")
    references = impact.get("references")
    rationale = require_string(
        errors, impact.get("rationale"), f"{label}.rationale"
    )
    if "TODO" in rationale:
        errors.append(f"{label}.rationale still contains TODO")
    if not isinstance(references, list) or not all(
        isinstance(item, str) and item for item in references
    ):
        errors.append(f"{label}.references must be an array of paths")
        references = []

    if name == "adr":
        required = impact.get("required")
        if not isinstance(required, bool):
            errors.append(f"{label}.required must be boolean")
        allowed = {"accepted", "proposed", "not_required"}
        if status not in allowed:
            errors.append(f"{label}.status must be one of {sorted(allowed)}")
        if required and not references:
            errors.append(f"{label} requires at least one ADR reference")
        if required is False and status != "not_required":
            errors.append(f"{label} must use not_required when required=false")
    else:
        allowed = {"updated", "not_required"}
        if status not in allowed:
            errors.append(f"{label}.status must be one of {sorted(allowed)}")
        if status == "updated" and not references:
            errors.append(f"{label} requires a document reference when updated")

    for reference in references:
        if not (ROOT / reference).is_file():
            errors.append(f"{label} references missing file: {reference}")


def validate_record(
    task: dict[str, Any],
    record: dict[str, Any],
    records: dict[str, dict[str, Any]],
    *,
    verify_git: bool,
) -> list[str]:
    errors: list[str] = []
    task_id = task["id"]
    if record.get("schema_version") != 1:
        errors.append(f"{task_id}.schema_version must be 1")
    if record.get("id") != task_id:
        errors.append(f"{task_id}.id does not match its filename")

    state = require_string(errors, record.get("state"), f"{task_id}.state")
    if state not in VALID_STATES:
        errors.append(f"{task_id}.state must be one of {sorted(VALID_STATES)}")
    owner = require_string(errors, record.get("owner"), f"{task_id}.owner")
    if state == "ready" and owner == "unassigned":
        pass
    elif owner == "unassigned":
        errors.append(f"{task_id}.owner must be assigned outside ready state")

    base_sha = require_string(
        errors,
        record.get("base_sha"),
        f"{task_id}.base_sha",
        allow_empty=state == "ready",
    )
    head_sha = require_string(
        errors,
        record.get("head_sha"),
        f"{task_id}.head_sha",
        allow_empty=state in {"ready", "claimed", "in_progress", "blocked"},
    )
    for label, commit in (("base_sha", base_sha), ("head_sha", head_sha)):
        if commit and not SHA_PATTERN.fullmatch(commit):
            errors.append(f"{task_id}.{label} must be a full lowercase SHA")
        elif commit and verify_git and not commit_exists(commit):
            source_may_be_archived = (
                label == "head_sha" and state in {"integrated", "done"}
            )
            if not source_may_be_archived:
                errors.append(f"{task_id}.{label} commit is unavailable: {commit}")

    handoff = require_string(errors, record.get("handoff"), f"{task_id}.handoff")
    if state in {"review", "integrated", "done"} and handoff:
        if not (ROOT / handoff).is_file():
            errors.append(f"{task_id}.handoff does not exist: {handoff}")

    impacts = record.get("impacts")
    if not isinstance(impacts, dict):
        errors.append(f"{task_id}.impacts must be an object")
    else:
        for name in ("adr", "architecture", "roadmap"):
            validate_impact(errors, task_id, name, impacts.get(name))

    for dependency in task.get("depends_on", []):
        dependency_record = records.get(dependency)
        if dependency_record is None:
            errors.append(f"{task_id} has no record for dependency {dependency}")
        elif state != "ready" and dependency_record.get("state") != "done":
            errors.append(f"{task_id} depends on incomplete task {dependency}")

    verification = record.get("verification")
    acceptance = record.get("acceptance")
    integration = record.get("integration")
    if not isinstance(verification, dict):
        errors.append(f"{task_id}.verification must be an object")
        verification = {}
    if not isinstance(acceptance, dict):
        errors.append(f"{task_id}.acceptance must be an object")
        acceptance = {}
    if not isinstance(integration, dict):
        errors.append(f"{task_id}.integration must be an object")
        integration = {}

    commands = verification.get("commands")
    if not isinstance(commands, list) or not commands or not all(
        isinstance(command, str) and command for command in commands
    ):
        errors.append(f"{task_id}.verification.commands must not be empty")

    if state == "done":
        if verification.get("status") != "passed":
            errors.append(f"{task_id}.verification.status must be passed")
        require_string(
            errors,
            verification.get("reference"),
            f"{task_id}.verification.reference",
        )
        require_string(
            errors,
            acceptance.get("accepted_by"),
            f"{task_id}.acceptance.accepted_by",
        )
        accepted_at = require_string(
            errors,
            acceptance.get("accepted_at"),
            f"{task_id}.acceptance.accepted_at",
        )
        if accepted_at:
            try:
                dt.datetime.fromisoformat(accepted_at)
            except ValueError:
                errors.append(f"{task_id}.acceptance.accepted_at is not ISO-8601")

    if state in {"integrated", "done"}:
        if integration.get("strategy") not in {"cherry-pick", "merge"}:
            errors.append(f"{task_id}.integration.strategy is invalid")
        mappings = integration.get("mappings")
        if not isinstance(mappings, list) or not mappings:
            errors.append(f"{task_id}.integration.mappings must not be empty")
            mappings = []
        verified_sha = require_string(
            errors,
            integration.get("verified_sha"),
            f"{task_id}.integration.verified_sha",
            allow_empty=state == "integrated",
        )
        if verified_sha and not SHA_PATTERN.fullmatch(verified_sha):
            errors.append(
                f"{task_id}.integration.verified_sha must be a full lowercase SHA"
            )
        elif verified_sha and verify_git and not commit_exists(verified_sha):
            errors.append(
                f"{task_id}.integration.verified_sha is unavailable: {verified_sha}"
            )

        mapped_sources: set[str] = set()
        for index, mapping in enumerate(mappings):
            mapping_label = f"{task_id}.integration.mappings[{index}]"
            if not isinstance(mapping, dict):
                errors.append(f"{mapping_label} must be an object")
                continue
            source = require_string(
                errors, mapping.get("source"), f"{mapping_label}.source"
            )
            result = require_string(
                errors, mapping.get("result"), f"{mapping_label}.result"
            )
            patch_id = require_string(
                errors, mapping.get("patch_id"), f"{mapping_label}.patch_id"
            )
            for name, commit in (("source", source), ("result", result)):
                if commit and not SHA_PATTERN.fullmatch(commit):
                    errors.append(f"{mapping_label}.{name} must be a full SHA")
            if source:
                if source in mapped_sources:
                    errors.append(f"{mapping_label}.source is duplicated")
                mapped_sources.add(source)
            if not verify_git or not result or not patch_id:
                continue
            if not commit_exists(result):
                errors.append(f"{mapping_label}.result is unavailable: {result}")
                continue
            if stable_patch_id(result) != patch_id:
                errors.append(f"{mapping_label}.result patch ID does not match")
            if commit_exists(source) and stable_patch_id(source) != patch_id:
                errors.append(f"{mapping_label}.source patch ID does not match")
            if verified_sha and not is_ancestor(result, verified_sha):
                errors.append(
                    f"{mapping_label}.result is not reachable from verified_sha"
                )
        if head_sha and mappings and head_sha not in mapped_sources:
            errors.append(f"{task_id}.head_sha has no integration mapping")

    return errors


def validate_repository(*, verify_git: bool = True) -> None:
    document, tasks = load_backlog()
    errors: list[str] = []
    if document.get("schema_version") != 1:
        errors.append("backlog schema_version must be 1")

    for task_id, task in tasks.items():
        if not re.fullmatch(r"XT-[0-9]{3,}", task_id):
            errors.append(f"Invalid task id: {task_id}")
        if task.get("readiness") not in {"ready", "blocked", "done"}:
            errors.append(f"Invalid readiness for {task_id}")
        dependencies = task.get("depends_on")
        if not isinstance(dependencies, list):
            errors.append(f"{task_id}.depends_on must be an array")
            dependencies = []
        for dependency in dependencies:
            if dependency not in tasks:
                errors.append(f"{task_id} has unknown dependency {dependency}")
        paths = task.get("owned_paths")
        if not isinstance(paths, list) or not paths:
            errors.append(f"{task_id}.owned_paths must not be empty")
        specs = task_spec_paths(task_id)
        if len(specs) != 1:
            errors.append(f"{task_id} must have exactly one tracked task spec")
        elif "TODO" in specs[0].read_text(encoding="utf-8"):
            errors.append(f"{task_id} task spec still contains TODO")

    record_files = sorted(RECORDS.glob("XT-*.json"))
    record_ids = {path.stem for path in record_files}
    expected_ids = set(tasks)
    for missing in sorted(expected_ids - record_ids):
        errors.append(f"Missing task record: {missing}")
    for extra in sorted(record_ids - expected_ids):
        errors.append(f"Task record is not in backlog: {extra}")

    records: dict[str, dict[str, Any]] = {}
    for task_id in sorted(expected_ids & record_ids):
        try:
            records[task_id] = load_record(task_id)
        except GovernanceError as error:
            errors.append(str(error))

    active_owners: dict[str, str] = {}
    for task_id, record in records.items():
        owner = record.get("owner")
        if record.get("state") in ACTIVE_STATES and isinstance(owner, str):
            previous = active_owners.get(owner)
            if previous is not None:
                errors.append(f"Owner {owner} has active tasks {previous} and {task_id}")
            active_owners[owner] = task_id
        errors.extend(
            validate_record(
                tasks[task_id],
                record,
                records,
                verify_git=verify_git,
            )
        )

    if errors:
        raise GovernanceError("\n".join(f"- {error}" for error in errors))
    print(f"Governance validation passed for {len(tasks)} tasks.")


def find_worktree(task_id: str) -> Path:
    target = f"refs/heads/task/{task_id}"
    current: Path | None = None
    for line in git_text("worktree", "list", "--porcelain").splitlines():
        if line.startswith("worktree "):
            current = Path(line.removeprefix("worktree "))
        elif line == f"branch {target}" and current is not None:
            return current
    raise GovernanceError(f"{task_id} has no task worktree")


def path_allowed(path: str, patterns: list[str], task_id: str) -> bool:
    always_allowed = {
        f".agents/records/{task_id}.json",
        f".agents/handoffs/{task_id}.md",
    }
    if path in always_allowed:
        return True
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def validate_handoff(path: Path) -> None:
    try:
        content = path.read_text(encoding="utf-8")
    except OSError as error:
        raise GovernanceError(f"Cannot read handoff {path}: {error}") from error
    headings = (
        "## Delivered",
        "## Contracts",
        "## Verification evidence",
        "## Residual risk",
        "## Review focus",
        "## Acceptance",
    )
    missing = [heading for heading in headings if heading not in content]
    if missing:
        raise GovernanceError(f"Handoff is missing headings: {', '.join(missing)}")
    required_labels = (
        "Task",
        "From owner",
        "Branch",
        "Base SHA",
        "Head SHA",
        "Observable behavior",
        "Added or changed",
        "Compatibility impact",
        "ADR or protocol reference",
        "Command",
        "Result",
        "Known limitation",
        "Follow-up task",
    )
    for label in required_labels:
        match = re.search(rf"(?m)^-\s+{re.escape(label)}:\s*(.+)$", content)
        if match is None or not match.group(1).strip():
            raise GovernanceError(f"Handoff field is missing or empty: {label}")


def prepare_review(task_id: str) -> None:
    _, tasks = load_backlog()
    if task_id not in tasks:
        raise GovernanceError(f"Unknown task: {task_id}")
    record = load_record(task_id)
    if record.get("state") != "in_progress":
        raise GovernanceError(f"{task_id} must be in_progress before review")
    worktree = find_worktree(task_id)
    if git_text("status", "--porcelain", cwd=worktree):
        raise GovernanceError(f"{task_id} worktree must be clean before review")
    expected_branch = f"task/{task_id}"
    if git_text("branch", "--show-current", cwd=worktree) != expected_branch:
        raise GovernanceError(f"{task_id} worktree is not on {expected_branch}")

    base_sha = record.get("base_sha", "")
    head_sha = git_text("rev-parse", "HEAD", cwd=worktree)
    changed = git_text("diff", "--name-only", f"{base_sha}..{head_sha}", cwd=worktree)
    patterns = tasks[task_id]["owned_paths"]
    outside = [
        path
        for path in changed.splitlines()
        if path and not path_allowed(path, patterns, task_id)
    ]
    if outside:
        raise GovernanceError(
            "Changed paths outside task ownership:\n"
            + "\n".join(f"- {path}" for path in outside)
        )

    handoff = worktree / record["handoff"]
    if not handoff.is_file():
        raise GovernanceError(f"Handoff does not exist: {record['handoff']}")
    validate_handoff(handoff)
    print(head_sha)


def update_state(task_id: str, expected: str, target: str) -> None:
    record = load_record(task_id)
    current = record.get("state")
    if current != expected:
        raise GovernanceError(f"{task_id} record state is {current}, expected {expected}")
    record["state"] = target
    if expected == "review" and target == "in_progress":
        record["head_sha"] = ""
        record["verification"]["status"] = "pending"
        record["verification"]["reference"] = ""
    write_json(record_path(task_id), record)


def mark_claimed(task_id: str, owner: str, base_sha: str) -> None:
    record = load_record(task_id)
    if record.get("state") != "ready":
        raise GovernanceError(f"{task_id} must be ready before claim")
    if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9._-]*", owner):
        raise GovernanceError("Owner must be a filesystem-safe slug")
    if not SHA_PATTERN.fullmatch(base_sha) or not commit_exists(base_sha):
        raise GovernanceError("Claim base must be an available full commit SHA")
    record["state"] = "claimed"
    record["owner"] = owner
    record["base_sha"] = base_sha
    write_json(record_path(task_id), record)


def mark_review(task_id: str, head_sha: str, reference: str) -> None:
    record = load_record(task_id)
    if record.get("state") != "in_progress":
        raise GovernanceError(f"{task_id} must be in_progress before review")
    record["state"] = "review"
    record["head_sha"] = head_sha
    record["verification"]["status"] = "passed"
    record["verification"]["reference"] = reference
    write_json(record_path(task_id), record)


def mark_integrated(task_id: str, mappings_path: Path) -> None:
    record = load_record(task_id)
    if record.get("state") != "review":
        raise GovernanceError(f"{task_id} must be in review before integration")
    mappings_value = load_json(mappings_path)
    mappings = mappings_value.get("mappings")
    if not isinstance(mappings, list) or not mappings:
        raise GovernanceError("Integration mapping file has no mappings")
    record["state"] = "integrated"
    record["integration"] = {
        "strategy": "cherry-pick",
        "mappings": mappings,
        "verified_sha": "",
    }
    write_json(record_path(task_id), record)


def mark_accepted(task_id: str, reviewer: str, reference: str, verified_sha: str) -> None:
    record = load_record(task_id)
    if record.get("state") != "integrated":
        raise GovernanceError(f"{task_id} must be integrated before acceptance")
    record["state"] = "done"
    record["integration"]["verified_sha"] = verified_sha
    record["verification"]["status"] = "passed"
    record["verification"]["reference"] = reference
    record["acceptance"] = {
        "accepted_by": reviewer,
        "accepted_at": dt.datetime.now(dt.timezone.utc).astimezone().isoformat(
            timespec="seconds"
        ),
        "note": "Accepted by the integration owner after repository verification.",
    }
    write_json(record_path(task_id), record)


def get_field(task_id: str, field: str) -> None:
    value: Any = load_record(task_id)
    for component in field.split("."):
        if not isinstance(value, dict) or component not in value:
            raise GovernanceError(f"Unknown record field: {field}")
        value = value[component]
    if isinstance(value, (dict, list)):
        print(json.dumps(value, separators=(",", ":")))
    else:
        print(value)


def verification_commands(task_id: str) -> None:
    record = load_record(task_id)
    commands = record.get("verification", {}).get("commands", [])
    if not isinstance(commands, list):
        raise GovernanceError(f"{task_id} has invalid verification commands")
    for command in commands:
        print(command)


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate_parser = subparsers.add_parser("validate")
    validate_parser.add_argument("--no-git", action="store_true")

    review_parser = subparsers.add_parser("prepare-review")
    review_parser.add_argument("task_id")

    update_parser = subparsers.add_parser("update-state")
    update_parser.add_argument("task_id")
    update_parser.add_argument("expected")
    update_parser.add_argument("target")

    claimed_parser = subparsers.add_parser("mark-claimed")
    claimed_parser.add_argument("task_id")
    claimed_parser.add_argument("owner")
    claimed_parser.add_argument("base_sha")

    mark_review_parser = subparsers.add_parser("mark-review")
    mark_review_parser.add_argument("task_id")
    mark_review_parser.add_argument("head_sha")
    mark_review_parser.add_argument("reference")

    integrated_parser = subparsers.add_parser("mark-integrated")
    integrated_parser.add_argument("task_id")
    integrated_parser.add_argument("mappings_path", type=Path)

    accepted_parser = subparsers.add_parser("mark-accepted")
    accepted_parser.add_argument("task_id")
    accepted_parser.add_argument("reviewer")
    accepted_parser.add_argument("reference")
    accepted_parser.add_argument("verified_sha")

    get_parser = subparsers.add_parser("get")
    get_parser.add_argument("task_id")
    get_parser.add_argument("field")

    commands_parser = subparsers.add_parser("verification-commands")
    commands_parser.add_argument("task_id")

    args = parser.parse_args()
    try:
        if args.command == "validate":
            validate_repository(verify_git=not args.no_git)
        elif args.command == "prepare-review":
            prepare_review(args.task_id)
        elif args.command == "update-state":
            update_state(args.task_id, args.expected, args.target)
        elif args.command == "mark-claimed":
            mark_claimed(args.task_id, args.owner, args.base_sha)
        elif args.command == "mark-review":
            mark_review(args.task_id, args.head_sha, args.reference)
        elif args.command == "mark-integrated":
            mark_integrated(args.task_id, args.mappings_path)
        elif args.command == "mark-accepted":
            mark_accepted(
                args.task_id,
                args.reviewer,
                args.reference,
                args.verified_sha,
            )
        elif args.command == "get":
            get_field(args.task_id, args.field)
        elif args.command == "verification-commands":
            verification_commands(args.task_id)
        else:
            parser.error("unknown command")
    except GovernanceError as error:
        print(f"Governance error:\n{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
