#!/usr/bin/env python3

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any, Callable

MODES = {"none", "add", "replace", "remove", "refactor"}
MODULE_ID_PATTERN = re.compile(r"^[a-z][a-z0-9_]*$")
LEASE_ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
TARGET_PATTERN = re.compile(r"^xnn_transfer_[a-z0-9_]+$")

OwnedPathCheck = Callable[[str, list[str], str], bool]


class ArchitectureChangeError(RuntimeError):
    pass


def module_ids(path: Path) -> set[str]:
    try:
        with path.open(encoding="utf-8") as source:
            document = json.load(source)
    except (OSError, json.JSONDecodeError) as error:
        raise ArchitectureChangeError(
            f"Cannot read module inventory {path}: {error}"
        ) from error
    modules = document.get("modules")
    if not isinstance(modules, list):
        raise ArchitectureChangeError(
            ".agents/architecture/modules.json must contain modules"
        )
    return {
        module["id"]
        for module in modules
        if isinstance(module, dict) and isinstance(module.get("id"), str)
    }


def safe_repository_path(value: Any) -> bool:
    if not isinstance(value, str) or not value:
        return False
    path = Path(value)
    return not path.is_absolute() and ".." not in path.parts


def string_array(
    errors: list[str],
    value: Any,
    label: str,
    *,
    pattern: re.Pattern[str] | None = None,
) -> list[str]:
    if not isinstance(value, list) or not all(
        isinstance(item, str) and item for item in value
    ):
        errors.append(f"{label} must be an array of non-empty strings")
        return []
    if len(value) != len(set(value)):
        errors.append(f"{label} contains duplicates")
    if pattern is not None:
        for item in value:
            if not pattern.fullmatch(item):
                errors.append(f"{label} contains invalid value: {item}")
    return value


def nonempty_string(errors: list[str], value: Any, label: str) -> str:
    if not isinstance(value, str) or not value.strip():
        errors.append(f"{label} must be a non-empty string")
        return ""
    return value


