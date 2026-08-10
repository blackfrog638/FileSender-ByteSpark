#!/usr/bin/env python3

"""Record and replay deterministic TDD checkpoints for schema-v4 tasks."""

from __future__ import annotations

import argparse
import datetime as dt
import fnmatch
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, NamedTuple

from trusted_gates import GateRegistryError, load_gate_registry


ROOT = Path(__file__).resolve().parents[2]
SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
SKIP_PATTERN = re.compile(r"\b(?:skip|skipped)\b", re.IGNORECASE)
ZERO_SKIP_PATTERN = re.compile(
    r"\b0\s+(?:tests?\s+)?skipped\b",
    re.IGNORECASE,
)
FAILING_PROOF_MODES = {"red_green", "regression", "mutation"}
PASSING_PROOF_MODES = {"equivalence"}
SURFACE_PROOF_MODES = FAILING_PROOF_MODES | PASSING_PROOF_MODES
GATE_TIMEOUT_SECONDS = 900.0
REGISTRATION_NAMES = {
    "BUILD",
    "BUILD.bazel",
    "CMakeLists.txt",
    "Makefile",
    "meson.build",
}
PRODUCTION_TREE_SEGMENTS = {"include", "lib", "src"}
CHECKPOINT_FIELDS = {
    "mode",
    "gate",
    "executor",
    "command_sha256",
    "plan_content_sha256",
    "criterion_ids_sha256",
    "proof_surface_sha256",
    "failure_fingerprints_sha256",
    "frozen_surface_sha256",
    "governance_context_sha256",
    "base_commit",
    "red_commit",
    "base_exit_code",
    "red_exit_code",
    "red_output_sha256",
    "checked_at",
}
PROOF_FIELDS = CHECKPOINT_FIELDS | {
    "checkpoint_commit",
    "checkpoint_sha256",
    "head_commit",
    "head_exit_code",
    "replayed_red_output_sha256",
}


class TddProofError(RuntimeError):
    pass


class GateRun(NamedTuple):
    result: subprocess.CompletedProcess[str]
    dirty: bool


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


