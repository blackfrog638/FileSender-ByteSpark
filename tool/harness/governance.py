#!/usr/bin/env python3

"""Deterministic task-governance validation and record updates."""

from __future__ import annotations

import argparse
import copy
import datetime as dt
import fnmatch
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

sys.dont_write_bytecode = True
HARNESS_DIR = Path(__file__).resolve().parent
if str(HARNESS_DIR) not in sys.path:
    sys.path.insert(0, str(HARNESS_DIR))
import architecture_change
import defect_proof as defect_proof_runner
from trusted_gates import GateRegistryError, load_gate_registry


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
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
RISK_SCHEMA_MIN_TASK_NUMBER = 41
RISK_DIMENSIONS = (
    "functionality",
    "security",
    "performance",
    "compatibility",
    "concurrency",
    "platform",
    "persistence",
)
RISK_LEVELS = {"none", "low", "medium", "high", "critical"}
SCHEMA_V3_TASK_TYPES = {
    "feature",
    "bugfix",
    "refactor",
    "investigation",
    "test",
    "governance",
}
DEFECT_FIELDS = {
    "severity",
    "source",
    "symptom",
    "expected_contract",
    "actual_behavior",
    "trigger",
    "affected_since",
    "proof_mode",
    "reproduction_commit",
    "regression_gate",
    "contract_disposition",
}
DEFECT_SEVERITIES = {"P0", "P1", "P2", "P3"}
DEFECT_SOURCES = {
    "audit",
    "ci",
    "user_report",
    "production",
    "test",
    "investigation",
}
DEFECT_PROOF_MODES = {
    "deterministic",
    "sanitizer",
    "stress",
    "platform_ci",
    "manual",
}
CONTRACT_DISPOSITIONS = {"restore", "preserve", "change"}
DEFECT_PROOF_FIELDS = {
    "mode",
    "gate",
    "command_sha256",
    "reproduction_commit",
    "head_commit",
    "reproduction_exit_code",
    "head_exit_code",
    "checked_at",
}
INVESTIGATION_FIELDS = {
    "question",
    "scope",
    "evidence_required",
    "exit_criteria",
    "outcome_disposition",
}
INVESTIGATION_DISPOSITIONS = {
    "pending",
    "bugfix",
    "feature",
    "no_change",
}
COMMIT_TYPES = {
    "feat",
    "fix",
    "docs",
    "style",
    "refactor",
    "perf",
    "test",
    "build",
    "ci",
    "chore",
    "revert",
}
COMMIT_SCOPE_PATTERN = re.compile(r"^[a-z0-9][a-z0-9.-]*$")
COMMIT_SUMMARY_PATTERN = re.compile(r"^[a-z][^\t\r\n]{11,}$")
TASK_ID_IN_TEXT_PATTERN = re.compile(r"\bXT-[0-9]{3,}\b", re.IGNORECASE)
ARCHITECTURE_SCHEMA_MIN_TASK_NUMBER = 48
ARCHITECTURE_MODULES = ROOT / ".agents" / "architecture" / "modules.json"
MANIFEST = ROOT / ".agents" / "manifest.yaml"


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


def trusted_gate_registry() -> dict[str, str]:
    try:
        return load_gate_registry(MANIFEST)
    except GateRegistryError as error:
        raise GovernanceError(str(error)) from error


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


def stable_range_patch_id(base: str, head: str, excluded_path: str) -> str:
    shown = git(
        "diff",
        "--binary",
        base,
        head,
        "--",
        ".",
        f":(exclude){excluded_path}",
    )
    result = git("patch-id", "--stable", input_bytes=shown.stdout)
    output = result.stdout.decode("ascii").strip()
    if not output:
        raise GovernanceError(
            f"Range {base}..{head} has no payload patch ID"
        )
    return output.split()[0]


def commit_parents(commit: str) -> list[str]:
    values = git_text("rev-list", "--parents", "-n", "1", commit).split()
    return values[1:]


def source_commits_digest(commits: list[str]) -> str:
    encoded = ("\n".join(commits) + "\n").encode("ascii")
    return hashlib.sha256(encoded).hexdigest()


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


def require_concrete_string(
    errors: list[str],
    value: Any,
    label: str,
) -> str:
    result = require_string(errors, value, label)
    if result and "TODO" in result.upper():
        errors.append(f"{label} still contains TODO")
    return result


def validate_exact_object(
    errors: list[str],
    value: Any,
    label: str,
    fields: set[str],
) -> dict[str, Any]:
    if not isinstance(value, dict):
        errors.append(f"{label} must be an object")
        return {}
    missing = sorted(fields - set(value))
    extra = sorted(set(value) - fields)
    if missing:
        errors.append(f"{label} is missing fields: {', '.join(missing)}")
    if extra:
        errors.append(f"{label} has unknown fields: {', '.join(extra)}")
    return value


