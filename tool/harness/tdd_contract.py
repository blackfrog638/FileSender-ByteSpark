#!/usr/bin/env python3

"""Validate future-task TDD contracts without executing their proof."""

from __future__ import annotations

import fnmatch
import json
import re
from pathlib import Path
from typing import Any


CRITERION_ID_PATTERN = re.compile(
    r"^CRIT-[A-Z0-9]+(?:-[A-Z0-9]+)*$"
)
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
PATH_MAGIC_PATTERN = re.compile(r"[*?\[]")
TEST_CONTRACT_FIELDS = {
    "schema_version",
    "plan_content_sha256",
    "criterion_ids",
    "proof_mode",
    "executor",
    "gate",
    "proof_surface",
    "failure_fingerprints",
    "allow_skipped",
}
PROOF_MODES_BY_TASK_TYPE = {
    "feature": "red_green",
    "bugfix": "regression",
    "refactor": "equivalence",
    "test": "mutation",
    "governance": "red_green",
    "investigation": "bounded_evidence",
}
EXECUTORS = {
    "deterministic",
    "sanitizer",
    "stress",
    "platform_ci",
    "manual_witness",
}
FAILING_PROOF_MODES = {"red_green", "regression", "mutation"}
SURFACE_PROOF_MODES = FAILING_PROOF_MODES | {"equivalence"}