def canonical_digest(value: Any) -> str:
    encoded = json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def string_digest(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise TddProofError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise TddProofError(f"{path} must contain an object")
    return value


def load_record(root: Path, task_id: str) -> dict[str, Any]:
    return load_json(root / ".agents" / "records" / f"{task_id}.json")


def write_record(
    root: Path,
    task_id: str,
    record: dict[str, Any],
) -> None:
    path = root / ".agents" / "records" / f"{task_id}.json"
    path.write_text(
        json.dumps(record, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )


def load_task(root: Path, task_id: str) -> dict[str, Any]:
    document = load_json(root / ".agents" / "backlog.yaml")
    tasks = document.get("tasks")
    if not isinstance(tasks, list):
        raise TddProofError("backlog tasks must be an array")
    matches = [
        task
        for task in tasks
        if isinstance(task, dict) and task.get("id") == task_id
    ]
    if len(matches) != 1:
        raise TddProofError(f"backlog must contain exactly one {task_id}")
    return matches[0]


def commit_exists(root: Path, commit: str) -> bool:
    return (
        SHA_PATTERN.fullmatch(commit) is not None
        and git(
            root,
            "cat-file",
            "-e",
            f"{commit}^{{commit}}",
            check=False,
        ).returncode
        == 0
    )


def is_ancestor(root: Path, ancestor: str, descendant: str) -> bool:
    return (
        git(
            root,
            "merge-base",
            "--is-ancestor",
            ancestor,
            descendant,
            check=False,
        ).returncode
        == 0
    )


def proof_environment(root: Path) -> dict[str, str]:
    environment = os.environ.copy()
    environment["XNN_TRANSFER_VCPKG_ROOT"] = str(
        (root / "out" / "tools" / "vcpkg").resolve()
    )
    return environment


def result_output(result: subprocess.CompletedProcess[str]) -> str:
    return result.stdout + result.stderr


def output_digest(result: subprocess.CompletedProcess[str]) -> str:
    return string_digest(result_output(result))


def result_tail(result: subprocess.CompletedProcess[str]) -> str:
    output = result_output(result)
    return output[-4000:] if output else "(no output)"


def remove_worktree(root: Path, worktree: Path) -> None:
    removed = git(
        root,
        "worktree",
        "remove",
        "--force",
        str(worktree),
        check=False,
    )
    if removed.returncode != 0:
        raise TddProofError(
            f"cannot remove proof worktree {worktree}:\n"
            + (removed.stderr or removed.stdout)
        )


def run_detached_gate(
    root: Path,
    commit: str,
    command: str,
    worktree: Path,
) -> GateRun:
    added = git(
        root,
        "worktree",
        "add",
        "--detach",
        str(worktree),
        commit,
        check=False,
    )
    if added.returncode != 0:
        raise TddProofError(
            f"cannot create detached worktree for {commit}:\n"
            + (added.stderr or added.stdout)
        )
    try:
        try:
            result = subprocess.run(
                ["bash", "-lc", command],
                cwd=worktree,
                env=proof_environment(root),
                check=False,
                capture_output=True,
                text=True,
                timeout=GATE_TIMEOUT_SECONDS,
            )
        except subprocess.TimeoutExpired as error:
            raise TddProofError(
                f"trusted gate timed out after {GATE_TIMEOUT_SECONDS:g}s "
                f"at {commit[:12]}"
            ) from error
        dirty = bool(git_text(worktree, "status", "--porcelain"))
        return GateRun(result=result, dirty=dirty)
    finally:
        remove_worktree(root, worktree)


def tree_entries(root: Path, commit: str) -> dict[str, tuple[str, str]]:
    result = git(
        root,
        "ls-tree",
        "-r",
        "--full-tree",
        commit,
        check=False,
    )
    if result.returncode != 0:
        raise TddProofError(
            f"cannot inspect tree at {commit}:\n"
            + (result.stderr or result.stdout)
        )
    entries: dict[str, tuple[str, str]] = {}
    for line in result.stdout.splitlines():
        metadata, path = line.split("\t", 1)
        mode, object_type, object_id = metadata.split(" ", 2)
        if object_type == "blob":
            entries[path] = (mode, object_id)
    return entries


def selected_tree_digest(
    entries: dict[str, tuple[str, str]],
    paths: list[str],
) -> str:
    selected = [
        [path, entries[path][0], entries[path][1]]
        for path in sorted(paths)
        if path in entries
    ]
    return canonical_digest(selected)


def proof_surface_paths(
    entries: dict[str, tuple[str, str]],
    patterns: list[str],
) -> list[str]:
    return sorted(
        path
        for path in entries
        if any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)
    )


def frozen_surface_digest(
    root: Path,
    commit: str,
    patterns: list[str],
) -> str:
    entries = tree_entries(root, commit)
    selected = proof_surface_paths(entries, patterns)
    if not selected:
        raise TddProofError(
            "declared proof surface does not match a tracked file at Red"
        )
    return selected_tree_digest(entries, selected)


def governance_context_digest(
    root: Path,
    commit: str,
    task_id: str,
    plan_id: str,
) -> str:
    entries = tree_entries(root, commit)
    exact = {
        ".agents/manifest.yaml",
        f".agents/plans/{plan_id}.json",
        "tool/harness/agent.sh",
        "tool/harness/governance.py",
        "tool/harness/tdd_contract.py",
        "tool/harness/tdd_proof.py",
        "tool/harness/trusted_gates.py",
    }
    prefixes = (
        f".agents/tasks/{task_id}-",
        ".github/workflows/",
    )
    selected = sorted(
        path
        for path in entries
        if path in exact or path.startswith(prefixes)
    )
    backlog_result = git(
        root,
        "show",
        f"{commit}:.agents/backlog.yaml",
        check=False,
    )
    if backlog_result.returncode != 0:
        raise TddProofError(
            f"cannot read backlog at {commit}:\n"
            + (backlog_result.stderr or backlog_result.stdout)
        )
    try:
        backlog = json.loads(backlog_result.stdout)
    except json.JSONDecodeError as error:
        raise TddProofError(
            f"backlog at {commit} is invalid: {error}"
        ) from error
    tasks = backlog.get("tasks") if isinstance(backlog, dict) else None
    matches = [
        task
        for task in tasks or []
        if isinstance(task, dict) and task.get("id") == task_id
    ]
    if not isinstance(tasks, list) or len(matches) != 1:
        raise TddProofError(
            f"backlog at {commit} must contain exactly one {task_id}"
        )
    return canonical_digest(
        {
            "tracked_context_sha256": selected_tree_digest(
                entries,
                selected,
            ),
            "task_contract_sha256": canonical_digest(matches[0]),
        }
    )


def commit_changed_paths(root: Path, commit: str) -> list[str]:
    parents = git_text(root, "rev-list", "--parents", "-n", "1", commit).split()
    if len(parents) > 2:
        raise TddProofError(
            f"merge commit {commit[:12]} is not allowed before Red"
        )
    result = git(
        root,
        "diff-tree",
        "--root",
        "--no-commit-id",
        "--name-only",
        "-r",
        "--no-renames",
        commit,
    )
    return [path for path in result.stdout.splitlines() if path]


def is_registration_path(path: str) -> bool:
    return Path(path).name in REGISTRATION_NAMES


def is_test_path(path: str) -> bool:
    lowered = path.lower()
    parts = lowered.split("/")
    if any(part in PRODUCTION_TREE_SEGMENTS for part in parts[:-1]):
        return False
    name = parts[-1]
    stem = name.rsplit(".", 1)[0]
    test_segments = {
        "fixture",
        "fixtures",
        "fuzz",
        "fuzzing",
        "golden",
        "goldens",
        "scenario",
        "scenarios",
        "snapshot",
        "snapshots",
        "test",
        "testdata",
        "tests",
    }
    return (
        any(part in test_segments for part in parts)
        or stem.startswith("test_")
        or stem.endswith("_test")
        or ".golden." in lowered
        or ".snap." in lowered
    )


def scan_pre_red_history(
    root: Path,
    task_id: str,
    base: str,
    red: str,
    proof_surface: list[str],
) -> None:
    commits = git_text(
        root,
        "rev-list",
        "--reverse",
        f"{base}..{red}",
    ).splitlines()
    record_path = f".agents/records/{task_id}.json"
    for commit in commits:
        for path in commit_changed_paths(root, commit):
            if path == record_path:
                continue
            matching = [
                pattern
                for pattern in proof_surface
                if fnmatch.fnmatchcase(path, pattern)
            ]
            if not matching or not is_test_path(path):
                raise TddProofError(
                    "production or undeclared path changed before Red: "
                    f"{path} ({commit[:12]})"
                )
            if is_registration_path(path) and path not in proof_surface:
                raise TddProofError(
                    "test registration path must be exactly declared in "
                    f"proof_surface: {path}"
                )


def has_skipped_result(result: subprocess.CompletedProcess[str]) -> bool:
    for line in result_output(result).splitlines():
        if SKIP_PATTERN.search(line) and not ZERO_SKIP_PATTERN.search(line):
            return True
    return False


def ensure_clean_gate_run(run: GateRun, label: str) -> None:
    if run.dirty:
        raise TddProofError(
            f"trusted gate changed tracked content at {label}"
        )
    if has_skipped_result(run.result):
        raise TddProofError(f"trusted gate reported a skipped result at {label}")


def validate_base_result(run: GateRun) -> None:
    ensure_clean_gate_run(run, "task base")
    if run.result.returncode != 0:
        raise TddProofError(
            f"trusted gate failed at task base with exit "
            f"{run.result.returncode}:\n{result_tail(run.result)}"
        )


def validate_red_result(
    run: GateRun,
    proof_mode: str,
    fingerprints: list[str],
) -> None:
    ensure_clean_gate_run(run, "Red")
    exit_code = run.result.returncode
    if proof_mode in FAILING_PROOF_MODES:
        if exit_code == 0:
            raise TddProofError(
                "trusted gate unexpectedly passed at Red:\n"
                + result_tail(run.result)
            )
        if exit_code in {126, 127}:
            raise TddProofError(
                f"trusted gate returned infrastructure exit {exit_code} "
                f"at Red:\n{result_tail(run.result)}"
            )
        if exit_code < 0 or exit_code >= 128:
            raise TddProofError(
                f"trusted gate returned crash exit {exit_code} at Red:\n"
                f"{result_tail(run.result)}"
            )
        output_lines = set(result_output(run.result).splitlines())
        missing = [
            fingerprint
            for fingerprint in fingerprints
            if fingerprint not in output_lines
        ]
        if missing:
            raise TddProofError(
                "Red failure did not contain all declared failure "
                f"fingerprints as complete lines: {missing}\n"
                f"{result_tail(run.result)}"
            )
    elif proof_mode in PASSING_PROOF_MODES:
        if exit_code != 0:
            raise TddProofError(
                f"characterization gate failed at Red with exit {exit_code}:\n"
                f"{result_tail(run.result)}"
            )
    else:
        raise TddProofError(
            f"proof mode {proof_mode!r} cannot record a Red checkpoint"
        )


def validate_head_result(run: GateRun) -> None:
    ensure_clean_gate_run(run, "reviewed head")
    if run.result.returncode != 0:
        raise TddProofError(
            f"trusted gate failed at reviewed head with exit "
            f"{run.result.returncode}:\n{result_tail(run.result)}"
        )


def contract_values(
    root: Path,
    task_id: str,
    record: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any], str]:
    if record.get("schema_version") != 4:
        raise TddProofError(f"{task_id} must use task-record schema version 4")
    contract = record.get("test_contract")
    if not isinstance(contract, dict):
        raise TddProofError(f"{task_id}.test_contract must be an object")
    if contract.get("executor") != "deterministic":
        raise TddProofError(
            f"executor {contract.get('executor')!r} has no local checkpoint "
            "runner"
        )
    proof_mode = contract.get("proof_mode")
    if proof_mode not in SURFACE_PROOF_MODES:
        raise TddProofError(
            f"proof mode {proof_mode!r} cannot record a Red checkpoint"
        )
    gate = contract.get("gate")
    if not isinstance(gate, str):
        raise TddProofError("test contract gate must be a string")
    try:
        registry = load_gate_registry(root / ".agents" / "manifest.yaml")
    except GateRegistryError as error:
        raise TddProofError(str(error)) from error
    command = registry.get(gate)
    if command is None:
        raise TddProofError(f"test contract gate is not registered: {gate}")
    verification = record.get("verification")
    gates = (
        verification.get("gates")
        if isinstance(verification, dict)
        else None
    )
    if not isinstance(gates, list) or gate not in gates:
        raise TddProofError(
            "test contract gate must appear in verification.gates"
        )
    proof_surface = contract.get("proof_surface")
    if not isinstance(proof_surface, list) or not proof_surface or not all(
        isinstance(path, str) and path for path in proof_surface
    ):
        raise TddProofError("test contract proof_surface must not be empty")
    criterion_ids = contract.get("criterion_ids")
    if not isinstance(criterion_ids, list) or not criterion_ids or not all(
        isinstance(criterion_id, str) and criterion_id
        for criterion_id in criterion_ids
    ):
        raise TddProofError("test contract criterion_ids must not be empty")
    plan_digest = contract.get("plan_content_sha256")
    if not isinstance(plan_digest, str) or not SHA256_PATTERN.fullmatch(
        plan_digest
    ):
        raise TddProofError(
            "test contract plan_content_sha256 must be a SHA-256 digest"
        )
    fingerprints = contract.get("failure_fingerprints")
    if not isinstance(fingerprints, list) or not all(
        isinstance(value, str) and value for value in fingerprints
    ):
        raise TddProofError(
            "test contract failure_fingerprints must be an array"
        )
    if proof_mode in FAILING_PROOF_MODES and not fingerprints:
        raise TddProofError(
            "failing proof mode requires declared failure fingerprints"
        )
    if proof_mode in PASSING_PROOF_MODES and fingerprints:
        raise TddProofError(
            "equivalence proof must not declare failure fingerprints"
        )
    if proof_mode == "regression":
        defect = record.get("defect")
        if not isinstance(defect, dict):
            raise TddProofError("regression proof requires defect metadata")
        if defect.get("regression_gate") != gate:
            raise TddProofError(
                "defect regression_gate must match test contract gate"
            )
        if defect.get("failure_fingerprint") not in fingerprints:
            raise TddProofError(
                "defect failure_fingerprint must appear in the test contract"
            )
    if contract.get("allow_skipped") is not False:
        raise TddProofError("test contract allow_skipped must be false")
    return contract, registry, command


