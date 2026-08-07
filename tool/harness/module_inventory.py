#!/usr/bin/env python3

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any

CMAKE_LIBRARY = re.compile(
    r"add_library\s*\(\s*"
    r"(?P<target>xnn_transfer_[a-z0-9_]+)"
    r"(?:\s+(?P<kind>STATIC|SHARED|MODULE|OBJECT|INTERFACE))?",
    flags=re.IGNORECASE,
)
TEMPORARY_MARKER = re.compile(
    r"XNN-TEMPORARY\((?P<id>[a-z0-9][a-z0-9._-]*)\)"
)
UNLEASED_DEBT_MARKER = re.compile(r"\b(?:TODO|FIXME)\b")
MODULES_PATH = PurePosixPath(".agents/architecture/modules.json")
PRODUCTION_SUFFIXES = {
    ".c",
    ".cc",
    ".cmake",
    ".cpp",
    ".cxx",
    ".dart",
    ".h",
    ".hpp",
}
PLACEHOLDER_STATES = {"ready", "claimed"}
CONCRETE_STATES = {"in_progress", "review", "integrated", "done"}


@dataclass(frozen=True, order=True)
class Violation:
    path: str
    line: int
    message: str

    def render(self) -> str:
        return f"{self.path}:{self.line}: {self.message}"


@dataclass(frozen=True)
class Module:
    id: str
    target: str
    definition: PurePosixPath
    concrete_type: str
    owned_roots: tuple[PurePosixPath, ...]
    allowed_project_dependencies: frozenset[str]
    placeholder_until: str | None


@dataclass(frozen=True)
class TargetDefinition:
    target: str
    kind: str
    path: PurePosixPath
    line: int


def load_json(path: Path) -> Any:
    with path.open(encoding="utf-8") as source:
        return json.load(source)


def schema_violation(message: str) -> Violation:
    return Violation(MODULES_PATH.as_posix(), 1, message)


def parse_modules(root: Path) -> tuple[list[Module], list[Violation]]:
    path = root / MODULES_PATH
    try:
        document = load_json(path)
    except (OSError, json.JSONDecodeError) as error:
        return [], [schema_violation(f"cannot read module inventory: {error}")]

    violations: list[Violation] = []
    if not isinstance(document, dict) or document.get("schema_version") != 1:
        return [], [schema_violation("module inventory schema_version must be 1")]
    raw_modules = document.get("modules")
    if not isinstance(raw_modules, list) or not raw_modules:
        return [], [schema_violation("module inventory must contain modules")]

    modules: list[Module] = []
    seen_ids: set[str] = set()
    seen_targets: set[str] = set()
    seen_roots: dict[PurePosixPath, str] = {}
    for index, raw in enumerate(raw_modules):
        label = f"modules[{index}]"
        if not isinstance(raw, dict):
            violations.append(schema_violation(f"{label} must be an object"))
            continue
        module_id = raw.get("id")
        target = raw.get("target")
        definition = raw.get("definition")
        concrete_type = raw.get("concrete_type")
        roots = raw.get("owned_roots")
        dependencies = raw.get("allowed_project_dependencies")
        placeholder = raw.get("placeholder_until")
        if not isinstance(module_id, str) or not re.fullmatch(
            r"[a-z][a-z0-9_]*", module_id
        ):
            violations.append(schema_violation(f"{label}.id is invalid"))
            continue
        if module_id in seen_ids:
            violations.append(
                schema_violation(f"duplicate module id: {module_id}")
            )
        seen_ids.add(module_id)
        if not isinstance(target, str) or not re.fullmatch(
            r"xnn_transfer_[a-z0-9_]+", target
        ):
            violations.append(schema_violation(f"{label}.target is invalid"))
            continue
        if target in seen_targets:
            violations.append(schema_violation(f"duplicate target: {target}"))
        seen_targets.add(target)
        if not isinstance(definition, str) or not definition.endswith(
            "CMakeLists.txt"
        ):
            violations.append(
                schema_violation(f"{label}.definition must be a CMakeLists.txt")
            )
            continue
        if concrete_type not in {"STATIC", "SHARED", "MODULE", "OBJECT"}:
            violations.append(
                schema_violation(f"{label}.concrete_type is invalid")
            )
            continue
        if not isinstance(roots, list) or not roots or not all(
            isinstance(item, str) and item for item in roots
        ):
            violations.append(
                schema_violation(f"{label}.owned_roots must not be empty")
            )
            continue
        if not isinstance(dependencies, list) or not all(
            isinstance(item, str)
            and re.fullmatch(r"xnn_transfer_[a-z0-9_]+", item)
            for item in dependencies
        ):
            violations.append(
                schema_violation(
                    f"{label}.allowed_project_dependencies is invalid"
                )
            )
            continue
        if placeholder is not None and (
            not isinstance(placeholder, str)
            or not re.fullmatch(r"XT-[0-9]{3,}", placeholder)
        ):
            violations.append(
                schema_violation(f"{label}.placeholder_until is invalid")
            )
            continue

        parsed_roots = tuple(PurePosixPath(item) for item in roots)
        for owned_root in parsed_roots:
            if owned_root.is_absolute() or ".." in owned_root.parts:
                violations.append(
                    schema_violation(
                        f"{label}.owned_roots contains an unsafe path"
                    )
                )
            for existing_root, previous in seen_roots.items():
                if (
                    owned_root == existing_root
                    or existing_root in owned_root.parents
                    or owned_root in existing_root.parents
                ):
                    violations.append(
                        schema_violation(
                            f"owned roots {existing_root} and {owned_root} "
                            f"overlap across {previous} and {module_id}"
                        )
                    )
            seen_roots[owned_root] = module_id
        modules.append(
            Module(
                id=module_id,
                target=target,
                definition=PurePosixPath(definition),
                concrete_type=concrete_type,
                owned_roots=parsed_roots,
                allowed_project_dependencies=frozenset(dependencies),
                placeholder_until=placeholder,
            )
        )

    declared_targets = {module.target for module in modules}
    for module in modules:
        unknown = module.allowed_project_dependencies - declared_targets
        for dependency in sorted(unknown):
            violations.append(
                schema_violation(
                    f"{module.id} allows undeclared dependency {dependency}"
                )
            )
    return modules, violations


