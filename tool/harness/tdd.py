#!/usr/bin/env python3
"""Harness V2 deterministic TDD chronology checks."""

from __future__ import annotations

import fnmatch
import shutil
import tempfile
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple

import git_ops
from executor import GateExecutor, GateResult
from gates import single_gate_plan
from model import ContractSet, canonical_sha256
from workspace import WorkspaceManager


class TddError(RuntimeError):
    """Raised when Red/Green chronology or evidence is invalid."""


RED_MODES = {"red_green", "regression", "mutation", "adversarial"}


def _matches_any(path: str, patterns: Sequence[str]) -> bool:
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def _tree_paths(root: Path, commit: str) -> List[str]:
    output = git_ops.git_text(root, "ls-tree", "-r", "--name-only", commit)
    return [line for line in output.splitlines() if line]


def surface_manifest(
    root: Path, commit: str, patterns: Sequence[str]
) -> Dict[str, str]:
    result: Dict[str, str] = {}
    for path in _tree_paths(root, commit):
        if _matches_any(path, patterns):
            result[path] = git_ops.object_id(root, "{}:{}".format(commit, path))
    return result


def surface_sha256(
    root: Path, commit: str, proof_paths: Sequence[str], oracle_paths: Sequence[str]
) -> str:
    return canonical_sha256(
        {
            "proof": surface_manifest(root, commit, proof_paths),
            "oracle": surface_manifest(root, commit, oracle_paths),
        }
    )


def _gate_payload(result: GateResult) -> Dict[str, Any]:
    return dict(result.attestation)


def _exact_fingerprint(diagnostic: str, fingerprints: Sequence[str]) -> Optional[str]:
    lines = set(diagnostic.splitlines())
    for fingerprint in fingerprints:
        if fingerprint in lines:
            return fingerprint
    return None