def validate_schema_v3(
    errors: list[str],
    task_id: str,
    record: dict[str, Any],
    state: str,
    gate_registry: dict[str, str],
    verification_gates: list[str],
    *,
    verify_git: bool,
) -> None:
    task_type = require_string(
        errors,
        record.get("task_type"),
        f"{task_id}.task_type",
    )
    if task_type not in SCHEMA_V3_TASK_TYPES:
        errors.append(
            f"{task_id}.task_type must be one of "
            f"{sorted(SCHEMA_V3_TASK_TYPES)}"
        )

    if task_type == "bugfix":
        if "investigation" in record:
            errors.append(
                f"{task_id}.investigation is invalid for task_type bugfix"
            )
        defect = validate_exact_object(
            errors,
            record.get("defect"),
            f"{task_id}.defect",
            DEFECT_FIELDS,
        )
        severity = require_concrete_string(
            errors,
            defect.get("severity"),
            f"{task_id}.defect.severity",
        )
        if severity not in DEFECT_SEVERITIES:
            errors.append(
                f"{task_id}.defect.severity must be one of "
                f"{sorted(DEFECT_SEVERITIES)}"
            )
        source = require_concrete_string(
            errors,
            defect.get("source"),
            f"{task_id}.defect.source",
        )
        if source not in DEFECT_SOURCES:
            errors.append(
                f"{task_id}.defect.source must be one of "
                f"{sorted(DEFECT_SOURCES)}"
            )
        for field in (
            "symptom",
            "expected_contract",
            "actual_behavior",
            "trigger",
            "affected_since",
        ):
            require_concrete_string(
                errors,
                defect.get(field),
                f"{task_id}.defect.{field}",
            )
        proof_mode = require_concrete_string(
            errors,
            defect.get("proof_mode"),
            f"{task_id}.defect.proof_mode",
        )
        if proof_mode not in DEFECT_PROOF_MODES:
            errors.append(
                f"{task_id}.defect.proof_mode must be one of "
                f"{sorted(DEFECT_PROOF_MODES)}"
            )
        reproduction_commit = require_string(
            errors,
            defect.get("reproduction_commit"),
            f"{task_id}.defect.reproduction_commit",
            allow_empty=state in {"ready", "claimed", "in_progress", "blocked"},
        )
        if (
            reproduction_commit
            and not SHA_PATTERN.fullmatch(reproduction_commit)
        ):
            errors.append(
                f"{task_id}.defect.reproduction_commit must be a full "
                "lowercase SHA"
            )
        elif (
            reproduction_commit
            and verify_git
            and state == "review"
            and not commit_exists(reproduction_commit)
        ):
            errors.append(
                f"{task_id}.defect.reproduction_commit is unavailable: "
                f"{reproduction_commit}"
            )
        regression_gate = require_concrete_string(
            errors,
            defect.get("regression_gate"),
            f"{task_id}.defect.regression_gate",
        )
        if regression_gate not in gate_registry:
            errors.append(
                f"{task_id}.defect.regression_gate is not registered: "
                f"{regression_gate}"
            )
        elif regression_gate not in verification_gates:
            errors.append(
                f"{task_id}.defect.regression_gate must appear in "
                "verification.gates"
            )
        disposition = require_concrete_string(
            errors,
            defect.get("contract_disposition"),
            f"{task_id}.defect.contract_disposition",
        )
        if disposition not in CONTRACT_DISPOSITIONS:
            errors.append(
                f"{task_id}.defect.contract_disposition must be one of "
                f"{sorted(CONTRACT_DISPOSITIONS)}"
            )
        if disposition == "change":
            impacts = record.get("impacts")
            adr = impacts.get("adr") if isinstance(impacts, dict) else None
            if not isinstance(adr, dict) or adr.get("required") is not True:
                errors.append(
                    f"{task_id} contract-changing bugfix requires an ADR"
                )
            elif adr.get("status") not in {"proposed", "accepted"}:
                errors.append(
                    f"{task_id} contract-changing bugfix ADR must be "
                    "proposed or accepted"
                )
        verification = record.get("verification")
        proof = (
            verification.get("defect_proof")
            if isinstance(verification, dict)
            else None
        )
        proof_required = state in {"review", "integrated", "done"}
        if proof_required and proof_mode != "deterministic":
            errors.append(
                f"{task_id}.defect.proof_mode {proof_mode!r} has no "
                "review executor"
            )
        if proof_required or proof is not None:
            proof = validate_exact_object(
                errors,
                proof,
                f"{task_id}.verification.defect_proof",
                DEFECT_PROOF_FIELDS,
            )
            proof_mode_value = require_string(
                errors,
                proof.get("mode"),
                f"{task_id}.verification.defect_proof.mode",
            )
            if proof_mode_value != proof_mode:
                errors.append(
                    f"{task_id}.verification.defect_proof.mode does not "
                    "match defect.proof_mode"
                )
            proof_gate = require_string(
                errors,
                proof.get("gate"),
                f"{task_id}.verification.defect_proof.gate",
            )
            if proof_gate != regression_gate:
                errors.append(
                    f"{task_id}.verification.defect_proof.gate does not "
                    "match defect.regression_gate"
                )
            command_sha = require_string(
                errors,
                proof.get("command_sha256"),
                f"{task_id}.verification.defect_proof.command_sha256",
            )
            if command_sha and not SHA256_PATTERN.fullmatch(command_sha):
                errors.append(
                    f"{task_id}.verification.defect_proof.command_sha256 "
                    "must be a SHA-256 digest"
                )
            command = gate_registry.get(regression_gate)
            if (
                command
                and command_sha
                and hashlib.sha256(command.encode("utf-8")).hexdigest()
                != command_sha
            ):
                errors.append(
                    f"{task_id}.verification.defect_proof.command_sha256 "
                    "does not match the trusted gate"
                )
            proof_reproduction = require_string(
                errors,
                proof.get("reproduction_commit"),
                f"{task_id}.verification.defect_proof.reproduction_commit",
            )
            if proof_reproduction != reproduction_commit:
                errors.append(
                    f"{task_id}.verification.defect_proof.reproduction_commit "
                    "does not match defect.reproduction_commit"
                )
            proof_head = require_string(
                errors,
                proof.get("head_commit"),
                f"{task_id}.verification.defect_proof.head_commit",
            )
            record_head = record.get("head_sha")
            if proof_required and proof_head != record_head:
                errors.append(
                    f"{task_id}.verification.defect_proof.head_commit does "
                    "not match head_sha"
                )
            for name, commit in (
                ("reproduction_commit", proof_reproduction),
                ("head_commit", proof_head),
            ):
                if commit and not SHA_PATTERN.fullmatch(commit):
                    errors.append(
                        f"{task_id}.verification.defect_proof.{name} must "
                        "be a full lowercase SHA"
                    )
            reproduction_exit = proof.get("reproduction_exit_code")
            if type(reproduction_exit) is not int or reproduction_exit in {
                0,
                126,
                127,
            }:
                errors.append(
                    f"{task_id}.verification.defect_proof."
                    "reproduction_exit_code must be a non-infrastructure "
                    "failure"
                )
            head_exit = proof.get("head_exit_code")
            if type(head_exit) is not int or head_exit != 0:
                errors.append(
                    f"{task_id}.verification.defect_proof.head_exit_code "
                    "must be zero"
                )
            checked_at = require_string(
                errors,
                proof.get("checked_at"),
                f"{task_id}.verification.defect_proof.checked_at",
            )
            if checked_at:
                try:
                    checked_time = dt.datetime.fromisoformat(checked_at)
                except ValueError:
                    errors.append(
                        f"{task_id}.verification.defect_proof.checked_at "
                        "is not ISO-8601"
                    )
                else:
                    if checked_time.tzinfo is None:
                        errors.append(
                            f"{task_id}.verification.defect_proof.checked_at "
                            "must include a timezone"
                        )
            base = record.get("base_sha")
            if (
                verify_git
                and proof_required
                and isinstance(base, str)
                and SHA_PATTERN.fullmatch(base)
                and SHA_PATTERN.fullmatch(proof_reproduction)
                and SHA_PATTERN.fullmatch(proof_head)
                and commit_exists(base)
                and commit_exists(proof_reproduction)
                and commit_exists(proof_head)
            ):
                if not is_ancestor(base, proof_reproduction):
                    errors.append(
                        f"{task_id}.verification.defect_proof reproduction "
                        "is outside the task base"
                    )
                if not is_ancestor(proof_reproduction, proof_head):
                    errors.append(
                        f"{task_id}.verification.defect_proof reproduction "
                        "is not an ancestor of head"
                    )
    elif task_type == "investigation":
        if "defect" in record:
            errors.append(
                f"{task_id}.defect is invalid for task_type investigation"
            )
        investigation = validate_exact_object(
            errors,
            record.get("investigation"),
            f"{task_id}.investigation",
            INVESTIGATION_FIELDS,
        )
        for field in (
            "question",
            "scope",
            "evidence_required",
            "exit_criteria",
        ):
            require_concrete_string(
                errors,
                investigation.get(field),
                f"{task_id}.investigation.{field}",
            )
        disposition = require_concrete_string(
            errors,
            investigation.get("outcome_disposition"),
            f"{task_id}.investigation.outcome_disposition",
        )
        if disposition not in INVESTIGATION_DISPOSITIONS:
            errors.append(
                f"{task_id}.investigation.outcome_disposition must be one of "
                f"{sorted(INVESTIGATION_DISPOSITIONS)}"
            )
        if state in {"review", "integrated", "done"} and disposition == "pending":
            errors.append(
                f"{task_id}.investigation.outcome_disposition cannot remain "
                "pending at review"
            )
    else:
        if "defect" in record:
            errors.append(
                f"{task_id}.defect is only valid for task_type bugfix"
            )
        if "investigation" in record:
            errors.append(
                f"{task_id}.investigation is only valid for task_type "
                "investigation"
            )


