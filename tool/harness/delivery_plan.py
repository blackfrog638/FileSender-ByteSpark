#!/usr/bin/env python3

"""Validate reviewed roadmap-to-task Delivery Plans."""

from __future__ import annotations

import argparse
import copy
import datetime as dt
import hashlib
import json
import os
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

sys.dont_write_bytecode = True

PLAN_ID_PATTERN = re.compile(r"^DP-[A-Z0-9]+(?:-[A-Z0-9]+)*$")
REQUIREMENT_ID_PATTERN = re.compile(r"^REQ-[A-Z0-9]+(?:-[A-Z0-9]+)*$")
SOURCE_REF_PATTERN = re.compile(r"^[A-Z][A-Z0-9]+(?:-[A-Z0-9]+)*$")
TASK_ID_PATTERN = re.compile(r"^XT-[0-9]{3,}$")
OWNER_PATTERN = re.compile(r"^[a-zA-Z0-9][a-zA-Z0-9._-]*$")
PLAN_STATUSES = {"draft", "approved", "superseded"}
SOURCE_KINDS = {"roadmap", "governance"}
DELIVERY_ROLES = {
    "implementation",
    "acceptance",
    "implementation_acceptance",
}
ACTIVE_TASK_STATES = {"claimed", "in_progress", "review", "integrated"}
PLAN_FIELDS = {
    "schema_version",
    "id",
    "title",
    "status",
    "source",
    "requirements",
    "approval",
    "superseded_by",
}
SOURCE_FIELDS = {"kind", "path"}
REQUIREMENT_FIELDS = {
    "id",
    "source_ref",
    "statement",
    "acceptance_criteria",
    "implementation_tasks",
    "acceptance_task",
}
APPROVAL_FIELDS = {"approved_by", "approved_at", "content_sha256"}


class DeliveryPlanError(RuntimeError):
    pass


@dataclass(frozen=True)
class DeliveryPlanConfig:
    directory: Path
    required_from_task: str
    required_from_number: int


def _relative(path: Path, root: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return str(path)


def _load_json(path: Path, root: Path) -> dict[str, Any]:
    try:
        with path.open(encoding="utf-8") as source:
            value = json.load(source)
    except (OSError, json.JSONDecodeError) as error:
        raise DeliveryPlanError(
            f"Cannot read {_relative(path, root)}: {error}"
        ) from error
    if not isinstance(value, dict):
        raise DeliveryPlanError(f"{_relative(path, root)} must contain an object")
    return value


def _write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )


def load_config(root: Path) -> DeliveryPlanConfig:
    manifest = root / ".agents" / "manifest.yaml"
    try:
        lines = manifest.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise DeliveryPlanError(f"Cannot read .agents/manifest.yaml: {error}") from error

    values: dict[str, str] = {}
    in_section = False
    for line in lines:
        if line == "delivery_plans:":
            in_section = True
            continue
        if in_section and line and not line.startswith(" "):
            break
        if not in_section:
            continue
        match = re.fullmatch(r"  ([a-z_]+):\s*(\S+)", line)
        if match:
            values[match.group(1)] = match.group(2)

    directory = values.get("directory")
    required_from = values.get("required_from_task")
    if not directory or not required_from:
        raise DeliveryPlanError(
            ".agents/manifest.yaml delivery_plans must define directory "
            "and required_from_task"
        )
    if not TASK_ID_PATTERN.fullmatch(required_from):
        raise DeliveryPlanError(
            "delivery_plans.required_from_task must match XT-NNN"
        )
    return DeliveryPlanConfig(
        directory=root / directory,
        required_from_task=required_from,
        required_from_number=int(required_from.removeprefix("XT-")),
    )


def load_backlog(root: Path) -> tuple[dict[str, Any], dict[str, dict[str, Any]]]:
    document = _load_json(root / ".agents" / "backlog.yaml", root)
    raw_tasks = document.get("tasks")
    if not isinstance(raw_tasks, list):
        raise DeliveryPlanError(".agents/backlog.yaml tasks must be an array")
    tasks: dict[str, dict[str, Any]] = {}
    for task in raw_tasks:
        if not isinstance(task, dict) or not isinstance(task.get("id"), str):
            raise DeliveryPlanError("Every backlog task must be an object with an id")
        task_id = task["id"]
        if task_id in tasks:
            raise DeliveryPlanError(f"Duplicate task id: {task_id}")
        tasks[task_id] = task
    return document, tasks