def frozen_metadata(
    root: Path,
    task_id: str,
    task: dict[str, Any],
    record: dict[str, Any],
    commit: str,
    command: str,
) -> dict[str, str]:
    contract = record["test_contract"]
    proof_surface = contract["proof_surface"]
    plan_id = task.get("delivery_plan")
    if not isinstance(plan_id, str) or not plan_id:
        raise TddProofError(f"{task_id} has no Delivery Plan")
    return {
        "command_sha256": string_digest(command),
        "plan_content_sha256": contract["plan_content_sha256"],
        "criterion_ids_sha256": canonical_digest(
            contract["criterion_ids"]
        ),
        "proof_surface_sha256": canonical_digest(proof_surface),
        "failure_fingerprints_sha256": canonical_digest(
            contract["failure_fingerprints"]
        ),
        "frozen_surface_sha256": frozen_surface_digest(
            root,
            commit,
            proof_surface,
        ),
        "governance_context_sha256": governance_context_digest(
            root,
            commit,
            task_id,
            plan_id,
        ),
    }


def checkpoint_digest(checkpoint: dict[str, Any]) -> str:
    return canonical_digest(checkpoint)


def checkpoint_lifecycle_commit(
    root: Path,
    task_id: str,
    checkpoint: dict[str, Any],
    head_commit: str,
) -> str:
    red = checkpoint["red_commit"]
    descendants = git_text(
        root,
        "rev-list",
        "--reverse",
        "--ancestry-path",
        f"{red}..{head_commit}",
    ).splitlines()
    if not descendants:
        raise TddProofError(
            "checkpoint lifecycle commit is missing after Red"
        )
    checkpoint_commit = descendants[0]
    parents = git_text(
        root,
        "rev-list",
        "--parents",
        "-n",
        "1",
        checkpoint_commit,
    ).split()[1:]
    record_path = f".agents/records/{task_id}.json"
    changed_paths = commit_changed_paths(root, checkpoint_commit)
    message = git_text(root, "show", "-s", "--format=%B", checkpoint_commit)
    if (
        parents != [red]
        or changed_paths != [record_path]
        or f"Xnn-Task: {task_id}" not in message.splitlines()
        or "Xnn-Lifecycle: checkpoint" not in message.splitlines()
    ):
        raise TddProofError(
            "Red must be followed by a generated checkpoint lifecycle "
            "commit that only changes the task record"
        )
    record_result = git(
        root,
        "show",
        f"{checkpoint_commit}:{record_path}",
        check=False,
    )
    if record_result.returncode != 0:
        raise TddProofError(
            "checkpoint lifecycle commit does not contain the task record"
        )
    try:
        checkpoint_record = json.loads(record_result.stdout)
    except json.JSONDecodeError as error:
        raise TddProofError(
            "checkpoint lifecycle commit contains an invalid task record"
        ) from error
    committed_checkpoint = (
        checkpoint_record.get("verification", {}).get("tdd_checkpoint")
        if isinstance(checkpoint_record, dict)
        else None
    )
    if committed_checkpoint != checkpoint:
        raise TddProofError(
            "checkpoint lifecycle commit does not contain the frozen "
            "checkpoint"
        )
    return checkpoint_commit