def validate_verified_sha(
    errors: list[str],
    task_id: str,
    state: str,
    integration: dict[str, Any],
    *,
    verify_git: bool,
) -> str:
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
    return verified_sha


def validate_cherry_pick_integration(
    errors: list[str],
    task_id: str,
    head_sha: str,
    integration: dict[str, Any],
    verified_sha: str,
    *,
    verify_git: bool,
) -> None:
    mappings = integration.get("mappings")
    if not isinstance(mappings, list) or not mappings:
        errors.append(f"{task_id}.integration.mappings must not be empty")
        mappings = []

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


def validate_squash_integration(
    errors: list[str],
    task_id: str,
    state: str,
    base_sha: str,
    head_sha: str,
    integration: dict[str, Any],
    verified_sha: str,
    *,
    verify_git: bool,
) -> None:
    label = f"{task_id}.integration"
    source_base = require_string(
        errors, integration.get("source_base"), f"{label}.source_base"
    )
    source_head = require_string(
        errors, integration.get("source_head"), f"{label}.source_head"
    )
    source_patch_id = require_string(
        errors,
        integration.get("source_patch_id"),
        f"{label}.source_patch_id",
    )
    commits_digest = require_string(
        errors,
        integration.get("source_commits_sha256"),
        f"{label}.source_commits_sha256",
    )
    result = require_string(
        errors,
        integration.get("result"),
        f"{label}.result",
        allow_empty=state == "integrated",
    )
    result_patch_id = require_string(
        errors,
        integration.get("result_patch_id"),
        f"{label}.result_patch_id",
    )
    source_commits = integration.get("source_commits")
    if not isinstance(source_commits, list) or not source_commits:
        errors.append(f"{label}.source_commits must not be empty")
        source_commits = []
    elif not all(
        isinstance(commit, str) and SHA_PATTERN.fullmatch(commit)
        for commit in source_commits
    ):
        errors.append(
            f"{label}.source_commits must contain full lowercase SHAs"
        )
        source_commits = []

    for name, commit in (
        ("source_base", source_base),
        ("source_head", source_head),
        ("result", result),
    ):
        if commit and not SHA_PATTERN.fullmatch(commit):
            errors.append(f"{label}.{name} must be a full lowercase SHA")
    for name, patch_id in (
        ("source_patch_id", source_patch_id),
        ("result_patch_id", result_patch_id),
    ):
        if patch_id and not SHA_PATTERN.fullmatch(patch_id):
            errors.append(f"{label}.{name} must be a stable patch ID")
    if commits_digest and not SHA256_PATTERN.fullmatch(commits_digest):
        errors.append(f"{label}.source_commits_sha256 must be a SHA-256 digest")

    if source_base and base_sha and source_base != base_sha:
        errors.append(f"{label}.source_base does not match base_sha")
    if source_head and source_commits and source_commits[-1] != source_head:
        errors.append(f"{label}.source_head is not the final source commit")
    if len(set(source_commits)) != len(source_commits):
        errors.append(f"{label}.source_commits contains duplicates")
    if (
        source_commits
        and commits_digest
        and source_commits_digest(source_commits) != commits_digest
    ):
        errors.append(f"{label}.source_commits SHA-256 does not match")
    if head_sha and source_commits and head_sha not in source_commits:
        errors.append(f"{task_id}.head_sha is outside the squash source range")
    if (
        source_patch_id
        and result_patch_id
        and source_patch_id != result_patch_id
    ):
        errors.append(f"{label}.source and result patch IDs do not match")

    record_relative = f".agents/records/{task_id}.json"
    if verify_git and source_base and source_head and commit_exists(source_head):
        if head_sha and commit_exists(head_sha):
            if commit_parents(source_head) != [head_sha]:
                errors.append(
                    f"{label}.source_head is not the immutable review commit"
                )
            review_paths = git_text(
                "diff-tree",
                "--no-commit-id",
                "--name-only",
                "-r",
                source_head,
            ).splitlines()
            if review_paths != [record_relative]:
                errors.append(
                    f"{label}.source_head changes paths outside the task record"
                )
            review_message = git_text(
                "show",
                "-s",
                "--format=%B",
                source_head,
            )
            if not re.search(
                r"(?m)^Xnn-Lifecycle:\s+review$",
                review_message,
            ):
                errors.append(
                    f"{label}.source_head is missing the review lifecycle"
                )
            try:
                reviewed_patch = stable_range_patch_id(
                    source_base,
                    head_sha,
                    record_relative,
                )
            except GovernanceError as error:
                errors.append(str(error))
            else:
                if source_patch_id and reviewed_patch != source_patch_id:
                    errors.append(
                        f"{label}.source payload differs from reviewed head"
                    )
        actual_commits = git_text(
            "rev-list", "--reverse", f"{source_base}..{source_head}"
        ).splitlines()
        if source_commits != actual_commits:
            errors.append(
                f"{label}.source_commits does not match the source range"
            )
        for commit in actual_commits:
            if len(commit_parents(commit)) != 1:
                errors.append(
                    f"{label}.source_commits contains merge commit {commit}"
                )
        try:
            actual_source_patch = stable_range_patch_id(
                source_base,
                source_head,
                record_relative,
            )
        except GovernanceError as error:
            errors.append(str(error))
        else:
            if source_patch_id and actual_source_patch != source_patch_id:
                errors.append(f"{label}.source patch ID does not match")

    delivery_result = result
    if state == "integrated" and not delivery_result and verify_git:
        delivery_result = git_text("rev-parse", "HEAD")
    if not verify_git or not delivery_result:
        return
    if not commit_exists(delivery_result):
        errors.append(f"{label}.result is unavailable: {delivery_result}")
        return
    parents = commit_parents(delivery_result)
    if len(parents) != 1:
        errors.append(f"{label}.result must have exactly one parent")
        return
    try:
        actual_result_patch = stable_range_patch_id(
            parents[0],
            delivery_result,
            record_relative,
        )
    except GovernanceError as error:
        errors.append(str(error))
    else:
        if result_patch_id and actual_result_patch != result_patch_id:
            errors.append(f"{label}.result patch ID does not match")

    message = git_text("show", "-s", "--format=%B", delivery_result)
    required_trailers = (
        f"Xnn-Task: {task_id}",
        "Xnn-Integration-Strategy: squash",
        f"Xnn-Source-Base: {source_base}",
        f"Xnn-Source-Head: {source_head}",
        f"Xnn-Source-Commits-SHA256: {commits_digest}",
        f"Xnn-Source-Patch-Id: {source_patch_id}",
    )
    for trailer in required_trailers:
        if trailer not in message.splitlines():
            errors.append(f"{label}.result is missing trailer: {trailer}")
    if verified_sha and not is_ancestor(delivery_result, verified_sha):
        errors.append(f"{label}.result is not reachable from verified_sha")


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