def target_links(modules: list[Module]) -> dict[str, set[str]]:
    return {
        module.target: set(module.allowed_project_dependencies)
        for module in modules
    }


def scan_library_definitions(
    path: PurePosixPath, text: str
) -> list[TargetDefinition]:
    uncommented = re.sub(r"(?m)#.*$", "", text)
    definitions: list[TargetDefinition] = []
    for match in CMAKE_LIBRARY.finditer(uncommented):
        definitions.append(
            TargetDefinition(
                target=match.group("target").lower(),
                kind=(match.group("kind") or "").upper(),
                path=path,
                line=uncommented.count("\n", 0, match.start()) + 1,
            )
        )
    return definitions


def load_records(root: Path) -> dict[str, dict[str, Any]]:
    records: dict[str, dict[str, Any]] = {}
    for path in sorted((root / ".agents" / "records").glob("XT-*.json")):
        value = load_json(path)
        if isinstance(value, dict) and isinstance(value.get("id"), str):
            records[value["id"]] = value
    return records


def validate_module_inventory(
    root: Path,
    modules: list[Module],
    cmake_paths: list[Path],
    records: dict[str, dict[str, Any]],
) -> list[Violation]:
    violations: list[Violation] = []
    definitions: dict[str, list[TargetDefinition]] = {}
    for path in cmake_paths:
        relative = PurePosixPath(path.relative_to(root).as_posix())
        for definition in scan_library_definitions(
            relative, path.read_text(encoding="utf-8")
        ):
            definitions.setdefault(definition.target, []).append(definition)

    declared_targets = {module.target for module in modules}
    for target, found in sorted(definitions.items()):
        if target not in declared_targets:
            for definition in found:
                violations.append(
                    Violation(
                        definition.path.as_posix(),
                        definition.line,
                        f"production target is not declared in the module "
                        f"inventory: {target}",
                    )
                )

    for module in modules:
        found = definitions.get(module.target, [])
        if len(found) != 1:
            violations.append(
                schema_violation(
                    f"{module.id} must have exactly one add_library definition; "
                    f"found {len(found)}"
                )
            )
            continue
        definition = found[0]
        if definition.path != module.definition:
            violations.append(
                Violation(
                    definition.path.as_posix(),
                    definition.line,
                    f"{module.target} must be defined in {module.definition}",
                )
            )
        if not definition.kind:
            violations.append(
                Violation(
                    definition.path.as_posix(),
                    definition.line,
                    f"{module.target} must declare an explicit library type",
                )
            )
            continue

        replacement_state = ""
        if module.placeholder_until is not None:
            replacement = records.get(module.placeholder_until)
            if replacement is None:
                violations.append(
                    schema_violation(
                        f"{module.id} references unknown replacement task "
                        f"{module.placeholder_until}"
                    )
                )
                continue
            replacement_state = str(replacement.get("state", ""))

        if module.placeholder_until is None:
            expected_kind = module.concrete_type
        elif replacement_state in PLACEHOLDER_STATES:
            expected_kind = "INTERFACE"
        elif replacement_state in CONCRETE_STATES:
            expected_kind = module.concrete_type
        elif replacement_state == "blocked":
            expected_kind = definition.kind
        else:
            violations.append(
                schema_violation(
                    f"{module.placeholder_until} has invalid state "
                    f"{replacement_state!r}"
                )
            )
            continue

        if definition.kind != expected_kind:
            if expected_kind == "INTERFACE":
                message = (
                    f"{module.target} became concrete before replacement task "
                    f"{module.placeholder_until} started"
                )
            elif definition.kind == "INTERFACE":
                message = (
                    f"{module.placeholder_until} started but "
                    f"{module.target} is still an INTERFACE placeholder"
                )
            else:
                message = (
                    f"{module.target} must be {expected_kind}, "
                    f"not {definition.kind}"
                )
            violations.append(
                Violation(
                    definition.path.as_posix(),
                    definition.line,
                    message,
                )
            )
    return violations


