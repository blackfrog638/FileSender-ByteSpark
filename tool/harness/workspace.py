#!/usr/bin/env python3
"""Harness V2 worktree ownership over derived runtime state."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

import git_ops
from model import ContractSet, patterns_overlap
from runtime import RuntimeView, RuntimeViewError


class WorkspaceError(RuntimeError):
    """Raised when a task cannot safely own or release a worktree."""


@dataclass(frozen=True)
class ClaimedWorkspace:
    task_id: str
    path: Path
    branch: str
    base_sha: str


class WorkspaceManager:
    def __init__(self, contracts: ContractSet, runtime: RuntimeView) -> None:
        self.contracts = contracts
        self.runtime = runtime
        self.root = contracts.root
        self.integration_branch = contracts.manifest["integration_branch"]

    def _integration_sha(self) -> str:
        try:
            return self.runtime.integration_sha()
        except RuntimeViewError as error:
            raise WorkspaceError(str(error)) from error

    def _task_branch(self, task_id: str) -> str:
        return "work/{}".format(task_id)

    def _default_path(self, task_id: str) -> Path:
        common = git_ops.common_git_dir(self.root)
        primary = common.parent
        return primary.parent / "{}-{}".format(primary.name, task_id)

    def _check_dependencies(self, task_id: str) -> None:
        task = self.contracts.tasks[task_id]
        snapshots = self.runtime.list()
        for dependency in task["depends_on"]:
            snapshot = snapshots[dependency]
            if snapshot.state != "done":
                raise WorkspaceError(
                    "{} dependency {} is {}".format(task_id, dependency, snapshot.state)
                )

    def _check_runtime_conflicts(self, task_id: str) -> None:
        task = self.contracts.tasks[task_id]
        for other_id, snapshot in self.runtime.list().items():
            if other_id == task_id or snapshot.state not in {"active", "queued"}:
                continue
            other = self.contracts.tasks[other_id]
            for owned in task["owned_paths"]:
                for reserved in other["owned_paths"]:
                    if patterns_overlap(owned, reserved):
                        raise WorkspaceError(
                            "{} conflicts with {}: {} <-> {}".format(
                                task_id, other_id, owned, reserved
                            )
                        )

    def claim(self, task_id: str, path: Optional[Path] = None) -> ClaimedWorkspace:
        if task_id not in self.contracts.tasks:
            raise WorkspaceError("unknown task {}".format(task_id))
        snapshot = self.runtime.read(task_id)
        if snapshot.state != "ready":
            raise WorkspaceError(
                "{} cannot be claimed from {}".format(task_id, snapshot.state)
            )
        self._check_dependencies(task_id)
        self._check_runtime_conflicts(task_id)
        target = (path or self._default_path(task_id)).resolve()
        branch = self._task_branch(task_id)
        if target.exists():
            raise WorkspaceError("worktree path already exists: {}".format(target))
        if git_ops.ref_sha(self.root, "refs/heads/{}".format(branch)) is not None:
            raise WorkspaceError("task branch already exists: {}".format(branch))
        base = self._integration_sha()
        try:
            git_ops.add_worktree(self.root, target, branch, base)
        except git_ops.GitError as error:
            raise WorkspaceError("claim failed: {}".format(error)) from error
        return ClaimedWorkspace(task_id, target, branch, base)

    def active_workspace(self, task_id: str) -> ClaimedWorkspace:
        snapshot = self.runtime.read(task_id)
        if snapshot.state != "active" or snapshot.worktree is None:
            raise WorkspaceError("{} is not active".format(task_id))
        active = snapshot.worktree
        path = active.path
        if not path.is_dir():
            raise WorkspaceError("{} worktree is missing".format(task_id))
        branch_ref = "refs/heads/{}".format(active.branch)
        branch_sha = git_ops.ref_sha(self.root, branch_ref)
        if branch_sha != active.head:
            raise WorkspaceError("{} branch is missing".format(task_id))
        actual_branch = git_ops.git_text(path, "symbolic-ref", "--short", "HEAD")
        if actual_branch != active.branch:
            raise WorkspaceError("{} worktree uses the wrong branch".format(task_id))
        integration = self._integration_sha()
        merge_base = git_ops.git_text(
            path,
            "merge-base",
            active.head,
            integration,
        )
        return ClaimedWorkspace(
            task_id,
            path,
            active.branch,
            merge_base,
        )

    def recover_claim(self, task_id: str) -> Optional[ClaimedWorkspace]:
        if task_id not in self.contracts.tasks:
            raise WorkspaceError("unknown task {}".format(task_id))
        snapshot = self.runtime.read(task_id)
        if snapshot.state == "active":
            return self.active_workspace(task_id)
        if snapshot.state != "ready":
            raise WorkspaceError(
                "{} cannot recover from {}".format(task_id, snapshot.state)
            )
        branch = self._task_branch(task_id)
        branch_ref = "refs/heads/{}".format(branch)
        branch_sha = git_ops.ref_sha(self.root, branch_ref)
        if branch_sha is None:
            raise WorkspaceError("{} has no interrupted claim".format(task_id))
        path = self._default_path(task_id).resolve()
        if path.exists():
            raise WorkspaceError("worktree path already exists: {}".format(path))
        integration = self._integration_sha()
        if branch_sha == integration:
            try:
                git_ops.delete_ref_cas(self.root, branch_ref, branch_sha)
            except git_ops.GitError as error:
                raise WorkspaceError(str(error)) from error
            return None
        result = git_ops.run_git(
            self.root,
            "worktree",
            "add",
            str(path),
            branch,
            check=False,
        )
        if result.returncode != 0:
            raise WorkspaceError("{} worktree cannot be recovered".format(task_id))
        return self.active_workspace(task_id)

    def stale_reasons(self, task_id: str) -> List[str]:
        workspace = self.active_workspace(task_id)
        current = self._integration_sha()
        if not git_ops.is_ancestor(self.root, workspace.base_sha, current):
            return ["task base is not an ancestor of the integration branch"]
        if current == workspace.base_sha:
            return []
        task = self.contracts.tasks[task_id]
        changed = git_ops.changed_paths(self.root, workspace.base_sha, current)
        reasons: List[str] = []
        for path in changed:
            if any(patterns_overlap(path, owned) for owned in task["owned_paths"]):
                reasons.append("integration changed owned path {}".format(path))
        governed_paths = {
            ".agents/manifest.json",
            ".agents/gates.json",
            ".agents/risk-routing.json",
            ".agents/plans/{}.json".format(task["plan"]),
            ".agents/tasks/{}.json".format(task_id),
        }
        for path in changed:
            if path in governed_paths or path.startswith(".agents/project/"):
                reasons.append("integration changed governance input {}".format(path))
        return sorted(set(reasons))

    def require_fresh(self, task_id: str) -> None:
        reasons = self.stale_reasons(task_id)
        if reasons:
            raise WorkspaceError("{} is stale:\n{}".format(task_id, "\n".join(reasons)))

    def release(self, workspace: ClaimedWorkspace, expected_head: str) -> Path:
        branch_ref = "refs/heads/{}".format(workspace.branch)
        try:
            git_ops.remove_worktree(self.root, workspace.path)
            git_ops.delete_ref_cas(self.root, branch_ref, expected_head)
        except git_ops.GitError as error:
            raise WorkspaceError(str(error)) from error
        return workspace.path
