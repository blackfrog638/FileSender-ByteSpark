#!/usr/bin/env python3
"""Harness V2 worktree ownership and claim operations."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

import approval
import git_ops
from model import ContractSet, patterns_overlap
from state import StateError, StateStore


class WorkspaceError(RuntimeError):
    """Raised when a task cannot safely own or release a worktree."""


@dataclass(frozen=True)
class ClaimedWorkspace:
    task_id: str
    path: Path
    branch: str
    base_sha: str
    state_commit: str


class WorkspaceManager:
    def __init__(self, contracts: ContractSet, states: StateStore) -> None:
        self.contracts = contracts
        self.states = states
        self.root = contracts.root
        self.integration_branch = contracts.manifest["integration_branch"]

    def _integration_sha(self) -> str:
        protected_ref = "refs/heads/{}".format(self.integration_branch)
        try:
            if self.states.remote is not None:
                return git_ops.fetch_remote_object(
                    self.root, self.states.remote, protected_ref
                )
            return git_ops.object_id(self.root, protected_ref)
        except git_ops.GitError as error:
            raise WorkspaceError(
                "integration branch {} is unavailable".format(self.integration_branch)
            ) from error

    def _task_branch(self, task_id: str) -> str:
        return "work/{}".format(task_id)

    def _default_path(self, task_id: str) -> Path:
        common = git_ops.common_git_dir(self.root)
        primary = common.parent
        return primary.parent / "{}-{}".format(primary.name, task_id)

    def _check_dependencies(self, task_id: str) -> None:
        if self.states.remote is not None:
            try:
                approval.require_task_plan(self.contracts, task_id, self.states.remote)
            except approval.ApprovalError as error:
                raise WorkspaceError(str(error)) from error
        task = self.contracts.tasks[task_id]
        for dependency in task["depends_on"]:
            if dependency in self.contracts.legacy_accepted:
                continue
            snapshot = self.states.read(dependency)
            if snapshot.state != "done":
                raise WorkspaceError(
                    "{} dependency {} is {}".format(task_id, dependency, snapshot.state)
                )

    def _check_runtime_conflicts(self, task_id: str) -> None:
        task = self.contracts.tasks[task_id]
        for other_id, snapshot in self.states.list().items():
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
        snapshot = self.states.read(task_id)
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
            active = self.states.transition(
                task_id,
                "ready",
                "active",
                "claimed",
                details={
                    "base_sha": base,
                    "branch": branch,
                    "worktree": str(target),
                    "owner": self.states.actor["id"],
                },
            )
        except StateError as error:
            raise WorkspaceError(str(error)) from error
        try:
            git_ops.add_worktree(self.root, target, branch, base)
        except git_ops.GitError as error:
            try:
                self.states.transition(
                    task_id,
                    "active",
                    "ready",
                    "claim_rollback",
                    details={
                        "failed_state_commit": active.commit,
                        "error": str(error)[:240],
                    },
                )
            except StateError as rollback_error:
                raise WorkspaceError(
                    "claim failed and state rollback failed: {}; {}".format(
                        error, rollback_error
                    )
                ) from rollback_error
            raise WorkspaceError("claim failed: {}".format(error)) from error
        if active.commit is None:
            raise WorkspaceError("active state has no commit")
        return ClaimedWorkspace(task_id, target, branch, base, active.commit)

    def active_workspace(self, task_id: str) -> ClaimedWorkspace:
        snapshot = self.states.read(task_id)
        if snapshot.state != "active" or snapshot.event is None:
            raise WorkspaceError("{} is not active".format(task_id))
        details = snapshot.event["details"]
        required = {"base_sha", "branch", "worktree", "owner"}
        if not required.issubset(details):
            raise WorkspaceError("{} active event is incomplete".format(task_id))
        path = Path(details["worktree"])
        if not path.is_dir():
            raise WorkspaceError("{} worktree is missing".format(task_id))
        branch_ref = "refs/heads/{}".format(details["branch"])
        branch_sha = git_ops.ref_sha(self.root, branch_ref)
        if branch_sha is None:
            raise WorkspaceError("{} branch is missing".format(task_id))
        actual_branch = git_ops.git_text(path, "symbolic-ref", "--short", "HEAD")
        if actual_branch != details["branch"]:
            raise WorkspaceError("{} worktree uses the wrong branch".format(task_id))
        if snapshot.commit is None:
            raise WorkspaceError("{} state commit is missing".format(task_id))
        return ClaimedWorkspace(
            task_id,
            path,
            details["branch"],
            details["base_sha"],
            snapshot.commit,
        )

    def recover_claim(self, task_id: str) -> Optional[ClaimedWorkspace]:
        snapshot = self.states.read(task_id)
        if snapshot.state != "active" or snapshot.event is None:
            raise WorkspaceError("{} has no active claim to recover".format(task_id))
        details = snapshot.event["details"]
        required = {"base_sha", "branch", "worktree", "owner"}
        if not required.issubset(details):
            raise WorkspaceError("{} active event is incomplete".format(task_id))
        path = Path(details["worktree"])
        branch_ref = "refs/heads/{}".format(details["branch"])
        branch_sha = git_ops.ref_sha(self.root, branch_ref)
        if path.exists():
            return self.active_workspace(task_id)
        if branch_sha is not None:
            if branch_sha != details["base_sha"]:
                raise WorkspaceError(
                    "{} missing worktree has a branch with user commits".format(task_id)
                )
            git_ops.run_git(
                self.root,
                "update-ref",
                "-d",
                branch_ref,
                branch_sha,
            )
        rolled_back = self.states.transition(
            task_id,
            "active",
            "ready",
            "claim_rollback",
            details={
                "recovered_state_commit": snapshot.commit,
                "missing_worktree": str(path),
            },
        )
        if rolled_back.state != "ready":
            raise WorkspaceError("{} claim rollback did not complete".format(task_id))
        return None

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
            ".agents/project/approval.json",
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

    def release_queued_worktree(self, task_id: str) -> Path:
        snapshot = self.states.read(task_id)
        if snapshot.state != "queued" or snapshot.event is None:
            raise WorkspaceError(
                "{} worktree can be released only while queued".format(task_id)
            )
        history = self.states.history(task_id)
        active_events = [event for event in history if event["to"] == "active"]
        if not active_events:
            raise WorkspaceError("{} has no active worktree event".format(task_id))
        active_details = active_events[-1]["details"]
        path = Path(active_details["worktree"])
        branch_ref = "refs/heads/{}".format(active_details["branch"])
        source_head = snapshot.event["details"].get("source_head")
        if not isinstance(source_head, str) or len(source_head) != 40:
            raise WorkspaceError("{} queued source head is invalid".format(task_id))
        try:
            git_ops.remove_worktree(self.root, path)
            git_ops.delete_ref_cas(self.root, branch_ref, source_head)
        except git_ops.GitError as error:
            raise WorkspaceError(str(error)) from error
        return path

    def release_done_worktree(
        self,
        task_id: str,
        path: Path,
        branch: str,
        expected_head: str,
    ) -> Path:
        if self.states.read(task_id).state != "done":
            raise WorkspaceError(
                "{} worktree can be released only while done".format(task_id)
            )
        branch_ref = "refs/heads/{}".format(branch)
        try:
            git_ops.remove_worktree(self.root, path)
            git_ops.delete_ref_cas(self.root, branch_ref, expected_head)
        except git_ops.GitError as error:
            raise WorkspaceError(str(error)) from error
        return path