def record_red_checkpoint(
    root: Path,
    task_id: str,
) -> dict[str, Any]:
    root = root.resolve()
    record = load_record(root, task_id)
    if record.get("state") != "in_progress":
        raise TddProofError(
            f"{task_id} must be in_progress before a Red checkpoint"
        )
    if git_text(root, "status", "--porcelain"):
        raise TddProofError("task worktree must be clean before checkpoint")
    current = git_text(root, "rev-parse", "HEAD")
    base = record.get("base_sha")
    if not isinstance(base, str) or not commit_exists(root, base):
        raise TddProofError("task base must be an available full commit SHA")
    if base == current or not is_ancestor(root, base, current):
        raise TddProofError(
            "Red commit must be a descendant of and differ from task base"
        )

    task = load_task(root, task_id)
    contract, _, command = contract_values(root, task_id, record)
    proof_surface = contract["proof_surface"]
    scan_pre_red_history(root, task_id, base, current, proof_surface)
    metadata = frozen_metadata(
        root,
        task_id,
        task,
        record,
        current,
        command,
    )

    with tempfile.TemporaryDirectory(
        prefix=f"xnn-{task_id.lower()}-checkpoint-"
    ) as temporary:
        base_run = run_detached_gate(
            root,
            base,
            command,
            Path(temporary) / "base",
        )
        red_run = run_detached_gate(
            root,
            current,
            command,
            Path(temporary) / "red",
        )
    validate_base_result(base_run)
    validate_red_result(
        red_run,
        contract["proof_mode"],
        contract["failure_fingerprints"],
    )

    checkpoint: dict[str, Any] = {
        "mode": contract["proof_mode"],
        "gate": contract["gate"],
        "executor": contract["executor"],
        **metadata,
        "base_commit": base,
        "red_commit": current,
        "base_exit_code": base_run.result.returncode,
        "red_exit_code": red_run.result.returncode,
        "red_output_sha256": output_digest(red_run.result),
        "checked_at": dt.datetime.now(dt.timezone.utc).isoformat(
            timespec="seconds"
        ),
    }
    verification = record.get("verification")
    assert isinstance(verification, dict)
    if contract["proof_mode"] == "regression":
        defect = record["defect"]
        defect["reproduction_commit"] = current
    verification["tdd_checkpoint"] = checkpoint
    verification.pop("tdd_proof", None)
    write_record(root, task_id, record)
    return checkpoint