def validate_risks(
    errors: list[str],
    task_id: str,
    risks: Any,
    verification_evidence: list[str],
) -> None:
    label = f"{task_id}.risks"
    if not isinstance(risks, dict):
        errors.append(f"{label} must be an object")
        return

    missing = sorted(set(RISK_DIMENSIONS) - set(risks))
    unexpected = sorted(set(risks) - set(RISK_DIMENSIONS))
    if missing:
        errors.append(f"{label} is missing dimensions: {', '.join(missing)}")
    if unexpected:
        errors.append(
            f"{label} has unexpected dimensions: {', '.join(unexpected)}"
        )

    evidence_set = set(verification_evidence)
    expected_fields = {"level", "rationale", "gates"}
    for dimension in RISK_DIMENSIONS:
        risk = risks.get(dimension)
        risk_label = f"{label}.{dimension}"
        if not isinstance(risk, dict):
            errors.append(f"{risk_label} must be an object")
            continue

        missing_fields = sorted(expected_fields - set(risk))
        unexpected_fields = sorted(set(risk) - expected_fields)
        if missing_fields:
            errors.append(
                f"{risk_label} is missing fields: {', '.join(missing_fields)}"
            )
        if unexpected_fields:
            errors.append(
                f"{risk_label} has unexpected fields: "
                + ", ".join(unexpected_fields)
            )

        level = require_string(
            errors, risk.get("level"), f"{risk_label}.level"
        )
        if level not in RISK_LEVELS:
            errors.append(
                f"{risk_label}.level must be one of {sorted(RISK_LEVELS)}"
            )

        rationale = require_string(
            errors, risk.get("rationale"), f"{risk_label}.rationale"
        )
        if re.search(r"\bTODO\b", rationale, flags=re.IGNORECASE):
            errors.append(f"{risk_label}.rationale still contains TODO")

        gates = risk.get("gates")
        if not isinstance(gates, list) or not all(
            isinstance(gate, str) and gate for gate in gates
        ):
            errors.append(
                f"{risk_label}.gates must be an array of non-empty references"
            )
            gates = []
        if len(gates) != len(set(gates)):
            errors.append(f"{risk_label}.gates contains duplicates")
        if level == "none" and gates:
            errors.append(f"{risk_label}.gates must be empty when level is none")
        elif level in RISK_LEVELS - {"none"} and not gates:
            errors.append(f"{risk_label}.gates must not be empty for {level} risk")
        for gate in gates:
            if gate not in evidence_set:
                errors.append(
                    f"{risk_label}.gates references unexecuted command or "
                    f"gate: {gate}"
                )