def load_plans(
    root: Path,
    config: DeliveryPlanConfig,
) -> tuple[dict[str, dict[str, Any]], dict[str, Path], list[str]]:
    plans: dict[str, dict[str, Any]] = {}
    paths: dict[str, Path] = {}
    errors: list[str] = []
    if not config.directory.is_dir():
        return plans, paths, [
            f"Delivery Plan directory does not exist: "
            f"{_relative(config.directory, root)}"
        ]
    for path in sorted(config.directory.glob("*.json")):
        try:
            plan = _load_json(path, root)
        except DeliveryPlanError as error:
            errors.append(str(error))
            continue
        plan_id = plan.get("id")
        if not isinstance(plan_id, str):
            errors.append(f"{_relative(path, root)}.id must be a string")
            continue
        if plan_id in plans:
            errors.append(f"Duplicate Delivery Plan id: {plan_id}")
            continue
        plans[plan_id] = plan
        paths[plan_id] = path
    return plans, paths, errors


def _parse_scalar(value: str) -> Any:
    if value == "[]":
        return []
    if value in {"true", "false"}:
        return value == "true"
    return value


def parse_task_spec(path: Path, root: Path) -> dict[str, Any]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise DeliveryPlanError(
            f"Cannot read {_relative(path, root)}: {error}"
        ) from error
    if not lines or lines[0] != "---":
        raise DeliveryPlanError(
            f"{_relative(path, root)} must start with YAML front matter"
        )
    try:
        end = lines.index("---", 1)
    except ValueError as error:
        raise DeliveryPlanError(
            f"{_relative(path, root)} has unterminated front matter"
        ) from error

    result: dict[str, Any] = {}
    current_list: str | None = None
    for line in lines[1:end]:
        item = re.fullmatch(r"  - (.+)", line)
        if item and current_list is not None:
            result[current_list].append(item.group(1))
            continue
        field = re.fullmatch(r"([a-z_]+):(.*)", line)
        if not field:
            continue
        key = field.group(1)
        raw_value = field.group(2).strip()
        if raw_value:
            result[key] = _parse_scalar(raw_value)
            current_list = None
        else:
            result[key] = []
            current_list = key
    return result


def _spec_for_task(root: Path, task_id: str) -> tuple[Path | None, list[str]]:
    paths = sorted((root / ".agents" / "tasks").glob(f"{task_id}-*.md"))
    if len(paths) != 1:
        return None, [f"{task_id} must have exactly one task specification"]
    return paths[0], []


def _require_nonempty_string(
    errors: list[str],
    label: str,
    value: Any,
) -> str | None:
    if not isinstance(value, str) or not value.strip():
        errors.append(f"{label} must be a nonempty string")
        return None
    return value


def _require_string_list(
    errors: list[str],
    label: str,
    value: Any,
    *,
    nonempty: bool,
) -> list[str]:
    if not isinstance(value, list) or not all(
        isinstance(item, str) and item.strip() for item in value
    ):
        errors.append(f"{label} must be an array of nonempty strings")
        return []
    if nonempty and not value:
        errors.append(f"{label} must not be empty")
    if len(value) != len(set(value)):
        errors.append(f"{label} must not contain duplicates")
    return value


def _valid_timestamp(value: Any) -> bool:
    if not isinstance(value, str) or not value:
        return False
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return False
    return parsed.tzinfo is not None