def require_checkpoint(
    root: Path,
    task_id: str,
    task: dict[str, Any],
    record: dict[str, Any],
    command: str,
    head_commit: str,
) -> tuple[dict[str, Any], dict[str, str]]:
    verification = record.get("verification")
    checkpoint = (
        verification.get("tdd_checkpoint")
        if isinstance(verification, dict)
        else None
    )
    if not isinstance(checkpoint, dict):
        raise TddProofError(
            f"{task_id}.verification.tdd_checkpoint must be an object"
        )
    if set(checkpoint) != CHECKPOINT_FIELDS:
        raise TddProofError(
            f"{task_id}.verification.tdd_checkpoint has invalid fields"
        )
    base = record.get("base_sha")
    red = checkpoint.get("red_commit")
    if checkpoint.get("base_commit") != base:
        raise TddProofError("checkpoint base_commit does not match base_sha")
    if not isinstance(red, str) or not commit_exists(root, red):
        raise TddProofError("checkpoint Red commit is unavailable")
    if (
        not isinstance(base, str)
        or not commit_exists(root, base)
        or not is_ancestor(root, base, red)
        or base == red
    ):
        raise TddProofError("checkpoint Red commit is outside the task base")
    if not is_ancestor(root, red, head_commit) or red == head_commit:
        raise TddProofError(
            "checkpoint Red commit must be an ancestor of and differ from head"
        )

    contract = record["test_contract"]
    if (
        contract["proof_mode"] == "regression"
        and record["defect"].get("reproduction_commit") != red
    ):
        raise TddProofError(
            "defect reproduction_commit does not match checkpoint Red commit"
        )
    if (
        checkpoint.get("mode") != contract["proof_mode"]
        or checkpoint.get("gate") != contract["gate"]
        or checkpoint.get("executor") != contract["executor"]
    ):
        raise TddProofError("checkpoint no longer matches test contract")
    metadata = frozen_metadata(
        root,
        task_id,
        task,
        record,
        red,
        command,
    )
    for field, expected in metadata.items():
        if checkpoint.get(field) != expected:
            raise TddProofError(
                f"checkpoint {field} does not match frozen contract"
            )
    if (
        frozen_surface_digest(
            root,
            head_commit,
            contract["proof_surface"],
        )
        != checkpoint["frozen_surface_sha256"]
    ):
        raise TddProofError("frozen proof surface changed after Red")
    plan_id = task["delivery_plan"]
    if (
        governance_context_digest(
            root,
            head_commit,
            task_id,
            plan_id,
        )
        != checkpoint["governance_context_sha256"]
    ):
        raise TddProofError(
            "frozen governance context changed after Red; record a fresh "
            "checkpoint"
        )
    scan_pre_red_history(
        root,
        task_id,
        base,
        red,
        contract["proof_surface"],
    )
    return checkpoint, metadata