def architecture_module_ids() -> set[str]:
    try:
        return architecture_change.module_ids(ARCHITECTURE_MODULES)
    except architecture_change.ArchitectureChangeError as error:
        raise GovernanceError(str(error)) from error


def validate_architecture_change(
    errors: list[str],
    task: dict[str, Any],
    record: dict[str, Any],
    tasks: dict[str, dict[str, Any]],
    module_ids: set[str],
) -> None:
    architecture_change.validate_change(
        errors,
        task,
        record,
        tasks,
        module_ids,
        path_allowed,
    )


def validate_record(
    task: dict[str, Any],
    record: dict[str, Any],
    records: dict[str, dict[str, Any]],
    tasks: dict[str, dict[str, Any]],
    module_ids: set[str],
    gate_registry: dict[str, str],
    *,
    verify_git: bool,
) -> list[str]:
    errors: list[str] = []
    task_id = task["id"]
    task_number = int(task_id.removeprefix("XT-"))
    schema_version = record.get("schema_version")
    if schema_version not in {1, 2, 3}:
        errors.append(f"{task_id}.schema_version must be 1, 2, or 3")
    if task.get("risk_profile_required") is True and schema_version not in {2, 3}:
        errors.append(
            f"{task_id}.schema_version must be 2 or 3 when "
            "risk_profile_required=true"
        )
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

    commit = record.get("commit")
    if task.get("commit_policy_required") is True:
        if not isinstance(commit, dict):
            errors.append(f"{task_id}.commit must be an object")
        else:
            commit_type = require_string(
                errors,
                commit.get("type"),
                f"{task_id}.commit.type",
            )
            scope = require_string(
                errors,
                commit.get("scope"),
                f"{task_id}.commit.scope",
            )
            summary = require_string(
                errors,
                commit.get("summary"),
                f"{task_id}.commit.summary",
            )
            if commit_type not in COMMIT_TYPES:
                errors.append(
                    f"{task_id}.commit.type must be one of "
                    f"{sorted(COMMIT_TYPES)}"
                )
            if scope and not COMMIT_SCOPE_PATTERN.fullmatch(scope):
                errors.append(
                    f"{task_id}.commit.scope must be lowercase and URL-safe"
                )
            if summary and not COMMIT_SUMMARY_PATTERN.fullmatch(summary):
                errors.append(
                    f"{task_id}.commit.summary must start lowercase and "
                    "contain at least 12 characters"
                )
            if summary.endswith((".", "!", "?")):
                errors.append(
                    f"{task_id}.commit.summary must not end with punctuation"
                )
            if TASK_ID_IN_TEXT_PATTERN.search(summary):
                errors.append(
                    f"{task_id}.commit.summary must not contain a task ID"
                )
            subject = f"{commit_type}({scope}): {summary}"
            if len(subject) > 72:
                errors.append(
                    f"{task_id}.commit metadata produces a subject over "
                    "72 characters"
                )
            title = task.get("title", "")
            review_subject = (
                f"chore({scope}): submit "
                f"{title[:1].lower() + title[1:]} for review"
            )
            if len(review_subject) > 72:
                errors.append(
                    f"{task_id}.title produces a lifecycle subject over "
                    "72 characters"
                )
    if task.get("architecture_contract_required") is True:
        validate_architecture_change(
            errors,
            task,
            record,
            tasks,
            module_ids,
        )

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
        commands = []
    elif len(commands) != len(set(commands)):
        errors.append(f"{task_id}.verification.commands contains duplicates")

    gate_ids = verification.get("gates")
    if gate_ids is None:
        if schema_version == 3:
            errors.append(
                f"{task_id}.verification.gates is required by schema version 3"
            )
        verification_evidence = commands
        registered_commands = set(gate_registry.values())
        for command in commands:
            if command not in registered_commands:
                errors.append(
                    f"{task_id}.verification.commands contains unregistered "
                    f"command: {command}"
                )
        if "make verify" not in commands:
            errors.append(
                f"{task_id}.verification.commands must include make verify"
            )
    else:
        if not isinstance(gate_ids, list) or not gate_ids or not all(
            isinstance(gate, str) and gate for gate in gate_ids
        ):
            errors.append(
                f"{task_id}.verification.gates must be a non-empty array"
            )
            gate_ids = []
        elif len(gate_ids) != len(set(gate_ids)):
            errors.append(f"{task_id}.verification.gates contains duplicates")
        for gate in gate_ids:
            if gate not in gate_registry:
                errors.append(
                    f"{task_id}.verification.gates contains unknown gate: "
                    f"{gate}"
                )
        resolved = [
            gate_registry[gate] for gate in gate_ids if gate in gate_registry
        ]
        if commands != resolved:
            errors.append(
                f"{task_id}.verification.commands must match trusted gates"
            )
        if "verify" not in gate_ids:
            errors.append(f"{task_id}.verification.gates must include verify")
        verification_evidence = gate_ids

    if schema_version in {2, 3}:
        validate_risks(
            errors,
            task_id,
            record.get("risks"),
            verification_evidence,
        )
    if schema_version == 3:
        validate_schema_v3(
            errors,
            task_id,
            record,
            state,
            gate_registry,
            gate_ids if isinstance(gate_ids, list) else [],
            verify_git=verify_git,
        )

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
        strategy = integration.get("strategy")
        if strategy not in {"cherry-pick", "merge", "squash"}:
            errors.append(f"{task_id}.integration.strategy is invalid")
        verified_sha = validate_verified_sha(
            errors,
            task_id,
            state,
            integration,
            verify_git=verify_git,
        )
        if strategy in {"cherry-pick", "merge"}:
            validate_cherry_pick_integration(
                errors,
                task_id,
                head_sha,
                integration,
                verified_sha,
                verify_git=verify_git,
            )
        elif strategy == "squash":
            validate_squash_integration(
                errors,
                task_id,
                state,
                base_sha,
                head_sha,
                integration,
                verified_sha,
                verify_git=verify_git,
            )

    return errors


