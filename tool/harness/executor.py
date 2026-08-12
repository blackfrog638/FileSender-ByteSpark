#!/usr/bin/env python3
"""Resource-aware Harness V2 Gate execution with exact local evidence cache."""

from __future__ import annotations

import concurrent.futures
import datetime as dt
import hashlib
import os
import platform
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Tuple

import git_ops
from gates import GatePlan
from model import ContractSet, canonical_sha256, load_json


MAX_DIAGNOSTIC_BYTES = 64 * 1024


class GateExecutionError(RuntimeError):
    """Raised when a Gate plan cannot be executed or does not pass."""


@dataclass(frozen=True)
class GateResult:
    gate_id: str
    attestation: Mapping[str, Any]
    cached: bool
    diagnostic: str

    @property
    def outcome(self) -> str:
        return str(self.attestation["outcome"])


@dataclass(frozen=True)
class PlanResult:
    plan: GatePlan
    results: Tuple[GateResult, ...]

    @property
    def passed(self) -> bool:
        return all(result.outcome == "success" for result in self.results)

    def require_success(self) -> None:
        failed = [
            "{}={}".format(result.gate_id, result.outcome)
            for result in self.results
            if result.outcome != "success"
        ]
        if failed:
            raise GateExecutionError("Gate plan failed: {}".format(", ".join(failed)))


def _utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def _hash_file(path: Path) -> Tuple[str, int, str]:
    digest = hashlib.sha256()
    size = 0
    diagnostic = bytearray()
    with path.open("rb") as source:
        while True:
            chunk = source.read(64 * 1024)
            if not chunk:
                break
            digest.update(chunk)
            size += len(chunk)
            if len(diagnostic) < MAX_DIAGNOSTIC_BYTES:
                diagnostic.extend(
                    chunk[: MAX_DIAGNOSTIC_BYTES - len(diagnostic)]
                )
    return (
        digest.hexdigest(),
        size,
        diagnostic.decode("utf-8", errors="replace"),
    )


