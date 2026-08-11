#!/usr/bin/env python3

"""Collect and validate criterion evidence for an exact integrated candidate."""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
from collections.abc import Callable
from io import BytesIO
from pathlib import Path, PurePosixPath
from typing import Any

from trusted_gates import GateRegistryError, load_gate_registry


SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
CRITERION_PATTERN = re.compile(r"^CRIT-[A-Z0-9]+(?:-[A-Z0-9]+)*$")
EVIDENCE_PATTERN = re.compile(r"^EVD-[A-Z0-9]+(?:-[A-Z0-9]+)*$")
TASK_PATTERN = re.compile(r"^XT-[0-9]{3,}$")
SAFE_VALUE_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
RUN_URL_PATTERN = re.compile(
    r"^https://github\.com/([^/]+)/([^/]+)/actions/runs/([1-9][0-9]*)$"
)
JOB_URL_PATTERN = re.compile(
    r"^https://github\.com/([^/]+)/([^/]+)/actions/runs/"
    r"([1-9][0-9]*)/job/([1-9][0-9]*)$"
)
ARTIFACT_API_URL_PATTERN = re.compile(
    r"^https://api\.github\.com/repos/[^/]+/[^/]+/actions/artifacts/"
    r"([1-9][0-9]*)/zip$"
)
MAX_ARTIFACT_BYTES = 512 * 1024 * 1024
MAX_UNCOMPRESSED_BYTES = 1024 * 1024 * 1024
MAX_ARCHIVE_ENTRIES = 256
MAX_MANIFEST_BYTES = 1024 * 1024
REAL_TRANSPORTS = {"tcp", "tcp_tls", "quic", "platform"}
TRANSPORTS = REAL_TRANSPORTS | {
    "none",
    "loopback",
    "in_memory",
    "fake_gateway",
}
RUNNER_BY_PLATFORM = {
    None: "ubuntu-latest",
    "linux": "ubuntu-latest",
    "ubuntu": "ubuntu-latest",
    "ubuntu-latest": "ubuntu-latest",
    "macos": "macos-latest",
    "macos-latest": "macos-latest",
    "windows": "windows-2022",
    "windows-2022": "windows-2022",
}
EVIDENCE_LEVELS = {
    "unit",
    "integration",
    "contract",
    "snapshot",
    "smoke",
    "e2e",
    "security",
    "reliability",
    "performance",
    "manual",
}
EVIDENCE_TOPOLOGIES = {
    "in_process",
    "real_process",
    "two_process",
    "packaged_e2e",
    "remote_ci",
}

BUNDLE_FIELDS = {
    "schema_version",
    "plan_id",
    "plan_content_sha256",
    "source_sha",
    "workflow_sha256",
    "run_id",
    "run_attempt",
    "run_url",
    "criteria",
    "bundle_sha256",
}
CRITERION_FIELDS = {"criterion_id", "evidence"}
EVIDENCE_FIELDS = {
    "evidence_id",
    "producer_task",
    "gate",
    "gate_command_sha256",
    "level",
    "scenarios",
    "assertions",
    "topology",
    "allow_skipped",
    "result",
    "run_url",
    "matrix",
}
MATRIX_FIELDS = {
    "platform",
    "role",
    "job_id",
    "job_run_attempt",
    "job_name",
    "job_url",
    "job_conclusion",
    "artifact_id",
    "artifact_name",
    "artifact_size",
    "artifact_sha256",
    "binary_digests",
    "runtime",
    "result",
    "skipped",
}
BINARY_FIELDS = {"path", "sha256"}
RUNTIME_FIELDS = {
    "process_count",
    "transport",
    "authenticated",
    "packaged",
}
WITNESS_FIELDS = {"runtime", "binaries"}
MANIFEST_FIELDS = {
    "schema_version",
    "criterion_id",
    "evidence_id",
    "source_sha",
    "run_attempt",
    "gate",
    "scenarios",
    "assertions",
    "topology",
    "platform",
    "role",
    "job_name",
    "result",
    "skipped",
    "runtime",
    "binaries",
}