def validate_repository(*, verify_git: bool = True) -> None:
    document, tasks = load_backlog()
    errors: list[str] = []
    try:
        gate_registry = trusted_gate_registry()
    except GovernanceError as error:
        errors.append(str(error))
        gate_registry = {}
    try:
        module_ids = architecture_module_ids()
    except GovernanceError as error:
        errors.append(str(error))
        module_ids = set()
    if document.get("schema_version") != 1:
        errors.append("backlog schema_version must be 1")

    for task_id, task in tasks.items():
        if not re.fullmatch(r"XT-[0-9]{3,}", task_id):
            errors.append(f"Invalid task id: {task_id}")
            task_number = 0
        else:
            task_number = int(task_id.removeprefix("XT-"))
        if task.get("readiness") not in {"ready", "blocked", "done"}:
            errors.append(f"Invalid readiness for {task_id}")
        risk_profile_required = task.get("risk_profile_required")
        if risk_profile_required is not None and not isinstance(
            risk_profile_required, bool
        ):
            errors.append(f"{task_id}.risk_profile_required must be boolean")
        if (
            task_number >= RISK_SCHEMA_MIN_TASK_NUMBER
            and risk_profile_required is not True
        ):
            errors.append(
                f"{task_id}.risk_profile_required must be true for new tasks"
            )
        commit_policy_required = task.get("commit_policy_required")
        if commit_policy_required is not None and not isinstance(
            commit_policy_required, bool
        ):
            errors.append(f"{task_id}.commit_policy_required must be boolean")
        architecture_contract_required = task.get(
            "architecture_contract_required"
        )
        if architecture_contract_required is not None and not isinstance(
            architecture_contract_required, bool
        ):
            errors.append(
                f"{task_id}.architecture_contract_required must be boolean"
            )
        if (
            task_number >= ARCHITECTURE_SCHEMA_MIN_TASK_NUMBER
            and architecture_contract_required is not True
        ):
            errors.append(
                f"{task_id}.architecture_contract_required must be true "
                "for new tasks"
            )
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

    try:
        module_document = load_json(ARCHITECTURE_MODULES)
        raw_modules = module_document.get("modules", [])
    except GovernanceError as error:
        errors.append(str(error))
        raw_modules = []
    for module in raw_modules:
        if not isinstance(module, dict):
            continue
        replacement_task = module.get("placeholder_until")
        module_id = module.get("id")
        if replacement_task is None:
            continue
        task = tasks.get(replacement_task)
        record = records.get(replacement_task)
        if task is None or record is None:
            errors.append(
                f"Module {module_id} references unknown replacement task "
                f"{replacement_task}"
            )
            continue
        if task.get("architecture_contract_required") is not True:
            errors.append(
                f"{replacement_task}.architecture_contract_required must "
                f"be true for placeholder module {module_id}"
            )
        change = record.get("architecture_change")
        if not isinstance(change, dict):
            errors.append(
                f"{replacement_task}.architecture_change must bind "
                f"placeholder module {module_id}"
            )
        else:
            if change.get("mode") != "replace":
                errors.append(
                    f"{replacement_task}.architecture_change.mode must be "
                    f"replace for placeholder module {module_id}"
                )
            if module_id not in change.get("modules", []):
                errors.append(
                    f"{replacement_task}.architecture_change.modules must "
                    f"contain {module_id}"
                )

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
                tasks,
                module_ids,
                gate_registry,
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


