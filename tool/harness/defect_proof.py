#!/usr/bin/env python3

"""Execute deterministic defect evidence at reproduction and reviewed head."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

from trusted_gates import GateRegistryError, load_gate_registry


ROOT = Path(__file__).resolve().parents[2]
SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")


class DefectProofError(RuntimeError):
    pass


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


def load_record(root: Path, task_id: str) -> dict[str, Any]:
    path = root / ".agents" / "records" / f"{task_id}.json"
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise DefectProofError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise DefectProofError(f"{path} must contain an object")
    return value


def write_record(root: Path, task_id: str, record: dict[str, Any]) -> None:
    path = root / ".agents" / "records" / f"{task_id}.json"
    path.write_text(
        json.dumps(record, indent=2, ensure_ascii=True) + "\n",
        encoding="utf-8",
    )


def proof_environment(root: Path) -> dict[str, str]:
    environment = os.environ.copy()
    environment["XNN_TRANSFER_VCPKG_ROOT"] = str(
        (root / "out" / "tools" / "vcpkg").resolve()
    )
    return environment


def command_digest(command: str) -> str:
    return hashlib.sha256(command.encode("utf-8")).hexdigest()


def result_output(result: subprocess.CompletedProcess[str]) -> str:
    return result.stdout + result.stderr


def output_digest(result: subprocess.CompletedProcess[str]) -> str:
    return hashlib.sha256(result_output(result).encode("utf-8")).hexdigest()


def result_tail(result: subprocess.CompletedProcess[str]) -> str:
    output = result_output(result)
    return output[-4000:] if output else "(no output)"


def remove_worktree(root: Path, worktree: Path) -> None:
    result = git(
        root,
        "worktree",
        "remove",
        "--force",
        str(worktree),
        check=False,
    )
    if result.returncode != 0:
        raise DefectProofError(
            "cannot remove reproduction worktree:\n"
            + (result.stderr or result.stdout)
        )


def run_detached_gate(
    root: Path,
    commit: str,
    command: str,
    worktree: Path,
) -> subprocess.CompletedProcess[str]:
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
        raise DefectProofError(
            f"cannot create detached worktree for {commit}:\n"
            + (added.stderr or added.stdout)
        )
    try:
        return subprocess.run(
            ["bash", "-lc", command],
            cwd=worktree,
            env=proof_environment(root),
            check=False,
            capture_output=True,
            text=True,
        )
    finally:
        remove_worktree(root, worktree)


def run_proof(
    root: Path,
    task_id: str,
    head_commit: str,
) -> dict[str, Any] | None:
    record = load_record(root, task_id)
    if record.get("schema_version") != 3 or record.get("task_type") != "bugfix":
        return None
    if record.get("state") != "in_progress":
        raise DefectProofError(
            f"{task_id} must be in_progress for deterministic proof"
        )
    if not SHA_PATTERN.fullmatch(head_commit):
        raise DefectProofError("reviewed head must be a full lowercase SHA")
    current_head = git_text(root, "rev-parse", "HEAD")
    if current_head != head_commit:
        raise DefectProofError(
            f"reviewed head {head_commit} does not match worktree {current_head}"
        )

    defect = record.get("defect")
    if not isinstance(defect, dict):
        raise DefectProofError(f"{task_id}.defect must be an object")
    mode = defect.get("proof_mode")
    if mode != "deterministic":
        raise DefectProofError(
            f"proof mode {mode!r} is not supported by deterministic proof"
        )
    reproduction = defect.get("reproduction_commit")
    base = record.get("base_sha")
    if not isinstance(reproduction, str) or not SHA_PATTERN.fullmatch(
        reproduction
    ):
        raise DefectProofError("reproduction commit must be a full lowercase SHA")
    if not isinstance(base, str) or not SHA_PATTERN.fullmatch(base):
        raise DefectProofError("task base must be a full lowercase SHA")
    for label, commit in (("base", base), ("reproduction", reproduction)):
        if git(root, "cat-file", "-e", f"{commit}^{{commit}}", check=False).returncode:
            raise DefectProofError(f"{label} commit is unavailable: {commit}")
    if not is_ancestor(root, base, reproduction):
        raise DefectProofError("reproduction commit is outside the task base")
    if base == reproduction:
        raise DefectProofError("task base and reproduction commit must differ")
    if not is_ancestor(root, reproduction, head_commit):
        raise DefectProofError("reproduction commit is not an ancestor of head")
    if reproduction == head_commit:
        raise DefectProofError("reproduction commit and head must differ")

    gate = defect.get("regression_gate")
    if not isinstance(gate, str):
        raise DefectProofError("regression gate must be a string")
    try:
        registry = load_gate_registry(root / ".agents" / "manifest.yaml")
    except GateRegistryError as error:
        raise DefectProofError(str(error)) from error
    command = registry.get(gate)
    if command is None:
        raise DefectProofError(f"regression gate is not registered: {gate}")
    verification = record.get("verification")
    if not isinstance(verification, dict) or gate not in verification.get(
        "gates", []
    ):
        raise DefectProofError(
            "regression gate must appear in verification.gates"
        )
    if git_text(root, "status", "--porcelain"):
        raise DefectProofError("task worktree must be clean before defect proof")

    failure_fingerprint = defect.get("failure_fingerprint")
    if (
        not isinstance(failure_fingerprint, str)
        or not failure_fingerprint.strip()
        or failure_fingerprint != failure_fingerprint.strip()
        or "TODO" in failure_fingerprint.upper()
        or "\n" in failure_fingerprint
        or "\r" in failure_fingerprint
        or not 16 <= len(failure_fingerprint) <= 256
    ):
        raise DefectProofError(
            "failure fingerprint must be 16-256 concrete single-line characters"
        )

    with tempfile.TemporaryDirectory(prefix=f"xnn-{task_id.lower()}-proof-") as tmp:
        base_result = run_detached_gate(
            root,
            base,
            command,
            Path(tmp) / "base",
        )
        if base_result.returncode != 0:
            raise DefectProofError(
                f"regression gate failed at task base with exit "
                f"{base_result.returncode}:\n{result_tail(base_result)}"
            )
        reproduction_result = run_detached_gate(
            root,
            reproduction,
            command,
            Path(tmp) / "reproduction",
        )

    if reproduction_result.returncode == 0:
        raise DefectProofError(
            "regression gate unexpectedly passed at reproduction:\n"
            + result_tail(reproduction_result)
        )
    if reproduction_result.returncode in {126, 127}:
        raise DefectProofError(
            f"regression gate could not execute at reproduction with exit "
            f"{reproduction_result.returncode}:\n"
            f"{result_tail(reproduction_result)}"
        )
    if failure_fingerprint not in result_output(reproduction_result).splitlines():
        raise DefectProofError(
            "regression gate failure did not contain the declared "
            f"fingerprint as a complete line {failure_fingerprint!r}:\n"
            f"{result_tail(reproduction_result)}"
        )

    head_result = subprocess.run(
        ["bash", "-lc", command],
        cwd=root,
        env=proof_environment(root),
        check=False,
        capture_output=True,
        text=True,
    )
    if head_result.returncode != 0:
        raise DefectProofError(
            f"regression gate failed at reviewed head with exit "
            f"{head_result.returncode}:\n{result_tail(head_result)}"
        )
    if git_text(root, "status", "--porcelain"):
        raise DefectProofError("regression gate changed tracked task content")

    proof = {
        "mode": "deterministic",
        "gate": gate,
        "command_sha256": command_digest(command),
        "failure_fingerprint_sha256": command_digest(failure_fingerprint),
        "base_commit": base,
        "reproduction_commit": reproduction,
        "head_commit": head_commit,
        "base_exit_code": base_result.returncode,
        "reproduction_exit_code": reproduction_result.returncode,
        "head_exit_code": head_result.returncode,
        "reproduction_output_sha256": output_digest(reproduction_result),
        "checked_at": dt.datetime.now(dt.timezone.utc).isoformat(
            timespec="seconds"
        ),
    }
    verification["defect_proof"] = proof
    write_record(root, task_id, record)
    return proof


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("task_id")
    parser.add_argument("head_commit")
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    try:
        proof = run_proof(
            args.root.resolve(),
            args.task_id,
            args.head_commit,
        )
    except DefectProofError as error:
        print(f"Defect proof error:\n{error}", file=sys.stderr)
        return 1
    if proof is not None:
        print(json.dumps(proof, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