def approval_digest(plan: dict[str, Any]) -> str:
    semantic = {
        key: value
        for key, value in plan.items()
        if key not in {"status", "approval", "superseded_by"}
    }
    encoded = json.dumps(
        semantic,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _reject_unknown_fields(
    errors: list[str],
    label: str,
    value: dict[str, Any],
    allowed: set[str],
) -> None:
    unknown = sorted(set(value) - allowed)
    if unknown:
        errors.append(f"{label} has unknown fields: {', '.join(unknown)}")


def _find_cycle(tasks: dict[str, dict[str, Any]]) -> list[str] | None:
    state: dict[str, int] = {}
    stack: list[str] = []
    positions: dict[str, int] = {}

    def visit(task_id: str) -> list[str] | None:
        state[task_id] = 1
        positions[task_id] = len(stack)
        stack.append(task_id)
        dependencies = tasks[task_id].get("depends_on", [])
        if isinstance(dependencies, list):
            for dependency in sorted(
                item for item in dependencies if isinstance(item, str)
            ):
                if dependency not in tasks:
                    continue
                if state.get(dependency) == 1:
                    return stack[positions[dependency] :] + [dependency]
                if state.get(dependency, 0) == 0:
                    cycle = visit(dependency)
                    if cycle:
                        return cycle
        stack.pop()
        positions.pop(task_id, None)
        state[task_id] = 2
        return None

    for task_id in sorted(tasks):
        if state.get(task_id, 0) == 0:
            cycle = visit(task_id)
            if cycle:
                return cycle
    return None


def _depends_on(
    tasks: dict[str, dict[str, Any]],
    task_id: str,
    dependency: str,
) -> bool:
    if task_id == dependency:
        return True
    pending = list(tasks.get(task_id, {}).get("depends_on", []))
    visited: set[str] = set()
    while pending:
        current = pending.pop()
        if current == dependency:
            return True
        if current in visited or current not in tasks:
            continue
        visited.add(current)
        raw_dependencies = tasks[current].get("depends_on", [])
        if isinstance(raw_dependencies, list):
            pending.extend(
                item for item in raw_dependencies if isinstance(item, str)
            )
    return False


def _plan_task_mappings(
    errors: list[str],
    plan_id: str,
    requirements: list[Any],
) -> dict[str, dict[str, set[str]]]:
    mappings: dict[str, dict[str, set[str]]] = {}
    seen_requirements: set[str] = set()
    for index, raw_requirement in enumerate(requirements):
        label = f"{plan_id}.requirements[{index}]"
        if not isinstance(raw_requirement, dict):
            errors.append(f"{label} must be an object")
            continue
        _reject_unknown_fields(
            errors,
            label,
            raw_requirement,
            REQUIREMENT_FIELDS,
        )
        requirement_id = raw_requirement.get("id")
        if not isinstance(requirement_id, str) or not REQUIREMENT_ID_PATTERN.fullmatch(
            requirement_id
        ):
            errors.append(f"{label}.id must match REQ-NAME")
            continue
        if requirement_id in seen_requirements:
            errors.append(f"{plan_id} has duplicate requirement {requirement_id}")
            continue
        seen_requirements.add(requirement_id)
        source_ref = raw_requirement.get("source_ref")
        if not isinstance(source_ref, str) or not SOURCE_REF_PATTERN.fullmatch(
            source_ref
        ):
            errors.append(f"{label}.source_ref must be a stable uppercase ID")
        _require_nonempty_string(
            errors, f"{label}.statement", raw_requirement.get("statement")
        )
        _require_string_list(
            errors,
            f"{label}.acceptance_criteria",
            raw_requirement.get("acceptance_criteria"),
            nonempty=True,
        )
        implementation_tasks = _require_string_list(
            errors,
            f"{label}.implementation_tasks",
            raw_requirement.get("implementation_tasks"),
            nonempty=True,
        )
        acceptance_task = raw_requirement.get("acceptance_task")
        if not isinstance(acceptance_task, str) or not TASK_ID_PATTERN.fullmatch(
            acceptance_task
        ):
            errors.append(f"{label}.acceptance_task must match XT-NNN")
            acceptance_task = ""
        for task_id in implementation_tasks:
            if not TASK_ID_PATTERN.fullmatch(task_id):
                errors.append(
                    f"{label}.implementation_tasks contains invalid task {task_id}"
                )
                continue
            entry = mappings.setdefault(
                task_id, {"requirements": set(), "roles": set()}
            )
            entry["requirements"].add(requirement_id)
            entry["roles"].add("implementation")
        if acceptance_task:
            entry = mappings.setdefault(
                acceptance_task, {"requirements": set(), "roles": set()}
            )
            entry["requirements"].add(requirement_id)
            entry["roles"].add("acceptance")
    return mappings


def _expected_role(roles: set[str]) -> str:
    if roles == {"implementation", "acceptance"}:
        return "implementation_acceptance"
    if roles == {"acceptance"}:
        return "acceptance"
    return "implementation"


def validate_repository(
    root: Path,
    *,
    backlog_document: dict[str, Any] | None = None,
    plan_overrides: dict[str, dict[str, Any]] | None = None,
) -> list[str]:
    errors: list[str] = []
    try:
        config = load_config(root)
    except DeliveryPlanError as error:
        return [str(error)]

    try:
        if backlog_document is None:
            backlog_document, tasks = load_backlog(root)
        else:
            raw_tasks = backlog_document.get("tasks")
            if not isinstance(raw_tasks, list):
                return [".agents/backlog.yaml tasks must be an array"]
            tasks = {
                task["id"]: task
                for task in raw_tasks
                if isinstance(task, dict) and isinstance(task.get("id"), str)
            }
            if len(tasks) != len(raw_tasks):
                errors.append("Candidate backlog has invalid or duplicate tasks")
    except DeliveryPlanError as error:
        return [str(error)]

    plans, plan_paths, load_errors = load_plans(root, config)
    errors.extend(load_errors)
    if plan_overrides:
        plans.update(copy.deepcopy(plan_overrides))

    cycle = _find_cycle(tasks)
    if cycle:
        errors.append("Task dependency cycle: " + " -> ".join(cycle))

    global_requirements: dict[str, str] = {}
    expected_by_task: dict[str, tuple[str, set[str], set[str]]] = {}
    plan_mappings: dict[str, dict[str, dict[str, set[str]]]] = {}

    for plan_id in sorted(plans):
        plan = plans[plan_id]
        label = plan_id
        _reject_unknown_fields(errors, label, plan, PLAN_FIELDS)
        if not PLAN_ID_PATTERN.fullmatch(plan_id):
            errors.append(f"Delivery Plan id must match DP-NAME: {plan_id}")
        plan_path = plan_paths.get(plan_id)
        if (
            plan_path is not None
            and plan_path.name != f"{plan_id}.json"
        ):
            errors.append(
                f"{label} must be stored as {plan_id}.json, not "
                f"{plan_path.name}"
            )
        if plan.get("schema_version") != 1:
            errors.append(f"{label}.schema_version must be 1")
        _require_nonempty_string(errors, f"{label}.title", plan.get("title"))
        status = plan.get("status")
        if status not in PLAN_STATUSES:
            errors.append(
                f"{label}.status must be one of {sorted(PLAN_STATUSES)}"
            )
        source = plan.get("source")
        source_kind: str | None = None
        source_path: Path | None = None
        if not isinstance(source, dict):
            errors.append(f"{label}.source must be an object")
        else:
            _reject_unknown_fields(
                errors,
                f"{label}.source",
                source,
                SOURCE_FIELDS,
            )
            source_kind = source.get("kind")
            if source_kind not in SOURCE_KINDS:
                errors.append(
                    f"{label}.source.kind must be one of {sorted(SOURCE_KINDS)}"
                )
            raw_path = source.get("path")
            if not isinstance(raw_path, str) or not raw_path:
                errors.append(f"{label}.source.path must be a repository path")
            else:
                candidate = Path(raw_path)
                if candidate.is_absolute():
                    errors.append(
                        f"{label}.source.path must be repository-relative"
                    )
                source_path = (root / candidate).resolve()
                try:
                    source_path.relative_to(root.resolve())
                except ValueError:
                    errors.append(
                        f"{label}.source.path escapes the repository: {raw_path}"
                    )
                    source_path = None
                if source_path is not None and not source_path.is_file():
                    errors.append(f"{label}.source.path does not exist: {raw_path}")

        requirements = plan.get("requirements")
        if not isinstance(requirements, list):
            errors.append(f"{label}.requirements must be an array")
            requirements = []
        if status == "approved" and not requirements:
            errors.append(f"{label}.requirements must not be empty when approved")
        mappings = _plan_task_mappings(errors, plan_id, requirements)
        plan_mappings[plan_id] = mappings

        for requirement in requirements:
            if not isinstance(requirement, dict):
                continue
            requirement_id = requirement.get("id")
            if not isinstance(requirement_id, str):
                continue
            previous = global_requirements.get(requirement_id)
            if previous is not None and previous != plan_id:
                errors.append(
                    f"Requirement {requirement_id} appears in {previous} and {plan_id}"
                )
            global_requirements[requirement_id] = plan_id
            if (
                status == "approved"
                and source_kind == "roadmap"
                and source_path is not None
                and source_path.is_file()
            ):
                source_ref = requirement.get("source_ref")
                marker = f"<!-- roadmap-id: {source_ref} -->"
                try:
                    source_text = source_path.read_text(encoding="utf-8")
                except OSError:
                    source_text = ""
                if marker not in source_text:
                    errors.append(
                        f"{label}.{requirement_id} is missing roadmap marker "
                        f"{marker}"
                    )

        approval = plan.get("approval")
        if not isinstance(approval, dict):
            errors.append(f"{label}.approval must be an object")
            approval = {}
        else:
            _reject_unknown_fields(
                errors,
                f"{label}.approval",
                approval,
                APPROVAL_FIELDS,
            )
        approved_by = approval.get("approved_by", "")
        approved_at = approval.get("approved_at", "")
        content_sha256 = approval.get("content_sha256", "")
        if status in {"approved", "superseded"}:
            if not isinstance(approved_by, str) or not OWNER_PATTERN.fullmatch(
                approved_by
            ):
                errors.append(f"{label}.approval.approved_by is invalid")
            if not _valid_timestamp(approved_at):
                errors.append(
                    f"{label}.approval.approved_at must be an RFC 3339 timestamp"
                )
            expected_digest = approval_digest(plan)
            if content_sha256 != expected_digest:
                errors.append(
                    f"{label}.approval.content_sha256 does not bind plan content"
                )
        elif approved_by or approved_at or content_sha256:
            errors.append(f"{label} draft plan must not carry approval")

        superseded_by = plan.get("superseded_by", "")
        if status == "superseded":
            if (
                not isinstance(superseded_by, str)
                or not PLAN_ID_PATTERN.fullmatch(superseded_by)
                or superseded_by == plan_id
            ):
                errors.append(f"{label}.superseded_by must name another plan")
        elif superseded_by:
            errors.append(f"{label}.superseded_by is valid only when superseded")

        for task_id, mapping in mappings.items():
            previous = expected_by_task.get(task_id)
            if previous is not None and previous[0] != plan_id:
                errors.append(
                    f"Task {task_id} appears in Delivery Plans "
                    f"{previous[0]} and {plan_id}"
                )
                continue
            expected_by_task[task_id] = (
                plan_id,
                mapping["requirements"],
                mapping["roles"],
            )
            if status == "approved" and task_id not in tasks:
                errors.append(f"{plan_id} references unknown task {task_id}")

        if status == "approved":
            for requirement in requirements:
                if not isinstance(requirement, dict):
                    continue
                requirement_id = requirement.get("id", "<invalid>")
                acceptance_task = requirement.get("acceptance_task")
                implementations = requirement.get("implementation_tasks", [])
                if not isinstance(acceptance_task, str) or acceptance_task not in tasks:
                    continue
                if not isinstance(implementations, list):
                    continue
                for implementation_task in implementations:
                    if (
                        isinstance(implementation_task, str)
                        and implementation_task in tasks
                        and not _depends_on(
                            tasks, acceptance_task, implementation_task
                        )
                    ):
                        errors.append(
                            f"{plan_id}.{requirement_id} acceptance task "
                            f"{acceptance_task} does not depend on "
                            f"{implementation_task}"
                        )

    for plan_id, plan in sorted(plans.items()):
        if plan.get("status") != "superseded":
            continue
        successor = plans.get(plan.get("superseded_by"))
        if successor is None:
            errors.append(
                f"{plan_id}.superseded_by references an unknown plan"
            )
        elif successor.get("status") != "approved":
            errors.append(
                f"{plan_id}.superseded_by must reference an approved plan"
            )

    for task_id, task in sorted(tasks.items()):
        match = TASK_ID_PATTERN.fullmatch(task_id)
        task_number = int(task_id.removeprefix("XT-")) if match else 0
        plan_id = task.get("delivery_plan")
        requirement_ids = task.get("requirement_ids")
        role = task.get("delivery_role")
        binding_present = any(
            value is not None for value in (plan_id, requirement_ids, role)
        )
        if task_number >= config.required_from_number and not binding_present:
            errors.append(
                f"{task_id} requires Delivery Plan metadata from "
                f"{config.required_from_task}"
            )
            continue
        if not binding_present:
            continue
        if not isinstance(plan_id, str) or not PLAN_ID_PATTERN.fullmatch(plan_id):
            errors.append(f"{task_id}.delivery_plan must match DP-NAME")
            continue
        listed_requirements = _require_string_list(
            errors,
            f"{task_id}.requirement_ids",
            requirement_ids,
            nonempty=True,
        )
        if role not in DELIVERY_ROLES:
            errors.append(
                f"{task_id}.delivery_role must be one of "
                f"{sorted(DELIVERY_ROLES)}"
            )
        plan = plans.get(plan_id)
        if plan is None:
            errors.append(f"{task_id} references unknown Delivery Plan {plan_id}")
            continue
        expected = expected_by_task.get(task_id)
        if expected is None:
            errors.append(f"{task_id} is orphaned from {plan_id} requirements")
            continue
        expected_plan, expected_requirements, expected_roles = expected
        if expected_plan != plan_id:
            errors.append(
                f"{task_id}.delivery_plan is {plan_id}, expected {expected_plan}"
            )
        if set(listed_requirements) != expected_requirements:
            errors.append(
                f"{task_id}.requirement_ids do not match {plan_id}: "
                f"expected {sorted(expected_requirements)}"
            )
        expected_role = _expected_role(expected_roles)
        if role != expected_role:
            errors.append(
                f"{task_id}.delivery_role is {role}, expected {expected_role}"
            )
        if plan.get("status") == "draft" and task.get("readiness") == "ready":
            errors.append(
                f"{task_id} must not be ready while {plan_id} is "
                f"{plan.get('status')}"
            )

        spec_path, spec_errors = _spec_for_task(root, task_id)
        errors.extend(spec_errors)
        if spec_path is None:
            continue
        try:
            spec = parse_task_spec(spec_path, root)
        except DeliveryPlanError as error:
            errors.append(str(error))
            continue
        comparable_fields = (
            "id",
            "title",
            "workstream",
            "depends_on",
            "owned_paths",
            "delivery_plan",
            "requirement_ids",
            "delivery_role",
        )
        for field in comparable_fields:
            if spec.get(field) != task.get(field):
                errors.append(
                    f"{task_id} task spec {field} does not match backlog"
                )
        if plan.get("status") == "approved":
            try:
                spec_text = spec_path.read_text(encoding="utf-8")
            except OSError:
                spec_text = ""
            if "TODO" in spec_text:
                errors.append(
                    f"{task_id} task spec contains TODO in approved {plan_id}"
                )

        if task_number >= config.required_from_number:
            record_path = root / ".agents" / "records" / f"{task_id}.json"
            if not record_path.is_file():
                errors.append(f"{task_id} is missing its task record")
                continue
            try:
                record = _load_json(record_path, root)
            except DeliveryPlanError as error:
                errors.append(str(error))
                continue
            for field in ("delivery_plan", "requirement_ids", "delivery_role"):
                if record.get(field) != task.get(field):
                    errors.append(
                        f"{task_id} task record {field} does not match backlog"
                    )
            if plan.get("status") == "approved":
                encoded = json.dumps(record, sort_keys=True)
                if "TODO" in encoded:
                    errors.append(
                        f"{task_id} task record contains TODO in approved {plan_id}"
                    )

    for task_id, (plan_id, _requirements, _roles) in sorted(
        expected_by_task.items()
    ):
        if task_id in tasks and not tasks[task_id].get("delivery_plan"):
            errors.append(
                f"{task_id} is mapped by {plan_id} but lacks inverse metadata"
            )

    return errors


def validate_claim(root: Path, task_id: str) -> list[str]:
    errors = validate_repository(root)
    if errors:
        return errors
    try:
        config = load_config(root)
        _document, tasks = load_backlog(root)
        plans, _paths, load_errors = load_plans(root, config)
    except DeliveryPlanError as error:
        return [str(error)]
    if load_errors:
        return load_errors
    task = tasks.get(task_id)
    if task is None:
        return [f"Unknown task: {task_id}"]
    task_number = int(task_id.removeprefix("XT-"))
    plan_id = task.get("delivery_plan")
    if task_number >= config.required_from_number or plan_id is not None:
        plan = plans.get(plan_id) if isinstance(plan_id, str) else None
        if plan is None or plan.get("status") != "approved":
            errors.append(f"{task_id} requires an approved Delivery Plan")
        for dependency in task.get("depends_on", []):
            record_path = root / ".agents" / "records" / f"{dependency}.json"
            if not record_path.is_file():
                errors.append(f"{task_id} dependency {dependency} has no record")
                continue
            try:
                record = _load_json(record_path, root)
            except DeliveryPlanError as error:
                errors.append(str(error))
                continue
            if record.get("state") != "done":
                errors.append(
                    f"{task_id} dependency {dependency} is "
                    f"{record.get('state')}, not done"
                )
    return errors


def validate_registration(
    root: Path,
    task_id: str,
    plan_id: str,
    requirement_ids: list[str],
    role: str,
) -> list[str]:
    errors: list[str] = []
    try:
        config = load_config(root)
        plans, _paths, load_errors = load_plans(root, config)
    except DeliveryPlanError as error:
        return [str(error)]
    errors.extend(load_errors)
    plan = plans.get(plan_id)
    if plan is None:
        return errors + [f"Unknown Delivery Plan: {plan_id}"]
    requirements = plan.get("requirements")
    if not isinstance(requirements, list):
        return errors + [f"{plan_id}.requirements must be an array"]
    mapping_errors: list[str] = []
    mappings = _plan_task_mappings(mapping_errors, plan_id, requirements)
    errors.extend(mapping_errors)
    expected = mappings.get(task_id)
    if expected is None:
        errors.append(f"{task_id} is not reserved by {plan_id}")
        return errors
    if set(requirement_ids) != expected["requirements"]:
        errors.append(
            f"{task_id} requirements must be "
            f"{sorted(expected['requirements'])}"
        )
    expected_role = _expected_role(expected["roles"])
    if role != expected_role:
        errors.append(f"{task_id} role must be {expected_role}")
    return errors


def _task_sort_key(task_id: str) -> tuple[int, str]:
    match = TASK_ID_PATTERN.fullmatch(task_id)
    if match is None:
        return (sys.maxsize, task_id)
    return (int(task_id.removeprefix("XT-")), task_id)


def _record_state(root: Path, task_id: str) -> str:
    path = root / ".agents" / "records" / f"{task_id}.json"
    if not path.is_file():
        raise DeliveryPlanError(f"{task_id} is missing its task record")
    record = _load_json(path, root)
    state = record.get("state")
    if not isinstance(state, str) or not state:
        raise DeliveryPlanError(f"{task_id}.state is missing")
    return state


def _scheduled_state(
    root: Path,
    task: dict[str, Any],
    states: dict[str, str],
) -> str:
    task_id = task["id"]
    state = states[task_id]
    if state != "ready":
        return state
    dependencies = task.get("depends_on", [])
    if not isinstance(dependencies, list):
        raise DeliveryPlanError(f"{task_id}.depends_on must be an array")
    for dependency in dependencies:
        if not isinstance(dependency, str):
            raise DeliveryPlanError(f"{task_id}.depends_on must contain task ids")
        dependency_state = states.get(dependency)
        if dependency_state is None:
            dependency_state = _record_state(root, dependency)
            states[dependency] = dependency_state
        if dependency_state != "done":
            return "dependency-blocked"
    return "claimable"


def _requirement_state(
    requirement: dict[str, Any],
    states: dict[str, str],
) -> str:
    acceptance_task = requirement["acceptance_task"]
    if states[acceptance_task] == "done":
        return "accepted"
    implementations = requirement["implementation_tasks"]
    implementation_states = [states[task_id] for task_id in implementations]
    if all(state == "done" for state in implementation_states):
        return "acceptance-ready"
    if any(state in ACTIVE_TASK_STATES for state in implementation_states):
        return "in-progress"
    if any(state == "done" for state in implementation_states):
        return "partially-delivered"
    return "planned"


def _markdown_cell(value: str) -> str:
    return value.replace("\\", "\\\\").replace("|", "\\|").replace("\n", " ")


def render_status_view(root: Path, plan_id: str) -> str:
    errors = validate_repository(root)
    if errors:
        raise DeliveryPlanError("\n".join(f"- {error}" for error in errors))
    config = load_config(root)
    _backlog, tasks = load_backlog(root)
    plans, _paths, load_errors = load_plans(root, config)
    if load_errors:
        raise DeliveryPlanError("\n".join(load_errors))
    plan = plans.get(plan_id)
    if plan is None:
        raise DeliveryPlanError(f"Unknown Delivery Plan: {plan_id}")

    requirements = plan.get("requirements")
    if not isinstance(requirements, list):
        raise DeliveryPlanError(f"{plan_id}.requirements must be an array")
    mapped_task_ids = {
        task_id
        for requirement in requirements
        for task_id in (
            list(requirement["implementation_tasks"])
            + [requirement["acceptance_task"]]
        )
    }
    states = {
        task_id: _record_state(root, task_id)
        for task_id in sorted(mapped_task_ids, key=_task_sort_key)
    }
    scheduled_states = {
        task_id: _scheduled_state(root, tasks[task_id], states)
        for task_id in sorted(mapped_task_ids, key=_task_sort_key)
    }
    requirement_states = {
        requirement["id"]: _requirement_state(requirement, states)
        for requirement in requirements
    }

    task_counts: dict[str, int] = {}
    for state in scheduled_states.values():
        task_counts[state] = task_counts.get(state, 0) + 1
    accepted_count = sum(
        state == "accepted" for state in requirement_states.values()
    )
    approval = plan.get("approval", {})
    summary = ", ".join(
        f"{state}={count}" for state, count in sorted(task_counts.items())
    )
    lines = [
        f"# {plan['title']} ({plan_id})",
        "",
        f"- Plan status: `{plan['status']}`",
        (
            f"- Approval: `{approval.get('approved_by', '')}` at "
            f"`{approval.get('approved_at', '')}`"
        ),
        (
            f"- Requirements accepted: `{accepted_count}/"
            f"{len(requirements)}`"
        ),
        f"- Task states: {summary}",
        "",
        "## Requirements",
        "",
        "| Requirement | State | Acceptance | Implementation |",
        "| --- | --- | --- | --- |",
    ]
    for requirement in requirements:
        requirement_id = requirement["id"]
        implementations = ", ".join(requirement["implementation_tasks"])
        lines.append(
            "| "
            + " | ".join(
                _markdown_cell(value)
                for value in (
                    requirement_id,
                    requirement_states[requirement_id],
                    requirement["acceptance_task"],
                    implementations,
                )
            )
            + " |"
        )

    lines.extend(
        [
            "",
            "## Tasks",
            "",
            "| Task | State | Role | Requirements | Depends on |",
            "| --- | --- | --- | --- | --- |",
        ]
    )
    for task_id in sorted(mapped_task_ids, key=_task_sort_key):
        task = tasks[task_id]
        lines.append(
            "| "
            + " | ".join(
                _markdown_cell(value)
                for value in (
                    task_id,
                    scheduled_states[task_id],
                    task["delivery_role"],
                    ", ".join(task["requirement_ids"]),
                    ", ".join(task.get("depends_on", [])) or "-",
                )
            )
            + " |"
        )
    return "\n".join(lines) + "\n"


def _candidate_plan_path(
    root: Path,
    config: DeliveryPlanConfig,
    plan_id: str,
) -> Path:
    return config.directory / f"{plan_id}.json"


def initialize_plan(root: Path, plan_id: str, title: str, kind: str, path: str) -> Path:
    config = load_config(root)
    if not PLAN_ID_PATTERN.fullmatch(plan_id):
        raise DeliveryPlanError("Plan id must match DP-NAME")
    if kind not in SOURCE_KINDS:
        raise DeliveryPlanError(f"Source kind must be one of {sorted(SOURCE_KINDS)}")
    source_value = Path(path)
    if source_value.is_absolute():
        raise DeliveryPlanError("Source path must be repository-relative")
    source = (root / source_value).resolve()
    try:
        source.relative_to(root.resolve())
    except ValueError as error:
        raise DeliveryPlanError("Source path escapes the repository") from error
    if not source.is_file():
        raise DeliveryPlanError(f"Source path does not exist: {path}")
    destination = _candidate_plan_path(root, config, plan_id)
    if destination.exists():
        raise DeliveryPlanError(f"Delivery Plan already exists: {plan_id}")
    _write_json(
        destination,
        {
            "schema_version": 1,
            "id": plan_id,
            "title": title,
            "status": "draft",
            "source": {"kind": kind, "path": path},
            "requirements": [],
            "approval": {
                "approved_by": "",
                "approved_at": "",
                "content_sha256": "",
            },
            "superseded_by": "",
        },
    )
    return destination


def _atomic_replace_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=path.parent,
        prefix=f".{path.name}.",
        suffix=".tmp",
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            json.dump(value, output, indent=2, ensure_ascii=True)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def approve_plan(root: Path, plan_id: str, approved_by: str) -> None:
    if not OWNER_PATTERN.fullmatch(approved_by):
        raise DeliveryPlanError("Approver must be a filesystem-safe slug")
    config = load_config(root)
    backlog_document, tasks = load_backlog(root)
    plans, paths, load_errors = load_plans(root, config)
    if load_errors:
        raise DeliveryPlanError("\n".join(load_errors))
    plan = plans.get(plan_id)
    if plan is None:
        raise DeliveryPlanError(f"Unknown Delivery Plan: {plan_id}")
    if plan.get("status") != "draft":
        raise DeliveryPlanError(f"{plan_id} must be draft before approval")

    candidate_plan = copy.deepcopy(plan)
    candidate_plan["status"] = "approved"
    candidate_plan["approval"] = {
        "approved_by": approved_by,
        "approved_at": dt.datetime.now(dt.timezone.utc).isoformat(
            timespec="seconds"
        ),
        "content_sha256": approval_digest(candidate_plan),
    }
    candidate_backlog = copy.deepcopy(backlog_document)
    referenced = {
        task_id
        for requirement in candidate_plan.get("requirements", [])
        if isinstance(requirement, dict)
        for task_id in (
            list(requirement.get("implementation_tasks", []))
            + [requirement.get("acceptance_task")]
        )
        if isinstance(task_id, str)
    }
    for task in candidate_backlog.get("tasks", []):
        if task.get("id") in referenced and task.get("readiness") == "blocked":
            task["readiness"] = "ready"

    errors = validate_repository(
        root,
        backlog_document=candidate_backlog,
        plan_overrides={plan_id: candidate_plan},
    )
    if errors:
        raise DeliveryPlanError("\n".join(f"- {error}" for error in errors))

    backlog_path = root / ".agents" / "backlog.yaml"
    plan_path = paths[plan_id]
    original_backlog = backlog_path.read_bytes()
    original_plan = plan_path.read_bytes()
    try:
        _atomic_replace_json(backlog_path, candidate_backlog)
        _atomic_replace_json(plan_path, candidate_plan)
    except OSError as error:
        backlog_path.write_bytes(original_backlog)
        plan_path.write_bytes(original_plan)
        raise DeliveryPlanError(f"Cannot approve {plan_id}: {error}") from error


def _print_errors(errors: list[str]) -> int:
    if errors:
        print("Delivery Plan validation failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("validate")

    claim_parser = subparsers.add_parser("claim-eligible")
    claim_parser.add_argument("task_id")

    init_parser = subparsers.add_parser("init")
    init_parser.add_argument("plan_id")
    init_parser.add_argument("title")
    init_parser.add_argument("--source-kind", choices=sorted(SOURCE_KINDS), required=True)
    init_parser.add_argument("--source-path", required=True)

    approve_parser = subparsers.add_parser("approve")
    approve_parser.add_argument("plan_id")
    approve_parser.add_argument("--by", required=True)

    status_parser = subparsers.add_parser("status")
    status_parser.add_argument("plan_id")

    args = parser.parse_args()
    root = args.root.resolve()
    try:
        if args.command == "validate":
            errors = validate_repository(root)
            result = _print_errors(errors)
            if result == 0:
                print("Delivery Plan validation passed.")
            return result
        if args.command == "claim-eligible":
            errors = validate_claim(root, args.task_id)
            result = _print_errors(errors)
            if result == 0:
                print(f"{args.task_id} has an approved Delivery Plan.")
            return result
        if args.command == "init":
            destination = initialize_plan(
                root,
                args.plan_id,
                args.title,
                args.source_kind,
                args.source_path,
            )
            print(f"Created {_relative(destination, root)}")
            return 0
        if args.command == "approve":
            approve_plan(root, args.plan_id, args.by)
            print(f"Approved {args.plan_id} by {args.by}")
            return 0
        if args.command == "status":
            print(render_status_view(root, args.plan_id), end="")
            return 0
    except DeliveryPlanError as error:
        print(f"Delivery Plan error:\n{error}", file=sys.stderr)
        return 1
    parser.error("unknown command")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