def validate_architecture_review(
    task: dict[str, Any],
    record: dict[str, Any],
    worktree: Path,
    changed_paths: list[str],
) -> None:
    try:
        architecture_change.validate_review(
            task,
            record,
            worktree,
            changed_paths,
        )
    except architecture_change.ArchitectureChangeError as error:
        raise GovernanceError(str(error)) from error


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
    changed_paths = [path for path in changed.splitlines() if path]
    patterns = tasks[task_id]["owned_paths"]
    outside = [
        path
        for path in changed_paths
        if not path_allowed(path, patterns, task_id)
    ]
    if outside:
        raise GovernanceError(
            "Changed paths outside task ownership:\n"
            + "\n".join(f"- {path}" for path in outside)
        )
    validate_architecture_review(
        tasks[task_id],
        record,
        worktree,
        changed_paths,
    )

    commit_check = subprocess.run(
        [
            sys.executable,
            "-B",
            str(worktree / "tool" / "harness" / "commit_message.py"),
            "range",
            "--root",
            str(worktree),
            base_sha,
            head_sha,
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if commit_check.returncode != 0:
        raise GovernanceError(commit_check.stderr.strip())

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
    original = copy.deepcopy(record)
    path = record_path(task_id)
    try:
        defect_proof_runner.run_proof(ROOT, task_id, head_sha)
        record = load_record(task_id)
        record["state"] = "review"
        record["head_sha"] = head_sha
        record["verification"]["status"] = "passed"
        record["verification"]["reference"] = reference
        write_json(path, record)
        validate_repository()
    except (GovernanceError, defect_proof_runner.DefectProofError) as error:
        write_json(path, original)
        if isinstance(error, GovernanceError):
            raise
        raise GovernanceError(str(error)) from error


def mark_integrated(task_id: str, provenance_path: Path) -> None:
    record = load_record(task_id)
    if record.get("state") != "review":
        raise GovernanceError(f"{task_id} must be in review before integration")
    provenance = load_json(provenance_path)
    strategy = provenance.get("strategy")
    if strategy == "squash":
        required = (
            "source_base",
            "source_head",
            "source_commits",
            "source_commits_sha256",
            "source_patch_id",
            "result",
            "result_patch_id",
            "verified_sha",
        )
        missing = [field for field in required if field not in provenance]
        if missing:
            raise GovernanceError(
                "Squash provenance is missing fields: " + ", ".join(missing)
            )
    elif strategy == "cherry-pick":
        mappings = provenance.get("mappings")
        if not isinstance(mappings, list) or not mappings:
            raise GovernanceError("Integration mapping file has no mappings")
    else:
        raise GovernanceError(f"Unsupported integration strategy: {strategy}")
    record["state"] = "integrated"
    record["integration"] = provenance
    write_json(record_path(task_id), record)


def mark_accepted(task_id: str, reviewer: str, reference: str, verified_sha: str) -> None:
    record = load_record(task_id)
    if record.get("state") != "integrated":
        raise GovernanceError(f"{task_id} must be integrated before acceptance")
    record["state"] = "done"
    if (
        record["integration"].get("strategy") == "squash"
        and not record["integration"].get("result")
    ):
        record["integration"]["result"] = verified_sha
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
    verification = record.get("verification", {})
    commands = verification.get("commands", [])
    if not isinstance(commands, list):
        raise GovernanceError(f"{task_id} has invalid verification commands")
    registry = trusted_gate_registry()
    gates = verification.get("gates")
    if gates is not None:
        if not isinstance(gates, list) or not all(
            isinstance(gate, str) and gate in registry for gate in gates
        ):
            raise GovernanceError(f"{task_id} has invalid verification gates")
        commands = [registry[gate] for gate in gates]
    elif not all(command in set(registry.values()) for command in commands):
        raise GovernanceError(
            f"{task_id} has an unregistered verification command"
        )
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
    integrated_parser.add_argument("provenance_path", type=Path)

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
            mark_integrated(args.task_id, args.provenance_path)
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