def run_review_proof(
    root: Path,
    task_id: str,
    head_commit: str,
) -> dict[str, Any] | None:
    root = root.resolve()
    record = load_record(root, task_id)
    if record.get("schema_version") != 4:
        return None
    if record.get("state") != "in_progress":
        raise TddProofError(
            f"{task_id} must be in_progress for deterministic TDD proof"
        )
    if not SHA_PATTERN.fullmatch(head_commit):
        raise TddProofError("reviewed head must be a full lowercase SHA")
    if git_text(root, "rev-parse", "HEAD") != head_commit:
        raise TddProofError("reviewed head does not match task worktree")
    if git_text(root, "status", "--porcelain"):
        raise TddProofError("task worktree must be clean before TDD proof")

    task = load_task(root, task_id)
    contract, _, command = contract_values(root, task_id, record)
    checkpoint, metadata = require_checkpoint(
        root,
        task_id,
        task,
        record,
        command,
        head_commit,
    )
    checkpoint_commit = checkpoint_lifecycle_commit(
        root,
        task_id,
        checkpoint,
        head_commit,
    )
    base = checkpoint["base_commit"]
    red = checkpoint["red_commit"]
    with tempfile.TemporaryDirectory(
        prefix=f"xnn-{task_id.lower()}-review-proof-"
    ) as temporary:
        base_run = run_detached_gate(
            root,
            base,
            command,
            Path(temporary) / "base",
        )
        red_run = run_detached_gate(
            root,
            red,
            command,
            Path(temporary) / "red",
        )
        head_run = run_detached_gate(
            root,
            head_commit,
            command,
            Path(temporary) / "head",
        )
    validate_base_result(base_run)
    validate_red_result(
        red_run,
        contract["proof_mode"],
        contract["failure_fingerprints"],
    )
    validate_head_result(head_run)

    proof: dict[str, Any] = {
        "mode": contract["proof_mode"],
        "gate": contract["gate"],
        "executor": contract["executor"],
        **metadata,
        "base_commit": base,
        "red_commit": red,
        "head_commit": head_commit,
        "checkpoint_commit": checkpoint_commit,
        "base_exit_code": base_run.result.returncode,
        "red_exit_code": red_run.result.returncode,
        "head_exit_code": head_run.result.returncode,
        "red_output_sha256": checkpoint["red_output_sha256"],
        "replayed_red_output_sha256": output_digest(red_run.result),
        "checkpoint_sha256": checkpoint_digest(checkpoint),
        "checked_at": dt.datetime.now(dt.timezone.utc).isoformat(
            timespec="seconds"
        ),
    }
    verification = record["verification"]
    verification["tdd_proof"] = proof
    write_record(root, task_id, record)
    return proof


def validate_digest(
    errors: list[str],
    value: Any,
    label: str,
) -> str:
    if not isinstance(value, str) or not SHA256_PATTERN.fullmatch(value):
        errors.append(f"{label} must be a SHA-256 digest")
        return ""
    return value