class TddManager:
    def __init__(
        self,
        contracts: ContractSet,
        workspaces: WorkspaceManager,
    ) -> None:
        self.contracts = contracts
        self.workspaces = workspaces
        self.root = contracts.root

    def _run_at_commit(
        self,
        task_id: str,
        gate_id: str,
        phase: str,
        commit: str,
    ) -> GateResult:
        parent = Path(tempfile.mkdtemp(prefix="xnn-tdd-"))
        worktree = parent / "worktree"
        try:
            git_ops.run_git(
                self.root,
                "worktree",
                "add",
                "--detach",
                str(worktree),
                commit,
            )
            executor = GateExecutor(
                self.contracts,
                execution_root=worktree,
                cache_enabled=True,
            )
            plan = single_gate_plan(self.contracts, task_id, phase, gate_id)
            result = executor.execute(plan)
            if len(result.results) != 1:
                raise TddError("focused TDD Gate must resolve to one leaf")
            return result.results[0]
        finally:
            if worktree.exists():
                git_ops.run_git(
                    self.root,
                    "worktree",
                    "remove",
                    "--force",
                    str(worktree),
                    check=False,
                )
            shutil.rmtree(parent, ignore_errors=True)

    def _contract_blobs(self, task_id: str) -> Tuple[str, str]:
        task = self.contracts.tasks[task_id]
        return (
            git_ops.object_id(self.root, "HEAD:.agents/tasks/{}.json".format(task_id)),
            git_ops.object_id(
                self.root,
                "HEAD:.agents/plans/{}.json".format(task["plan"]),
            ),
        )

    def record_red(self, task_id: str) -> Mapping[str, Any]:
        task = self.contracts.tasks[task_id]
        tdd = task["tdd"]
        if tdd["mode"] not in RED_MODES:
            raise TddError("{} does not use a Red proof mode".format(task_id))
        active = self.workspaces.active_workspace(task_id)
        if not git_ops.is_clean(active.path):
            raise TddError("{} worktree must be clean for Red".format(task_id))
        red_sha = git_ops.object_id(active.path, "HEAD")
        if red_sha == active.base_sha:
            raise TddError("{} Red commit must differ from base".format(task_id))
        if not git_ops.is_ancestor(active.path, active.base_sha, red_sha):
            raise TddError("{} Red commit is outside the task base".format(task_id))
        return self.load_red(task_id, red_sha)

    def load_red(self, task_id: str, red_sha: str) -> Mapping[str, Any]:
        task = self.contracts.tasks[task_id]
        tdd = task["tdd"]
        if tdd["mode"] not in RED_MODES:
            raise TddError("{} does not use a Red proof mode".format(task_id))
        active = self.workspaces.active_workspace(task_id)
        if not git_ops.is_ancestor(active.path, active.base_sha, red_sha):
            raise TddError("{} Red commit is outside the task base".format(task_id))
        proof_paths = list(tdd["proof_paths"])
        for commit in git_ops.commit_range(active.path, active.base_sha, red_sha):
            for path in git_ops.commit_changed_paths(active.path, commit):
                if not _matches_any(path, proof_paths):
                    raise TddError(
                        "{} production or undeclared path appears before Red: {}".format(
                            task_id, path
                        )
                    )
        gate_id = tdd["gate"]
        base_result = self._run_at_commit(task_id, gate_id, "tdd_base", active.base_sha)
        if base_result.outcome != "success":
            raise TddError(
                "{} focused Gate does not pass at base: {}".format(
                    task_id, base_result.outcome
                )
            )
        red_result = self._run_at_commit(task_id, gate_id, "tdd_red", red_sha)
        if (
            red_result.outcome != "failure"
            or red_result.attestation["skipped"] is not False
        ):
            raise TddError(
                "{} Red must be an attributed assertion failure, got {}".format(
                    task_id, red_result.outcome
                )
            )
        fingerprint = _exact_fingerprint(
            red_result.diagnostic, tdd["failure_fingerprints"]
        )
        if fingerprint is None:
            raise TddError("{} Red output has no exact fingerprint".format(task_id))
        task_blob, plan_blob = self._contract_blobs(task_id)
        attestation: Dict[str, Any] = {
            "schema_version": 1,
            "task_id": task_id,
            "mode": tdd["mode"],
            "base_sha": active.base_sha,
            "red_sha": red_sha,
            "gate_id": gate_id,
            "task_spec_blob": task_blob,
            "plan_blob": plan_blob,
            "gate_policy_sha256": canonical_sha256(self.contracts.gate_policy),
            "proof_paths": proof_paths,
            "oracle_paths": list(tdd["oracle_paths"]),
            "frozen_surface_sha256": surface_sha256(
                active.path,
                red_sha,
                proof_paths,
                tdd["oracle_paths"],
            ),
            "base_gate": _gate_payload(base_result),
            "red_gate": _gate_payload(red_result),
            "failure_fingerprint": fingerprint,
        }
        return attestation

    def review_green(self, task_id: str, red_sha: str) -> Mapping[str, Any]:
        active = self.workspaces.active_workspace(task_id)
        if not git_ops.is_clean(active.path):
            raise TddError("{} worktree must be clean for Green".format(task_id))
        head = git_ops.object_id(active.path, "HEAD")
        if not git_ops.is_ancestor(active.path, red_sha, head) or red_sha == head:
            raise TddError("{} Green head must descend from Red".format(task_id))
        red = self.load_red(task_id, red_sha)
        task_blob, plan_blob = self._contract_blobs(task_id)
        if (
            red["task_spec_blob"] != task_blob
            or red["plan_blob"] != plan_blob
            or red["gate_policy_sha256"] != canonical_sha256(self.contracts.gate_policy)
        ):
            raise TddError("{} Red governance context is stale".format(task_id))
        frozen_patterns = list(red["proof_paths"]) + list(red["oracle_paths"])
        for commit in git_ops.commit_range(active.path, red_sha, head):
            for path in git_ops.commit_changed_paths(active.path, commit):
                if _matches_any(path, frozen_patterns):
                    raise TddError(
                        "{} frozen test/oracle changed after Red: {}".format(
                            task_id, path
                        )
                    )
        current_surface = surface_sha256(
            active.path,
            head,
            red["proof_paths"],
            red["oracle_paths"],
        )
        if current_surface != red["frozen_surface_sha256"]:
            raise TddError("{} frozen proof surface no longer matches".format(task_id))
        executor = GateExecutor(
            self.contracts,
            execution_root=active.path,
            cache_enabled=True,
        )
        plan = single_gate_plan(self.contracts, task_id, "tdd_green", red["gate_id"])
        execution = executor.execute(plan)
        if len(execution.results) != 1:
            raise TddError("focused TDD Gate must resolve to one leaf")
        green = execution.results[0]
        if green.outcome != "success":
            raise TddError(
                "{} Green Gate did not pass: {}".format(task_id, green.outcome)
            )
        return {
            "schema_version": 1,
            "task_id": task_id,
            "red_attestation_sha256": canonical_sha256(red),
            "red_sha": red_sha,
            "green_sha": head,
            "gate_id": red["gate_id"],
            "green_gate": _gate_payload(green),
            "proof_surface_sha256": current_surface,
        }