class GateExecutor:
    def __init__(
        self,
        contracts: ContractSet,
        execution_root: Optional[Path] = None,
        platform_label: str = "local",
        isolation_mode: str = "worktree",
        cache_enabled: bool = True,
    ) -> None:
        self.contracts = contracts
        self.execution_root = (execution_root or contracts.root).resolve()
        self.platform_label = platform_label
        self.isolation_mode = isolation_mode
        self.cache_enabled = cache_enabled
        self.cache_root = (
            git_ops.common_git_dir(contracts.root) / "xnn-harness" / "cache"
        )
        self.semaphores = {
            group_id: threading.BoundedSemaphore(group["max_parallel"])
            for group_id, group in contracts.gate_policy["resource_groups"].items()
        }

    def _source_context(self) -> Tuple[str, str]:
        if not git_ops.is_clean(self.execution_root):
            raise GateExecutionError("Gate execution requires a clean worktree")
        return (
            git_ops.object_id(self.execution_root, "HEAD"),
            git_ops.current_tree(self.execution_root),
        )

    def _environment(self, command: Mapping[str, Any]) -> Tuple[Dict[str, str], str]:
        environment = os.environ.copy()
        environment.update(command["environment"])
        attested = {
            name: environment.get(name, "")
            for name in sorted(
                set(command["environment"]) | {"PATH", "LANG", "LC_ALL"}
            )
        }
        return environment, canonical_sha256(attested)

    def _toolchain_digest(
        self, argv: List[str], environment: Mapping[str, str]
    ) -> str:
        executable = shutil.which(argv[0], path=environment.get("PATH"))
        executable_info: Dict[str, Any]
        if executable is None:
            executable_info = {"requested": argv[0], "resolved": None}
        else:
            stat = Path(executable).stat()
            executable_info = {
                "requested": argv[0],
                "resolved": str(Path(executable).resolve()),
                "size": stat.st_size,
                "mtime_ns": stat.st_mtime_ns,
            }
        return canonical_sha256(
            {
                "python": sys.version,
                "platform": platform.platform(),
                "machine": platform.machine(),
                "git": git_ops.git_text(self.execution_root, "--version"),
                "executable": executable_info,
            }
        )

    def _execution_context(
        self, gate_id: str, source_sha: str, source_tree: str
    ) -> Tuple[Dict[str, Any], Dict[str, str]]:
        gate = self.contracts.gates[gate_id]
        command = gate["command"]
        if command is None:
            raise GateExecutionError("{} is not a leaf Gate".format(gate_id))
        if (
            self.platform_label not in gate["platforms"]
            and "local" not in gate["platforms"]
        ):
            raise GateExecutionError(
                "{} does not support platform {}".format(
                    gate_id, self.platform_label
                )
            )
        argv = list(command["argv"])
        environment, environment_sha = self._environment(command)
        context = {
            "gate_id": gate_id,
            "source_sha": source_sha,
            "source_tree": source_tree,
            "command_sha256": canonical_sha256(command),
            "policy_sha256": canonical_sha256(self.contracts.gate_policy),
            "toolchain_sha256": self._toolchain_digest(argv, environment),
            "environment_sha256": environment_sha,
            "platform": self.platform_label,
            "isolation_mode": self.isolation_mode,
        }
        cache_context = {
            key: value for key, value in context.items() if key != "source_sha"
        }
        context["cache_key"] = canonical_sha256(cache_context)
        return context, environment

    def _cache_path(self, cache_key: str) -> Path:
        return self.cache_root / cache_key[:2] / "{}.json".format(cache_key)

    def _cached(
        self, context: Mapping[str, Any], phase: str
    ) -> Optional[Mapping[str, Any]]:
        if not self.cache_enabled:
            return None
        path = self._cache_path(str(context["cache_key"]))
        if not path.is_file():
            return None
        try:
            attestation = load_json(path)
        except Exception:
            return None
        if (
            attestation.get("cache_key") != context["cache_key"]
            or attestation.get("outcome") != "success"
            or attestation.get("skipped") is not False
        ):
            return None
        for field, expected in context.items():
            if field == "source_sha":
                continue
            if attestation.get(field) != expected:
                return None
        reused = dict(attestation)
        reused["reused_from_source_sha"] = attestation.get("source_sha")
        reused["source_sha"] = context["source_sha"]
        reused["executed_phase"] = phase
        return reused

    def _write_cache(
        self, gate: Mapping[str, Any], attestation: Mapping[str, Any]
    ) -> None:
        if (
            not self.cache_enabled
            or gate["cache"] != "success_only"
            or attestation["outcome"] != "success"
            or attestation["skipped"] is not False
        ):
            return
        git_ops.atomic_write_json(
            self._cache_path(str(attestation["cache_key"])),
            attestation,
        )

    def _terminate(self, process: subprocess.Popen) -> None:
        if process.poll() is not None:
            return
        if os.name != "nt":
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                return
        else:
            process.kill()

    def _execute_leaf(
        self,
        gate_id: str,
        phase: str,
        source_sha: str,
        source_tree: str,
    ) -> GateResult:
        gate = self.contracts.gates[gate_id]
        command = gate["command"]
        if command is None:
            raise GateExecutionError("{} is not executable".format(gate_id))
        context, environment = self._execution_context(
            gate_id, source_sha, source_tree
        )
        cached = self._cached(context, phase)
        if cached is not None:
            return GateResult(gate_id, cached, True, "")
        group = gate["resource_group"]
        semaphore = self.semaphores[group]
        started_at = _utc_now()
        started_ns = time.monotonic_ns()
        outcome = "error"
        exit_code: Optional[int] = None
        output_sha = hashlib.sha256(b"").hexdigest()
        output_bytes = 0
        diagnostic = ""
        with semaphore:
            with tempfile.NamedTemporaryFile(
                prefix="xnn-gate-{}-".format(gate_id), delete=False
            ) as output:
                output_path = Path(output.name)
            try:
                with output_path.open("wb") as output_stream:
                    try:
                        process = subprocess.Popen(
                            command["argv"],
                            cwd=str(self.execution_root),
                            env=environment,
                            stdin=subprocess.DEVNULL,
                            stdout=output_stream,
                            stderr=subprocess.STDOUT,
                            start_new_session=(os.name != "nt"),
                        )
                    except OSError as error:
                        diagnostic = str(error)
                        process = None
                    if process is not None:
                        try:
                            exit_code = process.wait(
                                timeout=command["timeout_seconds"]
                            )
                            outcome = "success" if exit_code == 0 else "failure"
                        except subprocess.TimeoutExpired:
                            self._terminate(process)
                            process.wait()
                            outcome = "timeout"
                output_sha, output_bytes, captured = _hash_file(output_path)
                if captured:
                    diagnostic = captured
            finally:
                output_path.unlink(missing_ok=True)
        duration_ns = time.monotonic_ns() - started_ns
        attestation: Dict[str, Any] = {
            "schema_version": 1,
            **context,
            "executed_phase": phase,
            "started_at": started_at,
            "duration_ns": duration_ns,
            "outcome": outcome,
            "skipped": False,
            "exit_code": exit_code,
            "output_sha256": output_sha,
            "output_bytes": output_bytes,
            "reused_from_source_sha": None,
        }
        self._write_cache(gate, attestation)
        return GateResult(gate_id, attestation, False, diagnostic)

    def execute(self, plan: GatePlan) -> PlanResult:
        source_sha, source_tree = self._source_context()
        if not plan.leaves:
            raise GateExecutionError("Gate plan has no executable leaves")
        workers = min(32, max(1, len(plan.leaves)))
        futures: Dict[str, concurrent.futures.Future] = {}
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            for gate_id in plan.leaves:
                futures[gate_id] = pool.submit(
                    self._execute_leaf,
                    gate_id,
                    plan.phase,
                    source_sha,
                    source_tree,
                )
            results: List[GateResult] = []
            try:
                for gate_id in plan.leaves:
                    results.append(futures[gate_id].result())
            except Exception:
                for future in futures.values():
                    future.cancel()
                raise
        return PlanResult(plan, tuple(results))