def production_files(root: Path) -> list[Path]:
    paths: list[Path] = []
    for production_root in (
        root / "native" / "include",
        root / "native" / "src",
        root / "apps" / "desktop" / "lib",
    ):
        for path in sorted(production_root.rglob("*")):
            if not path.is_file():
                continue
            if path.suffix in PRODUCTION_SUFFIXES or path.name == "CMakeLists.txt":
                paths.append(path)
    return paths


def validate_temporary_leases(
    root: Path, records: dict[str, dict[str, Any]]
) -> list[Violation]:
    violations: list[Violation] = []
    markers: dict[str, list[tuple[PurePosixPath, int]]] = {}
    for path in production_files(root):
        relative = PurePosixPath(path.relative_to(root).as_posix())
        text = path.read_text(encoding="utf-8")
        for line_index, line in enumerate(text.splitlines(), start=1):
            line_markers = list(TEMPORARY_MARKER.finditer(line))
            for marker in line_markers:
                markers.setdefault(marker.group("id"), []).append(
                    (relative, line_index)
                )
            if UNLEASED_DEBT_MARKER.search(line) and not line_markers:
                violations.append(
                    Violation(
                        relative.as_posix(),
                        line_index,
                        "TODO/FIXME in production code requires an "
                        "XNN-TEMPORARY lease marker",
                    )
                )

    leases: dict[str, tuple[str, dict[str, Any]]] = {}
    retired_by: dict[str, str] = {}
    for task_id, record in records.items():
        change = record.get("architecture_change")
        if not isinstance(change, dict):
            continue
        for lease_id in change.get("retires_leases", []):
            if isinstance(lease_id, str):
                previous = retired_by.get(lease_id)
                if previous is not None:
                    violations.append(
                        schema_violation(
                            f"lease {lease_id} is retired by both "
                            f"{previous} and {task_id}"
                        )
                    )
                retired_by[lease_id] = task_id
        for lease in change.get("temporary_leases", []):
            if not isinstance(lease, dict) or not isinstance(
                lease.get("id"), str
            ):
                continue
            lease_id = lease["id"]
            previous = leases.get(lease_id)
            if previous is not None:
                violations.append(
                    schema_violation(
                        f"duplicate temporary lease {lease_id} in "
                        f"{previous[0]} and {task_id}"
                    )
                )
            leases[lease_id] = (task_id, lease)

    for lease_id, task_id in retired_by.items():
        if lease_id not in leases:
            violations.append(
                schema_violation(
                    f"{task_id} retires unknown temporary lease {lease_id}"
                )
            )
    for marker_id, locations in markers.items():
        if marker_id not in leases:
            for path, line in locations:
                violations.append(
                    Violation(
                        path.as_posix(),
                        line,
                        f"temporary marker has no registered lease: {marker_id}",
                    )
                )

    for lease_id, (owner_task, lease) in leases.items():
        owner_state = str(records[owner_task].get("state", ""))
        lease_path = lease.get("path")
        removal_task = lease.get("remove_by_task")
        locations = markers.get(lease_id, [])
        matching = [
            location
            for location in locations
            if location[0].as_posix() == lease_path
        ]
        other = [location for location in locations if location not in matching]
        for path, line in other:
            violations.append(
                Violation(
                    path.as_posix(),
                    line,
                    f"lease {lease_id} belongs in {lease_path}",
                )
            )
        removal_record = records.get(str(removal_task))
        if removal_record is None:
            violations.append(
                schema_violation(
                    f"lease {lease_id} references unknown removal task "
                    f"{removal_task}"
                )
            )
            continue
        removal_state = str(removal_record.get("state", ""))
        removal_started = removal_state in CONCRETE_STATES
        if removal_started:
            if retired_by.get(lease_id) != removal_task:
                violations.append(
                    schema_violation(
                        f"{removal_task} must declare retirement of "
                        f"lease {lease_id}"
                    )
                )
            for path, line in matching:
                violations.append(
                    Violation(
                        path.as_posix(),
                        line,
                        f"lease {lease_id} survived removal task {removal_task}",
                    )
                )
        elif owner_state in CONCRETE_STATES and not matching:
            violations.append(
                schema_violation(
                    f"active lease {lease_id} has no marker in {lease_path}"
                )
            )
    return violations