def validate_change(
    errors: list[str],
    task: dict[str, Any],
    record: dict[str, Any],
    tasks: dict[str, dict[str, Any]],
    known_modules: set[str],
    path_allowed: OwnedPathCheck,
) -> None:
    task_id = task["id"]
    label = f"{task_id}.architecture_change"
    change = record.get("architecture_change")
    if not isinstance(change, dict):
        errors.append(f"{label} must be an object")
        return
    expected_fields = {
        "mode",
        "modules",
        "supersedes",
        "temporary_leases",
        "retires_leases",
    }
    missing = sorted(expected_fields - set(change))
    unexpected = sorted(set(change) - expected_fields)
    if missing:
        errors.append(f"{label} is missing fields: {', '.join(missing)}")
    if unexpected:
        errors.append(f"{label} has unexpected fields: {', '.join(unexpected)}")

    mode = nonempty_string(errors, change.get("mode"), f"{label}.mode")
    if mode not in MODES:
        errors.append(f"{label}.mode must be one of {sorted(MODES)}")
    modules = string_array(
        errors,
        change.get("modules"),
        f"{label}.modules",
        pattern=MODULE_ID_PATTERN,
    )
    for module in modules:
        if module not in known_modules:
            errors.append(f"{label}.modules references unknown module: {module}")

    supersedes = change.get("supersedes")
    if not isinstance(supersedes, dict):
        errors.append(f"{label}.supersedes must be an object")
        supersedes = {}
    else:
        expected_supersedes = {"paths", "symbols", "targets"}
        missing_supersedes = sorted(expected_supersedes - set(supersedes))
        unexpected_supersedes = sorted(set(supersedes) - expected_supersedes)
        if missing_supersedes:
            errors.append(
                f"{label}.supersedes is missing fields: "
                + ", ".join(missing_supersedes)
            )
        if unexpected_supersedes:
            errors.append(
                f"{label}.supersedes has unexpected fields: "
                + ", ".join(unexpected_supersedes)
            )
    paths = string_array(
        errors,
        supersedes.get("paths"),
        f"{label}.supersedes.paths",
    )
    for path in paths:
        if not safe_repository_path(path):
            errors.append(f"{label}.supersedes.paths has unsafe path: {path}")
        elif not path_allowed(path, task.get("owned_paths", []), task_id):
            errors.append(
                f"{label}.supersedes.paths is outside task ownership: {path}"
            )
    targets = string_array(
        errors,
        supersedes.get("targets"),
        f"{label}.supersedes.targets",
        pattern=TARGET_PATTERN,
    )
    symbols = supersedes.get("symbols")
    if not isinstance(symbols, list):
        errors.append(f"{label}.supersedes.symbols must be an array")
        symbols = []
    seen_symbols: set[tuple[str, str]] = set()
    for index, symbol in enumerate(symbols):
        symbol_label = f"{label}.supersedes.symbols[{index}]"
        if not isinstance(symbol, dict) or set(symbol) != {"path", "name"}:
            errors.append(
                f"{symbol_label} must contain exactly path and name"
            )
            continue
        path = symbol.get("path")
        name = symbol.get("name")
        if not safe_repository_path(path):
            errors.append(f"{symbol_label}.path is unsafe")
        elif not path_allowed(str(path), task.get("owned_paths", []), task_id):
            errors.append(f"{symbol_label}.path is outside task ownership")
        if not isinstance(name, str) or not name.strip():
            errors.append(f"{symbol_label}.name must not be empty")
            continue
        key = (str(path), name)
        if key in seen_symbols:
            errors.append(f"{label}.supersedes.symbols contains duplicates")
        seen_symbols.add(key)

    leases = change.get("temporary_leases")
    if not isinstance(leases, list):
        errors.append(f"{label}.temporary_leases must be an array")
        leases = []
    seen_leases: set[str] = set()
    for index, lease in enumerate(leases):
        lease_label = f"{label}.temporary_leases[{index}]"
        expected_lease = {"id", "path", "remove_by_task", "reason"}
        if not isinstance(lease, dict) or set(lease) != expected_lease:
            errors.append(
                f"{lease_label} must contain exactly "
                "id, path, remove_by_task, and reason"
            )
            continue
        lease_id = lease.get("id")
        if not isinstance(lease_id, str) or not LEASE_ID_PATTERN.fullmatch(
            lease_id
        ):
            errors.append(f"{lease_label}.id is invalid")
        elif lease_id in seen_leases:
            errors.append(f"{label}.temporary_leases contains duplicate IDs")
        else:
            seen_leases.add(lease_id)
        path = lease.get("path")
        if not safe_repository_path(path):
            errors.append(f"{lease_label}.path is unsafe")
        elif not path_allowed(str(path), task.get("owned_paths", []), task_id):
            errors.append(f"{lease_label}.path is outside task ownership")
        removal_task = lease.get("remove_by_task")
        if removal_task not in tasks:
            errors.append(
                f"{lease_label}.remove_by_task is unknown: {removal_task}"
            )
        elif removal_task == task_id:
            errors.append(
                f"{lease_label}.remove_by_task must be a later task"
            )
        reason = lease.get("reason")
        if not isinstance(reason, str) or len(reason.strip()) < 20:
            errors.append(f"{lease_label}.reason is too short")

    retires = string_array(
        errors,
        change.get("retires_leases"),
        f"{label}.retires_leases",
        pattern=LEASE_ID_PATTERN,
    )
    has_supersession = bool(paths or symbols or targets)
    if mode == "none" and (
        modules or has_supersession or leases or retires
    ):
        errors.append(f"{label}.mode none cannot declare architecture changes")
    if mode == "add" and not modules:
        errors.append(f"{label}.mode add requires at least one module")
    if mode == "replace" and not modules:
        errors.append(f"{label}.mode replace requires at least one module")
    if mode == "remove" and not (modules or has_supersession or retires):
        errors.append(f"{label}.mode remove requires a removal claim")
    if mode == "refactor" and not (modules or has_supersession or leases):
        errors.append(f"{label}.mode refactor requires an affected boundary")