def validate_timestamp(
    errors: list[str],
    value: Any,
    label: str,
) -> None:
    if not isinstance(value, str) or not value:
        errors.append(f"{label} must be an ISO-8601 timestamp")
        return
    try:
        parsed = dt.datetime.fromisoformat(value)
    except ValueError:
        errors.append(f"{label} must be an ISO-8601 timestamp")
    else:
        if parsed.tzinfo is None:
            errors.append(f"{label} must include a timezone")


def validate_evidence_object(
    errors: list[str],
    label: str,
    value: Any,
    fields: set[str],
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


def validate_tdd_evidence(
    root: Path,
    task: dict[str, Any],
    record: dict[str, Any],
    gate_registry: dict[str, str],
    *,
    verify_git: bool,
) -> list[str]:
    if record.get("schema_version") != 4:
        return []
    errors: list[str] = []
    task_id = str(task.get("id", "<unknown>"))
    contract = record.get("test_contract")
    if not isinstance(contract, dict):
        return [f"{task_id}.test_contract must be an object"]
    verification = record.get("verification")
    if not isinstance(verification, dict):
        return [f"{task_id}.verification must be an object"]
    state = record.get("state")
    checkpoint_raw = verification.get("tdd_checkpoint")
    proof_raw = verification.get("tdd_proof")
    surface_mode = contract.get("proof_mode") in SURFACE_PROOF_MODES
    if checkpoint_raw is not None or (
        surface_mode and state in {"review", "integrated", "done"}
    ):
        checkpoint = validate_evidence_object(
            errors,
            f"{task_id}.verification.tdd_checkpoint",
            checkpoint_raw,
            CHECKPOINT_FIELDS,
        )
        command = gate_registry.get(contract.get("gate"))
        expected = {
            "mode": contract.get("proof_mode"),
            "gate": contract.get("gate"),
            "executor": contract.get("executor"),
            "command_sha256": (
                string_digest(command)
                if isinstance(command, str)
                else None
            ),
            "plan_content_sha256": contract.get("plan_content_sha256"),
            "criterion_ids_sha256": canonical_digest(
                contract.get("criterion_ids")
            ),
            "proof_surface_sha256": canonical_digest(
                contract.get("proof_surface")
            ),
            "failure_fingerprints_sha256": canonical_digest(
                contract.get("failure_fingerprints")
            ),
            "base_commit": record.get("base_sha"),
            "base_exit_code": 0,
        }
        for field, expected_value in expected.items():
            if checkpoint and checkpoint.get(field) != expected_value:
                errors.append(
                    f"{task_id}.verification.tdd_checkpoint.{field} "
                    "does not match frozen contract"
                )
        for field in (
            "command_sha256",
            "plan_content_sha256",
            "criterion_ids_sha256",
            "proof_surface_sha256",
            "failure_fingerprints_sha256",
            "frozen_surface_sha256",
            "governance_context_sha256",
            "red_output_sha256",
        ):
            validate_digest(
                errors,
                checkpoint.get(field),
                f"{task_id}.verification.tdd_checkpoint.{field}",
            )
        red = checkpoint.get("red_commit")
        if not isinstance(red, str) or not SHA_PATTERN.fullmatch(red):
            errors.append(
                f"{task_id}.verification.tdd_checkpoint.red_commit must be "
                "a full lowercase SHA"
            )
        red_exit = checkpoint.get("red_exit_code")
        if contract.get("proof_mode") in FAILING_PROOF_MODES:
            if (
                type(red_exit) is not int
                or red_exit <= 0
                or red_exit in {126, 127}
                or red_exit >= 128
            ):
                errors.append(
                    f"{task_id}.verification.tdd_checkpoint.red_exit_code "
                    "must be a non-infrastructure failure"
                )
        elif red_exit != 0:
            errors.append(
                f"{task_id}.verification.tdd_checkpoint.red_exit_code "
                "must be zero for equivalence"
            )
        validate_timestamp(
            errors,
            checkpoint.get("checked_at"),
            f"{task_id}.verification.tdd_checkpoint.checked_at",
        )
        if (
            contract.get("proof_mode") == "regression"
            and isinstance(record.get("defect"), dict)
            and record["defect"].get("reproduction_commit") != red
        ):
            errors.append(
                f"{task_id}.defect.reproduction_commit does not match "
                "tdd_checkpoint.red_commit"
            )
        if (
            verify_git
            and state == "review"
            and isinstance(red, str)
            and SHA_PATTERN.fullmatch(red)
            and not commit_exists(root, red)
        ):
            errors.append(
                f"{task_id}.verification.tdd_checkpoint.red_commit is "
                f"unavailable: {red}"
            )

    if proof_raw is not None or (
        surface_mode and state in {"review", "integrated", "done"}
    ):
        proof = validate_evidence_object(
            errors,
            f"{task_id}.verification.tdd_proof",
            proof_raw,
            PROOF_FIELDS,
        )
        if isinstance(checkpoint_raw, dict):
            expected_checkpoint_digest = checkpoint_digest(checkpoint_raw)
            if proof.get("checkpoint_sha256") != expected_checkpoint_digest:
                errors.append(
                    f"{task_id}.verification.tdd_proof.checkpoint_sha256 "
                    "does not match tdd_checkpoint"
                )
        for field in (
            "checkpoint_sha256",
            "command_sha256",
            "plan_content_sha256",
            "criterion_ids_sha256",
            "proof_surface_sha256",
            "failure_fingerprints_sha256",
            "frozen_surface_sha256",
            "governance_context_sha256",
            "red_output_sha256",
            "replayed_red_output_sha256",
        ):
            validate_digest(
                errors,
                proof.get(field),
                f"{task_id}.verification.tdd_proof.{field}",
            )
        for field in CHECKPOINT_FIELDS - {"checked_at", "red_output_sha256"}:
            if (
                isinstance(checkpoint_raw, dict)
                and proof.get(field) != checkpoint_raw.get(field)
            ):
                errors.append(
                    f"{task_id}.verification.tdd_proof.{field} does not "
                    "match tdd_checkpoint"
                )
        if proof.get("head_exit_code") != 0:
            errors.append(
                f"{task_id}.verification.tdd_proof.head_exit_code must be zero"
            )
        if (
            state in {"review", "integrated", "done"}
            and proof.get("head_commit") != record.get("head_sha")
        ):
            errors.append(
                f"{task_id}.verification.tdd_proof.head_commit does not "
                "match head_sha"
            )
        head = proof.get("head_commit")
        if not isinstance(head, str) or not SHA_PATTERN.fullmatch(head):
            errors.append(
                f"{task_id}.verification.tdd_proof.head_commit must be "
                "a full lowercase SHA"
            )
        checkpoint_commit = proof.get("checkpoint_commit")
        if (
            not isinstance(checkpoint_commit, str)
            or not SHA_PATTERN.fullmatch(checkpoint_commit)
        ):
            errors.append(
                f"{task_id}.verification.tdd_proof.checkpoint_commit must "
                "be a full lowercase SHA"
            )
        validate_timestamp(
            errors,
            proof.get("checked_at"),
            f"{task_id}.verification.tdd_proof.checked_at",
        )
        if (
            verify_git
            and state == "review"
            and isinstance(checkpoint_raw, dict)
        ):
            base = checkpoint_raw.get("base_commit")
            red = checkpoint_raw.get("red_commit")
            if (
                isinstance(base, str)
                and isinstance(red, str)
                and isinstance(head, str)
                and commit_exists(root, base)
                and commit_exists(root, red)
                and commit_exists(root, head)
            ):
                if not is_ancestor(root, base, red):
                    errors.append(
                        f"{task_id}.verification.tdd_proof Red commit is "
                        "outside task base"
                    )
                if not is_ancestor(root, red, head):
                    errors.append(
                        f"{task_id}.verification.tdd_proof Red commit is "
                        "not an ancestor of head"
                    )
                try:
                    expected_checkpoint_commit = (
                        checkpoint_lifecycle_commit(
                            root,
                            task_id,
                            checkpoint_raw,
                            head,
                        )
                    )
                except TddProofError as error:
                    errors.append(f"{task_id}.verification.tdd_proof {error}")
                else:
                    if (
                        proof.get("checkpoint_commit")
                        != expected_checkpoint_commit
                    ):
                        errors.append(
                            f"{task_id}.verification.tdd_proof."
                            "checkpoint_commit does not match lifecycle"
                        )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT)
    subparsers = parser.add_subparsers(dest="command", required=True)
    checkpoint_parser = subparsers.add_parser("checkpoint")
    checkpoint_parser.add_argument("task_id")
    checkpoint_parser.add_argument("kind", choices=("red",))
    review_parser = subparsers.add_parser("review")
    review_parser.add_argument("task_id")
    review_parser.add_argument("head_commit")
    args = parser.parse_args()
    try:
        if args.command == "checkpoint":
            result = record_red_checkpoint(
                args.root.resolve(),
                args.task_id,
            )
        else:
            result = run_review_proof(
                args.root.resolve(),
                args.task_id,
                args.head_commit,
            )
    except TddProofError as error:
        print(f"TDD proof error:\n{error}", file=sys.stderr)
        return 1
    if result is not None:
        print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
