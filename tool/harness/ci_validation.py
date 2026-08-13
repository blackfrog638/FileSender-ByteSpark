#!/usr/bin/env python3
"""Validate live exact-candidate GitHub Actions results."""

from __future__ import annotations

import re
from typing import Any, Dict, List, Mapping, Sequence

from model import ContractSet, canonical_sha256


SHA = re.compile(r"^[0-9a-f]{40}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
WORKFLOW_FIELDS = {
    "repository",
    "workflow_path",
    "workflow_blob",
    "run_id",
    "run_attempt",
    "head_sha",
    "head_branch",
    "event",
    "conclusion",
    "jobs",
    "artifacts",
}
JOB_FIELDS = {"name", "conclusion"}
ARTIFACT_FIELDS = {
    "name",
    "source_sha",
    "sha256",
    "platform",
    "gate_ids",
    "gate_attestations",
    "criterion_ids",
    "criterion_evidence",
}
BOOTSTRAP_ARTIFACT_FIELDS = {
    "name",
    "artifact_id",
    "source_sha",
    "source_tree",
    "sha256",
    "platform",
    "plan_sha256",
    "gate_ids",
    "gate_attestations",
    "skipped",
}


class CIValidationError(RuntimeError):
    """Raised when a live hosted result cannot authorize publication."""


def _string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value.strip() or value != value.strip():
        raise CIValidationError("{} must be a non-empty string".format(label))
    return value


def _strings(value: Any, label: str, allow_empty: bool = False) -> List[str]:
    if not isinstance(value, list):
        raise CIValidationError("{} must be an array".format(label))
    result = [_string(item, "{}[]".format(label)) for item in value]
    if not allow_empty and not result:
        raise CIValidationError("{} must not be empty".format(label))
    if len(result) != len(set(result)):
        raise CIValidationError("{} contains duplicates".format(label))
    return result


def criterion_evidence_digest(
    contracts: ContractSet,
    criterion_id: str,
    source_sha: str,
    gate_attestations: Sequence[str],
) -> str:
    documents = {
        criterion["id"]: criterion
        for plan in contracts.plans.values()
        for requirement in plan["requirements"]
        for criterion in requirement["criteria"]
    }
    if criterion_id not in documents:
        raise CIValidationError("unknown criterion {}".format(criterion_id))
    return canonical_sha256(
        {
            "criterion_id": criterion_id,
            "criterion_sha256": canonical_sha256(documents[criterion_id]),
            "source_sha": source_sha,
            "gate_attestations": list(gate_attestations),
        }
    )


def _validate_identity(
    value: Any,
    *,
    repository: str,
    workflow_path: str,
    workflow_blob: str,
    candidate_sha: str,
    candidate_branch: str,
    required_jobs: Sequence[str],
) -> Dict[str, Any]:
    if not isinstance(value, dict) or set(value) != WORKFLOW_FIELDS:
        raise CIValidationError("workflow result has invalid fields")
    result = dict(value)
    expected = {
        "repository": repository,
        "workflow_path": workflow_path,
        "workflow_blob": workflow_blob,
        "head_sha": candidate_sha,
        "head_branch": candidate_branch,
        "event": "push",
        "conclusion": "success",
    }
    for field, wanted in expected.items():
        if result.get(field) != wanted:
            raise CIValidationError(
                "workflow result {} does not match candidate".format(field)
            )
    if (
        SHA.fullmatch(candidate_sha) is None
        or SHA.fullmatch(workflow_blob) is None
        or not isinstance(result["run_id"], int)
        or isinstance(result["run_id"], bool)
        or result["run_id"] < 1
        or not isinstance(result["run_attempt"], int)
        or isinstance(result["run_attempt"], bool)
        or result["run_attempt"] < 1
    ):
        raise CIValidationError("workflow run identity is invalid")
    jobs = result["jobs"]
    if not isinstance(jobs, list):
        raise CIValidationError("workflow jobs must be an array")
    outcomes: Dict[str, str] = {}
    for raw in jobs:
        if not isinstance(raw, dict) or set(raw) != JOB_FIELDS:
            raise CIValidationError("workflow job has invalid fields")
        name = _string(raw["name"], "workflow job name")
        if name in outcomes:
            raise CIValidationError("workflow job names are duplicated")
        outcomes[name] = _string(raw["conclusion"], "workflow job conclusion")
    skipped = sorted(name for name, value in outcomes.items() if value == "skipped")
    if skipped:
        raise CIValidationError(
            "workflow contains skipped jobs: {}".format(", ".join(skipped))
        )
    failed = sorted(name for name in required_jobs if outcomes.get(name) != "success")
    if failed:
        raise CIValidationError(
            "required workflow jobs did not succeed: {}".format(", ".join(failed))
        )
    return result


def validate_workflow_result(
    value: Any,
    *,
    repository: str,
    workflow_path: str,
    workflow_blob: str,
    candidate_sha: str,
    candidate_branch: str,
    required_jobs: Sequence[str],
    required_artifacts: Sequence[str],
) -> Dict[str, Any]:
    result = _validate_identity(
        value,
        repository=repository,
        workflow_path=workflow_path,
        workflow_blob=workflow_blob,
        candidate_sha=candidate_sha,
        candidate_branch=candidate_branch,
        required_jobs=required_jobs,
    )
    artifacts = result["artifacts"]
    if not isinstance(artifacts, list):
        raise CIValidationError("workflow artifacts must be an array")
    by_name: Dict[str, Dict[str, Any]] = {}
    for raw in artifacts:
        if not isinstance(raw, dict) or set(raw) != ARTIFACT_FIELDS:
            raise CIValidationError("workflow artifact has invalid fields")
        name = _string(raw["name"], "artifact name")
        if name in by_name:
            raise CIValidationError("artifact names are duplicated")
        if raw["source_sha"] != candidate_sha:
            raise CIValidationError("artifact {} has stale source SHA".format(name))
        if (
            not isinstance(raw["sha256"], str)
            or SHA256.fullmatch(raw["sha256"]) is None
            or raw["platform"] not in {"linux", "macos", "windows"}
        ):
            raise CIValidationError("artifact {} identity is invalid".format(name))
        gates = _strings(raw["gate_ids"], "{} Gate IDs".format(name))
        criteria = _strings(raw["criterion_ids"], "{} criterion IDs".format(name))
        gate_digests = _strings(
            raw["gate_attestations"],
            "{} Gate digests".format(name),
        )
        criterion_digests = _strings(
            raw["criterion_evidence"],
            "{} criterion digests".format(name),
        )
        if (
            len(gates) != len(gate_digests)
            or len(criteria) != len(criterion_digests)
            or any(
                SHA256.fullmatch(digest) is None
                for digest in gate_digests + criterion_digests
            )
        ):
            raise CIValidationError("artifact {} evidence is incomplete".format(name))
        by_name[name] = dict(raw)
    missing = sorted(set(required_artifacts) - set(by_name))
    if missing:
        raise CIValidationError(
            "required artifacts are missing: {}".format(", ".join(missing))
        )
    return result


def validate_criterion_results(
    contracts: ContractSet,
    workflow: Mapping[str, Any],
    *,
    candidate_sha: str,
    required_artifacts: Sequence[str],
    criterion_ids: Sequence[str],
) -> None:
    artifacts = {artifact["name"]: artifact for artifact in workflow["artifacts"]}
    for name in required_artifacts:
        artifact = artifacts[name]
        criterion_map = dict(
            zip(
                artifact["criterion_ids"],
                artifact["criterion_evidence"],
            )
        )
        for criterion_id in criterion_ids:
            expected = criterion_evidence_digest(
                contracts,
                criterion_id,
                candidate_sha,
                artifact["gate_attestations"],
            )
            if criterion_map.get(criterion_id) != expected:
                raise CIValidationError(
                    "artifact {} lacks criterion {}".format(name, criterion_id)
                )


def validate_bootstrap_workflow_result(
    value: Any,
    *,
    repository: str,
    workflow_path: str,
    workflow_blob: str,
    candidate_sha: str,
    candidate_tree: str,
    candidate_branch: str,
    required_jobs: Sequence[str],
    required_artifacts: Sequence[str],
    expected_plan_sha256: str,
    expected_gates: Mapping[str, Sequence[str]],
) -> Dict[str, Any]:
    result = _validate_identity(
        value,
        repository=repository,
        workflow_path=workflow_path,
        workflow_blob=workflow_blob,
        candidate_sha=candidate_sha,
        candidate_branch=candidate_branch,
        required_jobs=required_jobs,
    )
    if SHA.fullmatch(candidate_tree) is None:
        raise CIValidationError("bootstrap candidate tree is invalid")
    artifacts = result["artifacts"]
    if not isinstance(artifacts, list):
        raise CIValidationError("bootstrap artifacts must be an array")
    by_name: Dict[str, Dict[str, Any]] = {}
    platforms = set()
    for raw in artifacts:
        if not isinstance(raw, dict) or set(raw) != BOOTSTRAP_ARTIFACT_FIELDS:
            raise CIValidationError("bootstrap artifact has invalid fields")
        name = _string(raw["name"], "bootstrap artifact name")
        platform = raw["platform"]
        if (
            name in by_name
            or platform not in expected_gates
            or platform in platforms
            or raw["source_sha"] != candidate_sha
            or raw["source_tree"] != candidate_tree
            or raw["plan_sha256"] != expected_plan_sha256
            or raw["skipped"] is not False
            or not isinstance(raw["artifact_id"], int)
            or isinstance(raw["artifact_id"], bool)
            or raw["artifact_id"] < 1
            or not isinstance(raw["sha256"], str)
            or SHA256.fullmatch(raw["sha256"]) is None
        ):
            raise CIValidationError("bootstrap artifact binding is invalid")
        gates = _strings(raw["gate_ids"], "{} Gate IDs".format(name))
        digests = _strings(
            raw["gate_attestations"],
            "{} Gate digests".format(name),
        )
        if (
            gates != list(expected_gates[platform])
            or len(gates) != len(digests)
            or any(SHA256.fullmatch(digest) is None for digest in digests)
        ):
            raise CIValidationError(
                "{} bootstrap Gate result is incomplete".format(platform)
            )
        platforms.add(platform)
        by_name[name] = dict(raw)
    if set(by_name) != set(required_artifacts):
        raise CIValidationError("bootstrap artifact set is incomplete")
    if platforms != set(expected_gates):
        raise CIValidationError("bootstrap platform matrix is incomplete")
    return result