def _load_json(path: Path) -> dict[str, Any] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def _string_list(
    errors: list[str],
    value: Any,
    label: str,
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


def _literal_prefix(pattern: str) -> tuple[str, ...]:
    parts: list[str] = []
    for part in pattern.split("/"):
        if PATH_MAGIC_PATTERN.search(part):
            break
        parts.append(part)
    return tuple(parts)


def _is_normalized_repo_pattern(pattern: str) -> bool:
    if pattern.startswith("/") or "\\" in pattern:
        return False
    parts = pattern.split("/")
    return bool(parts) and all(part not in {"", ".", ".."} for part in parts)


def _recursive_owned_prefix(owned_path: str) -> tuple[str, ...] | None:
    if not owned_path.endswith("/**"):
        return None
    root = owned_path.removesuffix("/**")
    if not root or PATH_MAGIC_PATTERN.search(root):
        return None
    return tuple(root.split("/"))


def _proof_path_is_owned(proof_path: str, owned_path: str) -> bool:
    if not (
        _is_normalized_repo_pattern(proof_path)
        and _is_normalized_repo_pattern(owned_path)
    ):
        return False
    if proof_path == owned_path:
        return True
    if not PATH_MAGIC_PATTERN.search(proof_path):
        return fnmatch.fnmatchcase(proof_path, owned_path)
    proof_prefix = _literal_prefix(proof_path)
    owned_prefix = _recursive_owned_prefix(owned_path)
    return (
        owned_prefix is not None
        and len(proof_prefix) >= len(owned_prefix)
        and proof_prefix[: len(owned_prefix)] == owned_prefix
    )


def expected_proof_mode(task_type: str, delivery_role: str) -> str:
    if delivery_role == "acceptance":
        return "evidence_closure"
    return PROOF_MODES_BY_TASK_TYPE.get(task_type, "")


def _criterion_ids_for_task(
    plan: dict[str, Any],
    task_id: str,
    requirement_ids: set[str],
    delivery_role: str,
) -> set[str]:
    result: set[str] = set()
    requirements = plan.get("requirements")
    if not isinstance(requirements, list):
        return result
    for requirement in requirements:
        if (
            not isinstance(requirement, dict)
            or requirement.get("id") not in requirement_ids
        ):
            continue
        acceptance_task = requirement.get("acceptance_task")
        criteria = requirement.get("criteria")
        if not isinstance(criteria, list):
            continue
        for criterion in criteria:
            if not isinstance(criterion, dict):
                continue
            criterion_id = criterion.get("id")
            implementations = criterion.get("implementation_tasks")
            implements = (
                isinstance(implementations, list)
                and task_id in implementations
            )
            accepts = (
                delivery_role in {"acceptance", "implementation_acceptance"}
                and acceptance_task == task_id
            )
            if isinstance(criterion_id, str) and (implements or accepts):
                result.add(criterion_id)
    return result


def validate_test_contract(
    root: Path,
    task: dict[str, Any],
    record: dict[str, Any],
    gate_registry: dict[str, str],
) -> list[str]:
    errors: list[str] = []
    task_id = str(task.get("id", "<unknown>"))
    label = f"{task_id}.test_contract"
    raw_contract = record.get("test_contract")
    if not isinstance(raw_contract, dict):
        return [f"{label} must be an object"]
    missing = sorted(TEST_CONTRACT_FIELDS - set(raw_contract))
    unknown = sorted(set(raw_contract) - TEST_CONTRACT_FIELDS)
    if missing:
        errors.append(f"{label} is missing fields: {', '.join(missing)}")
    if unknown:
        errors.append(f"{label} has unknown fields: {', '.join(unknown)}")

    if raw_contract.get("schema_version") != 1:
        errors.append(f"{label}.schema_version must be 1")

    plan_id = task.get("delivery_plan")
    plan = (
        _load_json(root / ".agents" / "plans" / f"{plan_id}.json")
        if isinstance(plan_id, str)
        else None
    )
    if plan is None:
        errors.append(f"{label} cannot load Delivery Plan {plan_id!r}")
        plan = {}
    elif plan.get("schema_version") != 2:
        errors.append(f"{label} requires Delivery Plan schema version 2")
    elif plan.get("status") != "approved":
        errors.append(f"{label} requires an approved Delivery Plan")

    plan_digest = raw_contract.get("plan_content_sha256")
    if (
        not isinstance(plan_digest, str)
        or not SHA256_PATTERN.fullmatch(plan_digest)
    ):
        errors.append(f"{label}.plan_content_sha256 must be a SHA-256 digest")
    approval = plan.get("approval")
    approved_digest = (
        approval.get("content_sha256")
        if isinstance(approval, dict)
        else None
    )
    if plan_digest != approved_digest:
        errors.append(
            f"{label}.plan_content_sha256 does not match approved plan"
        )

    criterion_ids = _string_list(
        errors,
        raw_contract.get("criterion_ids"),
        f"{label}.criterion_ids",
        nonempty=True,
    )
    for criterion_id in criterion_ids:
        if not CRITERION_ID_PATTERN.fullmatch(criterion_id):
            errors.append(
                f"{label}.criterion_ids contains invalid id {criterion_id}"
            )
    expected_criteria = _criterion_ids_for_task(
        plan,
        task_id,
        set(task.get("requirement_ids", [])),
        str(task.get("delivery_role", "")),
    )
    if set(criterion_ids) != expected_criteria:
        errors.append(
            f"{label}.criterion_ids do not match task mappings: "
            f"expected {sorted(expected_criteria)}"
        )

    proof_mode = raw_contract.get("proof_mode")
    expected_mode = expected_proof_mode(
        str(record.get("task_type", "")),
        str(task.get("delivery_role", "")),
    )
    if proof_mode != expected_mode:
        errors.append(f"{label}.proof_mode must be {expected_mode or 'valid'}")

    executor = raw_contract.get("executor")
    if executor not in EXECUTORS:
        errors.append(
            f"{label}.executor must be one of {sorted(EXECUTORS)}"
        )

    gate = raw_contract.get("gate")
    if not isinstance(gate, str) or gate not in gate_registry:
        errors.append(f"{label}.gate is not registered")
    verification = record.get("verification")
    verification_gates = (
        verification.get("gates")
        if isinstance(verification, dict)
        else None
    )
    if (
        isinstance(gate, str)
        and (
            not isinstance(verification_gates, list)
            or gate not in verification_gates
        )
    ):
        errors.append(f"{label}.gate must appear in verification.gates")

    proof_surface = _string_list(
        errors,
        raw_contract.get("proof_surface"),
        f"{label}.proof_surface",
        nonempty=proof_mode in SURFACE_PROOF_MODES,
    )
    owned_paths = task.get("owned_paths")
    owned = (
        [path for path in owned_paths if isinstance(path, str)]
        if isinstance(owned_paths, list)
        else []
    )
    for proof_path in proof_surface:
        if not any(
            _proof_path_is_owned(proof_path, owned_path)
            for owned_path in owned
        ):
            errors.append(
                f"{label}.proof_surface is outside task ownership: "
                f"{proof_path}"
            )

    fingerprints = _string_list(
        errors,
        raw_contract.get("failure_fingerprints"),
        f"{label}.failure_fingerprints",
        nonempty=proof_mode in FAILING_PROOF_MODES,
    )
    if proof_mode not in FAILING_PROOF_MODES and fingerprints:
        errors.append(
            f"{label}.failure_fingerprints must be empty for {proof_mode}"
        )
    for fingerprint in fingerprints:
        if (
            fingerprint != fingerprint.strip()
            or "\n" in fingerprint
            or "\r" in fingerprint
            or not 16 <= len(fingerprint) <= 256
        ):
            errors.append(
                f"{label}.failure_fingerprints entries must be "
                "16-256 concrete single-line characters"
            )

    if raw_contract.get("allow_skipped") is not False:
        errors.append(f"{label}.allow_skipped must be false")
    return errors