def module_boundaries(root: Path) -> dict[str, tuple[str, list[str]]]:
    with (
        root / ".agents" / "architecture" / "modules.json"
    ).open(encoding="utf-8") as source:
        document = json.load(source)
    boundaries: dict[str, tuple[str, list[str]]] = {}
    for module in document.get("modules", []):
        if not isinstance(module, dict):
            continue
        module_id = module.get("id")
        definition = module.get("definition")
        roots = module.get("owned_roots")
        if (
            isinstance(module_id, str)
            and isinstance(definition, str)
            and isinstance(roots, list)
            and all(isinstance(root_path, str) for root_path in roots)
        ):
            boundaries[module_id] = (definition, roots)
    return boundaries


def path_under(path: str, boundary: str) -> bool:
    return path == boundary or path.startswith(boundary.rstrip("/") + "/")


def managed_path(path: str) -> bool:
    if not (
        path.startswith("native/include/") or path.startswith("native/src/")
    ):
        return False
    return path.endswith(
        (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", "/CMakeLists.txt")
    )


def validate_review(
    task: dict[str, Any],
    record: dict[str, Any],
    worktree: Path,
    changed_paths: list[str],
) -> None:
    if task.get("architecture_contract_required") is not True:
        return
    change = record["architecture_change"]
    declared = set(change["modules"])
    affected: set[str] = set()
    boundaries = module_boundaries(worktree)
    unowned: list[str] = []
    for path in changed_paths:
        matched = {
            module_id
            for module_id, (definition, roots) in boundaries.items()
            if any(
                path_under(path, boundary)
                for boundary in [definition, *roots]
            )
        }
        affected.update(matched)
        if managed_path(path) and not matched:
            unowned.append(path)
    if unowned:
        raise ArchitectureChangeError(
            "Changed production paths have no canonical module:\n"
            + "\n".join(f"- {path}" for path in sorted(unowned))
        )
    if affected != declared:
        missing = sorted(affected - declared)
        untouched = sorted(declared - affected)
        details: list[str] = []
        if missing:
            details.append("undeclared affected modules: " + ", ".join(missing))
        if untouched:
            details.append("declared but untouched modules: " + ", ".join(untouched))
        raise ArchitectureChangeError(
            "Architecture change declaration does not match the diff:\n- "
            + "\n- ".join(details)
        )

    supersedes = change["supersedes"]
    remaining_paths = [
        path for path in supersedes["paths"] if (worktree / path).exists()
    ]
    if remaining_paths:
        raise ArchitectureChangeError(
            "Superseded paths still exist:\n"
            + "\n".join(f"- {path}" for path in remaining_paths)
        )
    remaining_symbols: list[str] = []
    for symbol in supersedes["symbols"]:
        path = worktree / symbol["path"]
        if path.is_file() and symbol["name"] in path.read_text(encoding="utf-8"):
            remaining_symbols.append(f"{symbol['path']}: {symbol['name']}")
    if remaining_symbols:
        raise ArchitectureChangeError(
            "Superseded symbols still exist:\n"
            + "\n".join(f"- {item}" for item in remaining_symbols)
        )

    remaining_targets: list[str] = []
    cmake_paths = [worktree / "native" / "CMakeLists.txt"]
    cmake_paths.extend(
        sorted((worktree / "native" / "src").rglob("CMakeLists.txt"))
    )
    for target in supersedes["targets"]:
        pattern = re.compile(
            rf"add_library\s*\(\s*{re.escape(target)}(?:\s|\))",
            flags=re.IGNORECASE,
        )
        if any(
            pattern.search(path.read_text(encoding="utf-8"))
            for path in cmake_paths
        ):
            remaining_targets.append(target)
    if remaining_targets:
        raise ArchitectureChangeError(
            "Superseded targets still exist:\n"
            + "\n".join(f"- {target}" for target in remaining_targets)
        )