class EvidenceError(RuntimeError):
    pass


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_digest(value: dict[str, Any]) -> str:
    encoded = json.dumps(
        value,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    return sha256_bytes(encoded)


def _require_exact(
    value: Any,
    fields: set[str],
    label: str,
) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise EvidenceError(f"{label} must be an object")
    missing = sorted(fields - set(value))
    unknown = sorted(set(value) - fields)
    if missing:
        raise EvidenceError(f"{label} is missing fields: {', '.join(missing)}")
    if unknown:
        raise EvidenceError(f"{label} has unknown fields: {', '.join(unknown)}")
    return value


def _string_list(
    value: Any,
    label: str,
    *,
    nonempty: bool,
) -> list[str]:
    if not isinstance(value, list) or not all(
        isinstance(item, str) and item for item in value
    ):
        raise EvidenceError(f"{label} must be an array of nonempty strings")
    if nonempty and not value:
        raise EvidenceError(f"{label} must not be empty")
    if len(value) != len(set(value)):
        raise EvidenceError(f"{label} contains duplicates")
    if len(value) > 128 or any(len(item) > 128 for item in value):
        raise EvidenceError(f"{label} exceeds the evidence size limit")
    return value


def _safe_optional(value: Any, label: str) -> str | None:
    if value is None:
        return None
    if not isinstance(value, str) or not SAFE_VALUE_PATTERN.fullmatch(value):
        raise EvidenceError(f"{label} must be null or a safe matrix value")
    return value


def _load_contracts(
    task: dict[str, Any],
    record: dict[str, Any],
    plan: dict[str, Any],
    gate_registry: dict[str, str],
) -> list[tuple[str, dict[str, Any]]]:
    task_id = task.get("id")
    if not isinstance(task_id, str):
        raise EvidenceError("task id is invalid")
    if plan.get("schema_version") != 2 or plan.get("status") != "approved":
        raise EvidenceError("criterion evidence requires an approved schema-v2 plan")
    approval = plan.get("approval")
    plan_digest = (
        approval.get("content_sha256")
        if isinstance(approval, dict)
        else None
    )
    test_contract = record.get("test_contract")
    if not isinstance(test_contract, dict):
        raise EvidenceError("schema-v4 record has no test contract")
    if test_contract.get("plan_content_sha256") != plan_digest:
        raise EvidenceError("test contract does not bind the approved plan")
    criterion_ids = _string_list(
        test_contract.get("criterion_ids"),
        "test_contract.criterion_ids",
        nonempty=True,
    )
    wanted = set(criterion_ids)
    selected: list[tuple[str, dict[str, Any]]] = []
    seen_criteria: set[str] = set()
    seen_evidence: set[str] = set()
    acceptance = task.get("delivery_role") in {
        "acceptance",
        "implementation_acceptance",
    }
    requirements = plan.get("requirements")
    if not isinstance(requirements, list):
        raise EvidenceError("approved plan has no requirements array")
    for requirement in requirements:
        if not isinstance(requirement, dict):
            continue
        criteria = requirement.get("criteria")
        if not isinstance(criteria, list):
            continue
        for criterion in criteria:
            if not isinstance(criterion, dict):
                continue
            criterion_id = criterion.get("id")
            if criterion_id not in wanted:
                continue
            if (
                not isinstance(criterion_id, str)
                or not CRITERION_PATTERN.fullmatch(criterion_id)
                or criterion_id in seen_criteria
            ):
                raise EvidenceError("criterion mapping is invalid or duplicated")
            seen_criteria.add(criterion_id)
            raw_evidence = criterion.get("evidence")
            if not isinstance(raw_evidence, list) or not raw_evidence:
                raise EvidenceError(
                    f"{criterion_id} has no approved evidence contracts"
                )
            if len(raw_evidence) > 128:
                raise EvidenceError(
                    f"{criterion_id} has too many evidence contracts"
                )
            criterion_selected = 0
            for contract in raw_evidence:
                if not isinstance(contract, dict):
                    raise EvidenceError(
                        f"{criterion_id} evidence contract is malformed"
                    )
                producer = contract.get("producer_task")
                if not acceptance and producer != task_id:
                    continue
                evidence_id = contract.get("id")
                if (
                    not isinstance(evidence_id, str)
                    or not EVIDENCE_PATTERN.fullmatch(evidence_id)
                    or evidence_id in seen_evidence
                ):
                    raise EvidenceError(
                        "evidence mapping is invalid or duplicated"
                    )
                seen_evidence.add(evidence_id)
                gate = contract.get("gate")
                if not isinstance(gate, str) or gate not in gate_registry:
                    raise EvidenceError(f"{evidence_id} gate is not trusted")
                verification = record.get("verification")
                executed_gates = (
                    verification.get("gates")
                    if isinstance(verification, dict)
                    else None
                )
                if not isinstance(executed_gates, list) or gate not in executed_gates:
                    raise EvidenceError(
                        f"{evidence_id} gate is not an acceptance gate"
                    )
                if gate == "verify":
                    raise EvidenceError(
                        "generic verify cannot satisfy specialized criterion evidence"
                    )
                if contract.get("allow_skipped") is not False:
                    raise EvidenceError(f"{evidence_id} must reject skipped evidence")
                producer = contract.get("producer_task")
                if not isinstance(producer, str) or not TASK_PATTERN.fullmatch(
                    producer
                ):
                    raise EvidenceError(f"{evidence_id} producer task is invalid")
                _string_list(
                    contract.get("required_scenarios"),
                    f"{evidence_id}.required_scenarios",
                    nonempty=True,
                )
                _string_list(
                    contract.get("required_assertions"),
                    f"{evidence_id}.required_assertions",
                    nonempty=True,
                )
                platforms = _string_list(
                    contract.get("required_platforms"),
                    f"{evidence_id}.required_platforms",
                    nonempty=False,
                )
                roles = _string_list(
                    contract.get("required_roles"),
                    f"{evidence_id}.required_roles",
                    nonempty=False,
                )
                for platform in platforms:
                    _safe_optional(platform, f"{evidence_id} platform")
                for role in roles:
                    _safe_optional(role, f"{evidence_id} role")
                if max(1, len(platforms)) * max(1, len(roles)) > 64:
                    raise EvidenceError(f"{evidence_id} matrix exceeds 64 jobs")
                if contract.get("level") not in EVIDENCE_LEVELS:
                    raise EvidenceError(f"{evidence_id} level is invalid")
                if contract.get("topology") not in EVIDENCE_TOPOLOGIES:
                    raise EvidenceError(f"{evidence_id} topology is invalid")
                selected.append((criterion_id, contract))
                criterion_selected += 1
            if acceptance and criterion_selected == 0:
                raise EvidenceError(
                    f"{criterion_id} has no approved evidence for acceptance"
                )
    if seen_criteria != wanted:
        raise EvidenceError("record criterion IDs do not match approved plan")
    return selected


def _complete_response(
    payload: dict[str, Any],
    array_field: str,
    label: str,
) -> list[dict[str, Any]]:
    values = payload.get(array_field)
    total = payload.get("total_count")
    if (
        not isinstance(total, int)
        or total < 0
        or not isinstance(values, list)
        or total != len(values)
        or not all(isinstance(item, dict) for item in values)
    ):
        raise EvidenceError(f"{label} response is incomplete or malformed")
    if total > 100:
        raise EvidenceError(f"{label} response exceeds one complete API page")
    return values


def _validate_run(
    run: dict[str, Any],
    task_id: str,
    source_sha: str,
) -> tuple[int, int, str]:
    run_id = run.get("id")
    run_attempt = run.get("run_attempt")
    run_url = run.get("html_url")
    if (
        not isinstance(run_id, int)
        or run_id <= 0
        or not isinstance(run_attempt, int)
        or run_attempt <= 0
        or run.get("event") != "push"
        or run.get("head_branch") != f"ci/{task_id}"
        or run.get("head_sha") != source_sha
    ):
        raise EvidenceError("workflow run does not bind the exact source SHA")
    if run.get("status") != "completed" or run.get("conclusion") != "success":
        raise EvidenceError("workflow run is not successfully completed")
    match = RUN_URL_PATTERN.fullmatch(run_url) if isinstance(run_url, str) else None
    if match is None or int(match.group(3)) != run_id:
        raise EvidenceError("workflow run URL does not bind its run id")
    return run_id, run_attempt, run_url


def _safe_archive_path(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value or len(value) > 240:
        raise EvidenceError(f"{label} is invalid")
    if "\\" in value:
        raise EvidenceError(f"{label} is not a normalized archive path")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise EvidenceError(f"{label} is not a normalized archive path")
    return value


def _parse_archive(
    archive_bytes: bytes,
) -> tuple[dict[str, Any], list[dict[str, str]]]:
    if len(archive_bytes) > MAX_ARTIFACT_BYTES:
        raise EvidenceError("artifact exceeds the compressed size limit")
    try:
        archive = zipfile.ZipFile(BytesIO(archive_bytes))
    except zipfile.BadZipFile as error:
        raise EvidenceError("criterion artifact is not a valid ZIP archive") from error
    with archive:
        infos = archive.infolist()
        if not infos or len(infos) > MAX_ARCHIVE_ENTRIES:
            raise EvidenceError("criterion artifact has an invalid entry count")
        names = [info.filename for info in infos if not info.is_dir()]
        if len(names) != len(set(names)):
            raise EvidenceError("criterion artifact contains duplicate paths")
        for name in names:
            _safe_archive_path(name, "artifact path")
        total_size = sum(info.file_size for info in infos)
        if total_size > MAX_UNCOMPRESSED_BYTES:
            raise EvidenceError("criterion artifact exceeds the expansion limit")
        if names.count("evidence.json") != 1:
            raise EvidenceError("criterion artifact must contain evidence.json")
        info = archive.getinfo("evidence.json")
        if info.file_size > MAX_MANIFEST_BYTES:
            raise EvidenceError("criterion evidence manifest is too large")
        try:
            manifest = json.loads(archive.read(info))
        except (
            UnicodeDecodeError,
            json.JSONDecodeError,
            zipfile.BadZipFile,
            RuntimeError,
            NotImplementedError,
        ) as error:
            raise EvidenceError("criterion evidence manifest is malformed") from error
        manifest = _require_exact(manifest, MANIFEST_FIELDS, "artifact manifest")
        if manifest.get("schema_version") != 1:
            raise EvidenceError("artifact manifest schema_version must be 1")
        binaries = manifest.get("binaries")
        if not isinstance(binaries, list) or len(binaries) > 64:
            raise EvidenceError("artifact manifest binaries must be a bounded array")
        parsed_binaries: list[dict[str, str]] = []
        binary_paths: set[str] = set()
        for index, raw_binary in enumerate(binaries):
            binary = _require_exact(
                raw_binary,
                BINARY_FIELDS,
                f"artifact manifest binaries[{index}]",
            )
            path = _safe_archive_path(
                binary.get("path"),
                f"artifact manifest binaries[{index}].path",
            )
            digest = binary.get("sha256")
            if not isinstance(digest, str) or not SHA256_PATTERN.fullmatch(digest):
                raise EvidenceError("artifact binary digest is malformed")
            if path in binary_paths or path == "evidence.json":
                raise EvidenceError("artifact binary paths contain duplicates")
            binary_paths.add(path)
            try:
                actual_digest = hashlib.sha256()
                with archive.open(path) as content:
                    while chunk := content.read(1024 * 1024):
                        actual_digest.update(chunk)
            except (
                KeyError,
                zipfile.BadZipFile,
                RuntimeError,
                NotImplementedError,
            ) as error:
                raise EvidenceError(
                    "artifact binary is missing or unreadable"
                ) from error
            if actual_digest.hexdigest() != digest:
                raise EvidenceError("artifact binary digest does not match content")
            parsed_binaries.append({"path": path, "sha256": digest})
        if set(names) != {"evidence.json"} | binary_paths:
            raise EvidenceError("criterion artifact contains undeclared files")
    return manifest, parsed_binaries


def _validate_runtime(
    raw_runtime: Any,
    *,
    topology: str,
    level: str,
    has_binaries: bool,
) -> dict[str, Any]:
    runtime = _require_exact(raw_runtime, RUNTIME_FIELDS, "artifact runtime")
    process_count = runtime.get("process_count")
    transport = runtime.get("transport")
    authenticated = runtime.get("authenticated")
    packaged = runtime.get("packaged")
    if not isinstance(process_count, int) or not 1 <= process_count <= 64:
        raise EvidenceError("artifact runtime process_count is invalid")
    if transport not in TRANSPORTS:
        raise EvidenceError("artifact runtime transport is invalid")
    if not isinstance(authenticated, bool) or not isinstance(packaged, bool):
        raise EvidenceError("artifact runtime booleans are invalid")
    if topology == "two_process" and process_count < 2:
        raise EvidenceError("two-process evidence requires at least two processes")
    if topology == "real_process" and process_count < 1:
        raise EvidenceError("real-process evidence requires a process")
    if topology == "packaged_e2e" and not (
        process_count >= 2
        and transport in REAL_TRANSPORTS
        and authenticated
        and packaged
        and has_binaries
    ):
        raise EvidenceError(
            "packaged E2E requires two processes, authenticated real "
            "transport, and packaged binaries"
        )
    if level == "e2e" and not (
        process_count >= 2
        and transport in REAL_TRANSPORTS
        and authenticated
    ):
        raise EvidenceError(
            "E2E evidence requires two processes and authenticated real transport"
        )
    return {
        "process_count": process_count,
        "transport": transport,
        "authenticated": authenticated,
        "packaged": packaged,
    }


def _job_for_manifest(
    jobs: list[dict[str, Any]],
    manifest: dict[str, Any],
    source_sha: str,
    run_id: int,
    run_attempt: int,
    gate: str,
) -> dict[str, Any]:
    job_name = manifest.get("job_name")
    if (
        not isinstance(job_name, str)
        or not 1 <= len(job_name) <= 200
        or "\n" in job_name
        or "\r" in job_name
    ):
        raise EvidenceError("artifact job name is invalid")
    matches = [job for job in jobs if job.get("name") == job_name]
    if len(matches) != 1:
        raise EvidenceError("criterion artifact has no unique required job")
    job = matches[0]
    job_id = job.get("id")
    if (
        not isinstance(job_id, int)
        or job_id <= 0
        or job.get("run_attempt") != run_attempt
        or job.get("head_sha") != source_sha
        or job.get("status") != "completed"
        or job.get("conclusion") != "success"
    ):
        raise EvidenceError("criterion artifact is not bound to a successful job")
    job_url = job.get("html_url")
    match = (
        JOB_URL_PATTERN.fullmatch(job_url)
        if isinstance(job_url, str)
        else None
    )
    if (
        match is None
        or int(match.group(3)) != run_id
        or int(match.group(4)) != job_id
    ):
        raise EvidenceError("criterion job URL does not bind run and job ids")
    steps = job.get("steps")
    if not isinstance(steps, list):
        raise EvidenceError("criterion job has no structured steps")
    expected_step = f"Evidence gate: {gate}"
    matching_steps = [
        step
        for step in steps
        if isinstance(step, dict) and step.get("name") == expected_step
    ]
    if len(matching_steps) != 1 or not (
        matching_steps[0].get("status") == "completed"
        and matching_steps[0].get("conclusion") == "success"
    ):
        raise EvidenceError("criterion gate step is missing, skipped, or failed")
    return job


def _artifact_entry(
    *,
    artifact: dict[str, Any],
    archive_bytes: bytes,
    manifest: dict[str, Any],
    binaries: list[dict[str, str]],
    job: dict[str, Any],
    runtime: dict[str, Any],
    run_id: int,
    run_attempt: int,
    source_sha: str,
) -> dict[str, Any]:
    artifact_id = artifact.get("id")
    name = artifact.get("name")
    size = artifact.get("size_in_bytes")
    digest = artifact.get("digest")
    download_url = artifact.get("archive_download_url")
    workflow_run = artifact.get("workflow_run")
    if not isinstance(artifact_id, int) or artifact_id <= 0:
        raise EvidenceError("criterion artifact id is invalid")
    if (
        not isinstance(name, str)
        or not name.startswith("criterion-")
        or len(name) > 200
        or "\n" in name
        or "\r" in name
    ):
        raise EvidenceError("criterion artifact name is invalid")
    if not isinstance(size, int) or not 0 < size <= MAX_ARTIFACT_BYTES:
        raise EvidenceError("criterion artifact size is invalid")
    if artifact.get("expired") is not False:
        raise EvidenceError("criterion artifact is expired")
    if (
        not isinstance(digest, str)
        or not digest.startswith("sha256:")
        or not SHA256_PATTERN.fullmatch(digest.removeprefix("sha256:"))
        or digest.removeprefix("sha256:") != sha256_bytes(archive_bytes)
    ):
        raise EvidenceError("artifact digest does not match downloaded content")
    url_match = (
        ARTIFACT_API_URL_PATTERN.fullmatch(download_url)
        if isinstance(download_url, str)
        else None
    )
    if url_match is None or int(url_match.group(1)) != artifact_id:
        raise EvidenceError("criterion artifact download URL is invalid")
    if not isinstance(workflow_run, dict) or (
        workflow_run.get("id") != run_id
        or workflow_run.get("head_sha") != source_sha
    ):
        raise EvidenceError("criterion artifact does not bind the exact source SHA")
    return {
        "platform": manifest["platform"],
        "role": manifest["role"],
        "job_id": job["id"],
        "job_run_attempt": run_attempt,
        "job_name": job["name"],
        "job_url": job["html_url"],
        "job_conclusion": "success",
        "artifact_id": artifact_id,
        "artifact_name": name,
        "artifact_size": size,
        "artifact_sha256": sha256_bytes(archive_bytes),
        "binary_digests": sorted(binaries, key=lambda item: item["path"]),
        "runtime": runtime,
        "result": "passed",
        "skipped": False,
    }


def collect_bundle(
    *,
    task: dict[str, Any],
    record: dict[str, Any],
    plan: dict[str, Any],
    source_sha: str,
    workflow_bytes: bytes,
    gate_registry: dict[str, str],
    run: dict[str, Any],
    jobs_payload: dict[str, Any],
    artifacts_payload: dict[str, Any],
    download_artifact: Callable[[int], bytes],
) -> dict[str, Any]:
    task_id = task.get("id")
    if not isinstance(task_id, str) or not SHA_PATTERN.fullmatch(source_sha):
        raise EvidenceError("task id or source SHA is invalid")
    integration = record.get("integration")
    if (
        not isinstance(integration, dict)
        or integration.get("verified_sha") != source_sha
    ):
        raise EvidenceError("record integration does not bind the source SHA")
    contracts = _load_contracts(task, record, plan, gate_registry)
    run_id, run_attempt, run_url = _validate_run(run, task_id, source_sha)
    jobs = _complete_response(jobs_payload, "jobs", "job")
    artifacts = _complete_response(
        artifacts_payload,
        "artifacts",
        "artifact",
    )
    contract_by_id = {
        contract["id"]: (criterion_id, contract)
        for criterion_id, contract in contracts
    }
    entries: dict[str, list[dict[str, Any]]] = {
        evidence_id: [] for evidence_id in contract_by_id
    }
    used_artifact_ids: set[int] = set()
    for artifact in artifacts:
        name = artifact.get("name")
        if not isinstance(name, str) or not name.startswith("criterion-"):
            continue
        artifact_id = artifact.get("id")
        if not isinstance(artifact_id, int) or artifact_id in used_artifact_ids:
            raise EvidenceError("criterion artifact ids are invalid or duplicated")
        used_artifact_ids.add(artifact_id)
        archive_bytes = download_artifact(artifact_id)
        if not isinstance(archive_bytes, bytes):
            raise EvidenceError("artifact downloader returned non-bytes content")
        manifest, binaries = _parse_archive(archive_bytes)
        evidence_id = manifest.get("evidence_id")
        contract_entry = (
            contract_by_id.get(evidence_id)
            if isinstance(evidence_id, str)
            else None
        )
        if contract_entry is None:
            raise EvidenceError("criterion artifact names unapproved evidence")
        criterion_id, contract = contract_entry
        if manifest.get("criterion_id") != criterion_id:
            raise EvidenceError("criterion artifact maps to the wrong criterion")
        if manifest.get("source_sha") != source_sha:
            raise EvidenceError("criterion artifact has a stale source SHA")
        if manifest.get("run_attempt") != run_attempt:
            raise EvidenceError("criterion artifact has a stale run attempt")
        gate = contract.get("gate")
        scenarios = _string_list(
            manifest.get("scenarios"),
            "artifact scenarios",
            nonempty=True,
        )
        assertions = _string_list(
            manifest.get("assertions"),
            "artifact assertions",
            nonempty=True,
        )
        if (
            manifest.get("gate") != gate
            or scenarios != contract.get("required_scenarios")
            or assertions != contract.get("required_assertions")
            or manifest.get("topology") != contract.get("topology")
            or manifest.get("result") != "passed"
            or manifest.get("skipped") is not False
        ):
            raise EvidenceError(
                "criterion artifact does not match its approved evidence contract"
            )
        platform = _safe_optional(manifest.get("platform"), "artifact platform")
        role = _safe_optional(manifest.get("role"), "artifact role")
        required_platforms = contract.get("required_platforms")
        required_roles = contract.get("required_roles")
        if not isinstance(required_platforms, list) or not isinstance(
            required_roles,
            list,
        ):
            raise EvidenceError("evidence matrix contract is malformed")
        allowed_platforms = required_platforms or [None]
        allowed_roles = required_roles or [None]
        if platform not in allowed_platforms or role not in allowed_roles:
            raise EvidenceError("criterion artifact is outside the required matrix")
        level = contract.get("level")
        if not isinstance(level, str):
            raise EvidenceError("criterion evidence level is invalid")
        runtime = _validate_runtime(
            manifest.get("runtime"),
            topology=str(contract.get("topology")),
            level=level,
            has_binaries=bool(binaries),
        )
        job = _job_for_manifest(
            jobs,
            manifest,
            source_sha,
            run_id,
            run_attempt,
            str(gate),
        )
        entries[str(evidence_id)].append(
            _artifact_entry(
                artifact=artifact,
                archive_bytes=archive_bytes,
                manifest=manifest,
                binaries=binaries,
                job=job,
                runtime=runtime,
                run_id=run_id,
                run_attempt=run_attempt,
                source_sha=source_sha,
            )
        )

    grouped: dict[str, list[dict[str, Any]]] = {}
    for criterion_id, contract in contracts:
        evidence_id = str(contract["id"])
        matrix = entries[evidence_id]
        platforms = contract.get("required_platforms") or [None]
        roles = contract.get("required_roles") or [None]
        required_matrix = set(itertools.product(platforms, roles))
        actual_matrix = {
            (entry["platform"], entry["role"]) for entry in matrix
        }
        if (
            actual_matrix != required_matrix
            or len(matrix) != len(required_matrix)
        ):
            raise EvidenceError(
                f"{evidence_id} does not cover the exact required matrix"
            )
        command = gate_registry[str(contract["gate"])]
        proof = {
            "evidence_id": evidence_id,
            "producer_task": contract["producer_task"],
            "gate": contract["gate"],
            "gate_command_sha256": sha256_bytes(command.encode("utf-8")),
            "level": contract["level"],
            "scenarios": contract["required_scenarios"],
            "assertions": contract["required_assertions"],
            "topology": contract["topology"],
            "allow_skipped": False,
            "result": "passed",
            "run_url": run_url,
            "matrix": sorted(
                matrix,
                key=lambda item: (
                    item["platform"] or "",
                    item["role"] or "",
                ),
            ),
        }
        grouped.setdefault(criterion_id, []).append(proof)

    approval = plan["approval"]
    bundle: dict[str, Any] = {
        "schema_version": 1,
        "plan_id": plan["id"],
        "plan_content_sha256": approval["content_sha256"],
        "source_sha": source_sha,
        "workflow_sha256": sha256_bytes(workflow_bytes),
        "run_id": run_id,
        "run_attempt": run_attempt,
        "run_url": run_url,
        "criteria": [
            {
                "criterion_id": criterion_id,
                "evidence": sorted(
                    grouped[criterion_id],
                    key=lambda item: item["evidence_id"],
                ),
            }
            for criterion_id in sorted(grouped)
        ],
        "bundle_sha256": "",
    }
    bundle["bundle_sha256"] = canonical_digest(
        {key: value for key, value in bundle.items() if key != "bundle_sha256"}
    )
    errors = validate_bundle(
        task,
        record,
        plan,
        bundle,
        gate_registry,
        source_sha,
    )
    if errors:
        raise EvidenceError(errors[0])
    return bundle


def _append_exact_errors(
    errors: list[str],
    value: Any,
    fields: set[str],
    label: str,
) -> dict[str, Any]:
    if not isinstance(value, dict):
        errors.append(f"{label} must be an object")
        return {}
    missing = sorted(fields - set(value))
    unknown = sorted(set(value) - fields)
    if missing:
        errors.append(f"{label} is missing fields: {', '.join(missing)}")
    if unknown:
        errors.append(f"{label} has unknown fields: {', '.join(unknown)}")
    return value


def validate_bundle(
    task: dict[str, Any],
    record: dict[str, Any],
    plan: dict[str, Any],
    raw_bundle: Any,
    gate_registry: dict[str, str],
    expected_source_sha: str,
) -> list[str]:
    errors: list[str] = []
    bundle = _append_exact_errors(
        errors,
        raw_bundle,
        BUNDLE_FIELDS,
        "criterion_evidence",
    )
    if not bundle:
        return errors
    if bundle.get("schema_version") != 1:
        errors.append("criterion_evidence.schema_version must be 1")
    if bundle.get("plan_id") != plan.get("id"):
        errors.append("criterion_evidence.plan_id does not match task plan")
    approval = plan.get("approval")
    approved_digest = (
        approval.get("content_sha256")
        if isinstance(approval, dict)
        else None
    )
    if bundle.get("plan_content_sha256") != approved_digest:
        errors.append("criterion_evidence does not bind the approved plan")
    if bundle.get("source_sha") != expected_source_sha:
        errors.append("criterion_evidence.source_sha is stale")
    for field in ("workflow_sha256", "bundle_sha256"):
        value = bundle.get(field)
        if not isinstance(value, str) or not SHA256_PATTERN.fullmatch(value):
            errors.append(f"criterion_evidence.{field} must be SHA-256")
    run_id = bundle.get("run_id")
    run_attempt = bundle.get("run_attempt")
    run_url = bundle.get("run_url")
    run_match = (
        RUN_URL_PATTERN.fullmatch(run_url)
        if isinstance(run_url, str)
        else None
    )
    if (
        not isinstance(run_id, int)
        or run_id <= 0
        or not isinstance(run_attempt, int)
        or run_attempt <= 0
        or run_match is None
        or int(run_match.group(3)) != run_id
    ):
        errors.append("criterion_evidence run URL and id are invalid")
    try:
        contracts = _load_contracts(task, record, plan, gate_registry)
    except EvidenceError as error:
        errors.append(str(error))
        contracts = []
    expected = {
        (criterion_id, str(contract.get("id"))): contract
        for criterion_id, contract in contracts
    }
    criteria = bundle.get("criteria")
    if not isinstance(criteria, list):
        errors.append("criterion_evidence.criteria must be an array")
        criteria = []
    seen_criteria: set[str] = set()
    seen_evidence: set[tuple[str, str]] = set()
    artifact_ids: set[int] = set()
    for criterion_index, raw_criterion in enumerate(criteria):
        criterion = _append_exact_errors(
            errors,
            raw_criterion,
            CRITERION_FIELDS,
            f"criterion_evidence.criteria[{criterion_index}]",
        )
        criterion_id = criterion.get("criterion_id")
        if not isinstance(criterion_id, str):
            errors.append("criterion evidence criterion_id is invalid")
            continue
        if criterion_id in seen_criteria:
            errors.append(f"duplicate criterion {criterion_id}")
        seen_criteria.add(criterion_id)
        proofs = criterion.get("evidence")
        if not isinstance(proofs, list) or not proofs:
            errors.append(f"{criterion_id} evidence must be a nonempty array")
            continue
        for proof_index, raw_proof in enumerate(proofs):
            proof = _append_exact_errors(
                errors,
                raw_proof,
                EVIDENCE_FIELDS,
                f"{criterion_id}.evidence[{proof_index}]",
            )
            evidence_id = proof.get("evidence_id")
            key = (criterion_id, str(evidence_id))
            if key in seen_evidence:
                errors.append(f"duplicate evidence {evidence_id}")
            seen_evidence.add(key)
            contract = expected.get(key)
            if contract is None:
                errors.append(f"{criterion_id} contains unapproved evidence")
                continue
            command = gate_registry.get(str(contract.get("gate")), "")
            expected_values = {
                "producer_task": contract.get("producer_task"),
                "gate": contract.get("gate"),
                "gate_command_sha256": sha256_bytes(command.encode("utf-8")),
                "level": contract.get("level"),
                "scenarios": contract.get("required_scenarios"),
                "assertions": contract.get("required_assertions"),
                "topology": contract.get("topology"),
                "allow_skipped": False,
                "result": "passed",
                "run_url": run_url,
            }
            for field, expected_value in expected_values.items():
                if proof.get(field) != expected_value:
                    errors.append(
                        f"{criterion_id}.{evidence_id}.{field} "
                        "does not match the approved contract"
                    )
            matrix = proof.get("matrix")
            if not isinstance(matrix, list):
                errors.append(f"{criterion_id}.{evidence_id}.matrix must be an array")
                continue
            matrix_values: list[tuple[str | None, str | None]] = []
            for entry_index, raw_entry in enumerate(matrix):
                entry = _append_exact_errors(
                    errors,
                    raw_entry,
                    MATRIX_FIELDS,
                    f"{criterion_id}.{evidence_id}.matrix[{entry_index}]",
                )
                platform = entry.get("platform")
                role = entry.get("role")
                try:
                    _safe_optional(platform, "criterion evidence platform")
                    _safe_optional(role, "criterion evidence role")
                except EvidenceError as error:
                    errors.append(str(error))
                matrix_values.append((platform, role))
                if (
                    entry.get("job_conclusion") != "success"
                    or entry.get("result") != "passed"
                    or entry.get("skipped") is not False
                ):
                    errors.append(
                        f"{criterion_id}.{evidence_id} contains non-passing matrix evidence"
                    )
                job_id = entry.get("job_id")
                job_run_attempt = entry.get("job_run_attempt")
                artifact_id = entry.get("artifact_id")
                artifact_digest = entry.get("artifact_sha256")
                if not isinstance(job_id, int) or job_id <= 0:
                    errors.append("criterion evidence job_id is invalid")
                if job_run_attempt != run_attempt:
                    errors.append(
                        "criterion evidence job run attempt is stale"
                    )
                job_name = entry.get("job_name")
                if (
                    not isinstance(job_name, str)
                    or not 1 <= len(job_name) <= 200
                    or "\n" in job_name
                    or "\r" in job_name
                ):
                    errors.append("criterion evidence job_name is invalid")
                job_url = entry.get("job_url")
                job_match = (
                    JOB_URL_PATTERN.fullmatch(job_url)
                    if isinstance(job_url, str)
                    else None
                )
                if (
                    job_match is None
                    or not isinstance(job_id, int)
                    or int(job_match.group(3)) != run_id
                    or int(job_match.group(4)) != job_id
                    or (
                        run_match is not None
                        and job_match.groups()[:2] != run_match.groups()[:2]
                    )
                ):
                    errors.append("criterion evidence job URL is invalid")
                if not isinstance(artifact_id, int) or artifact_id <= 0:
                    errors.append("criterion evidence artifact_id is invalid")
                elif artifact_id in artifact_ids:
                    errors.append("criterion evidence artifact ids are duplicated")
                else:
                    artifact_ids.add(artifact_id)
                if not isinstance(artifact_digest, str) or not SHA256_PATTERN.fullmatch(
                    artifact_digest
                ):
                    errors.append("criterion evidence artifact digest is invalid")
                artifact_name = entry.get("artifact_name")
                artifact_size = entry.get("artifact_size")
                if (
                    not isinstance(artifact_name, str)
                    or not artifact_name.startswith("criterion-")
                    or len(artifact_name) > 200
                    or "\n" in artifact_name
                    or "\r" in artifact_name
                ):
                    errors.append("criterion evidence artifact name is invalid")
                if (
                    not isinstance(artifact_size, int)
                    or not 0 < artifact_size <= MAX_ARTIFACT_BYTES
                ):
                    errors.append("criterion evidence artifact size is invalid")
                binaries = entry.get("binary_digests")
                if not isinstance(binaries, list):
                    errors.append("criterion evidence binary_digests must be an array")
                    binaries = []
                binary_paths: set[str] = set()
                for raw_binary in binaries:
                    binary = _append_exact_errors(
                        errors,
                        raw_binary,
                        BINARY_FIELDS,
                        "criterion evidence binary",
                    )
                    path = binary.get("path")
                    digest = binary.get("sha256")
                    try:
                        normalized_path = _safe_archive_path(
                            path,
                            "criterion evidence binary path",
                        )
                    except EvidenceError as error:
                        errors.append(str(error))
                        normalized_path = ""
                    if not normalized_path or normalized_path in binary_paths:
                        errors.append("criterion evidence binary paths are invalid")
                    else:
                        binary_paths.add(normalized_path)
                    if not isinstance(digest, str) or not SHA256_PATTERN.fullmatch(
                        digest
                    ):
                        errors.append("criterion evidence binary digest is invalid")
                try:
                    _validate_runtime(
                        entry.get("runtime"),
                        topology=str(contract.get("topology")),
                        level=str(contract.get("level")),
                        has_binaries=bool(binaries),
                    )
                except EvidenceError as error:
                    errors.append(str(error))
            platforms = contract.get("required_platforms") or [None]
            roles = contract.get("required_roles") or [None]
            required_matrix = set(itertools.product(platforms, roles))
            if (
                set(matrix_values) != required_matrix
                or len(matrix_values) != len(required_matrix)
            ):
                errors.append(
                    f"{criterion_id}.{evidence_id} does not cover exact matrix"
                )
    if seen_evidence != set(expected):
        errors.append("criterion evidence does not cover every approved contract")
    if isinstance(bundle.get("bundle_sha256"), str):
        expected_digest = canonical_digest(
            {
                key: value
                for key, value in bundle.items()
                if key != "bundle_sha256"
            }
        )
        if bundle.get("bundle_sha256") != expected_digest:
            errors.append("criterion_evidence.bundle_sha256 does not match content")
    return errors


def _load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise EvidenceError(f"cannot load evidence input {path.name}") from error
    if not isinstance(value, dict):
        raise EvidenceError(f"evidence input {path.name} must be an object")
    return value


def _load_task(root: Path, task_id: str) -> dict[str, Any]:
    backlog = _load_json(root / ".agents" / "backlog.yaml")
    tasks = backlog.get("tasks")
    if not isinstance(tasks, list):
        raise EvidenceError("backlog has no tasks array")
    matches = [
        task
        for task in tasks
        if isinstance(task, dict) and task.get("id") == task_id
    ]
    if len(matches) != 1:
        raise EvidenceError(f"backlog does not contain one {task_id} task")
    return matches[0]


def _git_bytes(root: Path, *args: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(root), *args],
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        raise EvidenceError("cannot read evidence input from Git")
    return result.stdout


def collect_for_task(
    root: Path,
    task_id: str,
    source_sha: str,
    run: dict[str, Any],
    jobs_payload: dict[str, Any],
    artifacts_payload: dict[str, Any],
    download_artifact: Callable[[int], bytes],
) -> dict[str, Any]:
    task = _load_task(root, task_id)
    record = _load_json(root / ".agents" / "records" / f"{task_id}.json")
    plan_id = task.get("delivery_plan")
    if not isinstance(plan_id, str):
        raise EvidenceError(f"{task_id} has no Delivery Plan")
    plan = _load_json(root / ".agents" / "plans" / f"{plan_id}.json")
    try:
        gates = load_gate_registry(root / ".agents" / "manifest.yaml")
    except GateRegistryError as error:
        raise EvidenceError("cannot load trusted gate registry") from error
    workflow = _git_bytes(
        root,
        "show",
        f"{source_sha}:.github/workflows/ci.yml",
    )
    return collect_bundle(
        task=task,
        record=record,
        plan=plan,
        source_sha=source_sha,
        workflow_bytes=workflow,
        gate_registry=gates,
        run=run,
        jobs_payload=jobs_payload,
        artifacts_payload=artifacts_payload,
        download_artifact=download_artifact,
    )


def validate_record_evidence(
    root: Path,
    task: dict[str, Any],
    record: dict[str, Any],
    gate_registry: dict[str, str],
) -> list[str]:
    state = record.get("state")
    bundle = record.get("criterion_evidence")
    if record.get("schema_version") != 4:
        if bundle is not None:
            return [
                f"{task.get('id')}.criterion_evidence is valid only for schema 4"
            ]
        return []
    if state != "done":
        if bundle is not None:
            return [
                f"{task.get('id')}.criterion_evidence is generated only at acceptance"
            ]
        return []
    plan_id = task.get("delivery_plan")
    if not isinstance(plan_id, str):
        return [f"{task.get('id')}.criterion_evidence has no Delivery Plan"]
    try:
        plan = _load_json(root / ".agents" / "plans" / f"{plan_id}.json")
    except EvidenceError as error:
        return [str(error)]
    integration = record.get("integration")
    source_sha = (
        integration.get("verified_sha")
        if isinstance(integration, dict)
        else ""
    )
    return validate_bundle(
        task,
        record,
        plan,
        bundle,
        gate_registry,
        str(source_sha),
    )


def write_bundle(path: Path, bundle: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(bundle, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )


def _find_contract(
    contracts: list[tuple[str, dict[str, Any]]],
    evidence_id: str,
) -> tuple[str, dict[str, Any]]:
    matches = [
        (criterion_id, contract)
        for criterion_id, contract in contracts
        if contract.get("id") == evidence_id
    ]
    if len(matches) != 1:
        raise EvidenceError(f"{evidence_id} is not one approved task contract")
    return matches[0]


def ci_matrix(
    task: dict[str, Any],
    record: dict[str, Any],
    plan: dict[str, Any],
    gate_registry: dict[str, str],
) -> dict[str, list[dict[str, str]]]:
    if (
        record.get("schema_version") != 4
        or record.get("state") != "integrated"
    ):
        return {"include": []}
    contracts = _load_contracts(task, record, plan, gate_registry)
    entries: list[dict[str, str]] = []
    for _criterion_id, contract in contracts:
        platforms = contract.get("required_platforms") or [None]
        roles = contract.get("required_roles") or [None]
        for platform, role in itertools.product(platforms, roles):
            if platform not in RUNNER_BY_PLATFORM:
                raise EvidenceError(
                    f"evidence platform {platform!r} has no trusted CI runner"
                )
            platform_label = platform or "none"
            role_label = role or "none"
            evidence_id = str(contract["id"])
            entries.append(
                {
                    "runner": RUNNER_BY_PLATFORM[platform],
                    "platform": platform or "",
                    "platform_label": platform_label,
                    "role": role or "",
                    "role_label": role_label,
                    "evidence_id": evidence_id,
                    "gate": str(contract["gate"]),
                    "artifact_name": (
                        f"criterion-{evidence_id}-"
                        f"{platform_label}-{role_label}"
                    ),
                    "job_name": (
                        f"Criterion evidence ({evidence_id}, "
                        f"{platform_label}, {role_label})"
                    ),
                }
            )
    if len(entries) > 64:
        raise EvidenceError("task criterion evidence matrix exceeds 64 jobs")
    return {
        "include": sorted(
            entries,
            key=lambda item: (
                item["evidence_id"],
                item["platform"],
                item["role"],
            ),
        )
    }


def matrix_command(args: argparse.Namespace) -> None:
    root = args.root.resolve()
    task = _load_task(root, args.task_id)
    record = _load_json(
        root / ".agents" / "records" / f"{args.task_id}.json"
    )
    if record.get("schema_version") != 4:
        print(json.dumps({"include": []}, separators=(",", ":")))
        return
    plan_id = task.get("delivery_plan")
    if not isinstance(plan_id, str):
        raise EvidenceError(f"{args.task_id} has no Delivery Plan")
    plan = _load_json(root / ".agents" / "plans" / f"{plan_id}.json")
    try:
        gates = load_gate_registry(root / ".agents" / "manifest.yaml")
    except GateRegistryError as error:
        raise EvidenceError("cannot load trusted gate registry") from error
    print(
        json.dumps(
            ci_matrix(task, record, plan, gates),
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        )
    )


def run_gate_and_emit(args: argparse.Namespace) -> None:
    root = args.root.resolve()
    task = _load_task(root, args.task_id)
    record = _load_json(
        root / ".agents" / "records" / f"{args.task_id}.json"
    )
    plan = _load_json(
        root / ".agents" / "plans" / f"{task['delivery_plan']}.json"
    )
    try:
        gates = load_gate_registry(root / ".agents" / "manifest.yaml")
    except GateRegistryError as error:
        raise EvidenceError("cannot load trusted gate registry") from error
    contracts = _load_contracts(task, record, plan, gates)
    criterion_id, contract = _find_contract(contracts, args.evidence_id)
    platform = args.platform or None
    role = args.role or None
    required_platforms = contract.get("required_platforms") or [None]
    required_roles = contract.get("required_roles") or [None]
    if platform not in required_platforms or role not in required_roles:
        raise EvidenceError("emitted evidence is outside the approved matrix")
    source_sha = _git_bytes(root, "rev-parse", "HEAD").decode("ascii").strip()
    environment_sha = os.environ.get("GITHUB_SHA")
    if environment_sha and environment_sha != source_sha:
        raise EvidenceError("checkout SHA does not match GITHUB_SHA")
    raw_attempt = os.environ.get("GITHUB_RUN_ATTEMPT", "1")
    try:
        run_attempt = int(raw_attempt)
    except ValueError as error:
        raise EvidenceError("GITHUB_RUN_ATTEMPT is invalid") from error
    if run_attempt <= 0:
        raise EvidenceError("GITHUB_RUN_ATTEMPT is invalid")
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        raise EvidenceError("evidence output directory must be empty")
    output.mkdir(parents=True, exist_ok=True)
    witness_path = output / ".witness.json"
    environment = os.environ.copy()
    environment.update(
        {
            "XNN_EVIDENCE_ID": args.evidence_id,
            "XNN_EVIDENCE_PLATFORM": platform or "",
            "XNN_EVIDENCE_ROLE": role or "",
            "XNN_EVIDENCE_SOURCE_SHA": source_sha,
            "XNN_EVIDENCE_WITNESS": str(witness_path),
        }
    )
    command = gates[str(contract["gate"])]
    result = subprocess.run(
        ["bash", "-lc", command],
        cwd=root,
        env=environment,
        check=False,
    )
    if result.returncode != 0:
        raise EvidenceError("trusted evidence gate failed")
    runtime = {
        "process_count": args.process_count,
        "transport": args.transport,
        "authenticated": args.authenticated,
        "packaged": args.packaged,
    }
    binary_inputs = list(args.binary)
    if witness_path.is_file():
        if witness_path.stat().st_size > MAX_MANIFEST_BYTES:
            raise EvidenceError("evidence witness is too large")
        witness = _load_json(witness_path)
        _require_exact(witness, WITNESS_FIELDS, "evidence witness")
        runtime = _require_exact(
            witness.get("runtime"),
            RUNTIME_FIELDS,
            "evidence witness runtime",
        )
        raw_binaries = witness.get("binaries")
        if not isinstance(raw_binaries, list) or not all(
            isinstance(path, str) and path for path in raw_binaries
        ):
            raise EvidenceError(
                "evidence witness binaries must be an array of paths"
            )
        if len(raw_binaries) != len(set(raw_binaries)):
            raise EvidenceError("evidence witness binaries contains duplicates")
        binary_inputs.extend(raw_binaries)
        witness_path.unlink()
    elif contract.get("topology") in {
        "real_process",
        "two_process",
        "packaged_e2e",
    } or contract.get("level") == "e2e":
        raise EvidenceError(
            "process and E2E evidence gates must emit a runtime witness"
        )
    if any(output.iterdir()):
        raise EvidenceError("evidence gate wrote undeclared output files")
    if len(binary_inputs) > 64:
        raise EvidenceError("evidence binary list exceeds 64 entries")
    binaries: list[dict[str, str]] = []
    resolved_sources: set[Path] = set()
    for index, raw_path in enumerate(binary_inputs):
        candidate = Path(raw_path)
        if not candidate.is_absolute():
            candidate = root / candidate
        if candidate.is_symlink():
            raise EvidenceError("evidence binary must not be a symbolic link")
        source = candidate.resolve()
        try:
            source.relative_to(root)
        except ValueError as error:
            raise EvidenceError("binary path escapes the repository") from error
        if source in resolved_sources:
            raise EvidenceError("evidence binary list contains duplicates")
        resolved_sources.add(source)
        if not source.is_file() or source.is_symlink():
            raise EvidenceError("evidence binary must be a regular file")
        target_name = f"bin/{index:02d}-{source.name}"
        target = output / target_name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        binaries.append(
            {
                "path": target_name,
                "sha256": sha256_file(target),
            }
        )
    _validate_runtime(
        runtime,
        topology=str(contract["topology"]),
        level=str(contract["level"]),
        has_binaries=bool(binaries),
    )
    manifest = {
        "schema_version": 1,
        "criterion_id": criterion_id,
        "evidence_id": args.evidence_id,
        "source_sha": source_sha,
        "run_attempt": run_attempt,
        "gate": contract["gate"],
        "scenarios": contract["required_scenarios"],
        "assertions": contract["required_assertions"],
        "topology": contract["topology"],
        "platform": platform,
        "role": role,
        "job_name": args.job_name,
        "result": "passed",
        "skipped": False,
        "runtime": runtime,
        "binaries": binaries,
    }
    (output / "evidence.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    matrix_parser = subparsers.add_parser("matrix")
    matrix_parser.add_argument("--root", type=Path, required=True)
    matrix_parser.add_argument("--task-id", required=True)
    run_parser = subparsers.add_parser("run")
    run_parser.add_argument("--root", type=Path, required=True)
    run_parser.add_argument("--task-id", required=True)
    run_parser.add_argument("--evidence-id", required=True)
    run_parser.add_argument("--platform", default="")
    run_parser.add_argument("--role", default="")
    run_parser.add_argument("--job-name", required=True)
    run_parser.add_argument("--output", type=Path, required=True)
    run_parser.add_argument("--binary", action="append", default=[])
    run_parser.add_argument("--process-count", type=int, default=1)
    run_parser.add_argument("--transport", default="none")
    run_parser.add_argument("--authenticated", action="store_true")
    run_parser.add_argument("--packaged", action="store_true")
    args = parser.parse_args()
    try:
        if args.command == "matrix":
            matrix_command(args)
        elif args.command == "run":
            run_gate_and_emit(args)
    except (EvidenceError, OSError) as error:
        print(f"Criterion evidence error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
