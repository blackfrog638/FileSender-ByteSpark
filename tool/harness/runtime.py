#!/usr/bin/env python3
"""Derive Harness V2 task state from existing Git facts."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Mapping, Optional, Tuple

import git_ops
from model import ContractSet


QUEUE_TASK = re.compile(r".*/[0-9]{3}-(XT-[0-9]{3,})$")


class RuntimeViewError(RuntimeError):
    """Raised when runtime Git facts are ambiguous or unavailable."""


@dataclass(frozen=True)
class Worktree:
    task_id: str
    path: Path
    branch: str
    head: str


@dataclass(frozen=True)
class TaskSnapshot:
    task_id: str
    state: str
    delivery_sha: Optional[str] = None
    worktree: Optional[Worktree] = None
    queue_refs: Tuple[str, ...] = ()


class RuntimeView:
    """Read-only projection over the protected history and transient refs."""

    def __init__(
        self,
        contracts: ContractSet,
        remote: Optional[str] = None,
    ) -> None:
        self.contracts = contracts
        self.root = contracts.root
        self.remote = remote
        self.integration_branch = contracts.manifest["integration_branch"]
        self.queue_prefix = contracts.manifest["queue_namespace"]

    def integration_sha(self) -> str:
        ref = "refs/heads/{}".format(self.integration_branch)
        try:
            if self.remote is not None:
                return git_ops.fetch_remote_object(self.root, self.remote, ref)
            return git_ops.object_id(self.root, ref)
        except git_ops.GitError as error:
            raise RuntimeViewError(
                "integration branch {} is unavailable".format(self.integration_branch)
            ) from error

    def deliveries(self) -> Mapping[str, str]:
        head = self.integration_sha()
        result: Dict[str, str] = {}
        for commit in git_ops.first_parent_history(self.root, head):
            message = git_ops.git_text(
                self.root,
                "show",
                "-s",
                "--format=%B",
                commit,
            )
            tasks = [
                line.removeprefix("Xnn-Task: ")
                for line in message.splitlines()
                if line.startswith("Xnn-Task: ")
            ]
            lifecycle = [
                line.removeprefix("Xnn-Lifecycle: ")
                for line in message.splitlines()
                if line.startswith("Xnn-Lifecycle: ")
            ]
            if lifecycle != ["delivery"]:
                continue
            if len(tasks) != 1 or tasks[0] not in self.contracts.tasks:
                continue
            if tasks[0] in result:
                raise RuntimeViewError(
                    "{} has multiple accepted deliveries".format(tasks[0])
                )
            result[tasks[0]] = commit
        return result

    def worktrees(self) -> Mapping[str, Worktree]:
        output = git_ops.git_text(self.root, "worktree", "list", "--porcelain")
        records = output.split("\n\n") if output else []
        result: Dict[str, Worktree] = {}
        for record in records:
            fields: Dict[str, str] = {}
            for line in record.splitlines():
                key, _, value = line.partition(" ")
                if key in {"worktree", "HEAD", "branch"}:
                    fields[key] = value
            branch_ref = fields.get("branch", "")
            prefix = "refs/heads/work/"
            if not branch_ref.startswith(prefix):
                continue
            task_id = branch_ref[len(prefix) :]
            if task_id not in self.contracts.tasks:
                continue
            if task_id in result:
                raise RuntimeViewError(
                    "{} has multiple attached worktrees".format(task_id)
                )
            result[task_id] = Worktree(
                task_id=task_id,
                path=Path(fields["worktree"]).resolve(),
                branch="work/{}".format(task_id),
                head=fields["HEAD"],
            )
        return result

    def queue_refs(self) -> Mapping[str, Tuple[str, ...]]:
        try:
            if self.remote is None:
                refs = git_ops.list_refs(self.root, self.queue_prefix)
            else:
                refs = git_ops.list_remote_refs(
                    self.root,
                    self.remote,
                    self.queue_prefix,
                )
        except git_ops.GitError as error:
            raise RuntimeViewError(str(error)) from error
        result: Dict[str, list[str]] = {}
        for ref in sorted(refs):
            match = QUEUE_TASK.fullmatch(ref)
            if match is None:
                continue
            task_id = match.group(1)
            if task_id not in self.contracts.tasks:
                raise RuntimeViewError(
                    "queue ref references unknown task {}".format(task_id)
                )
            result.setdefault(task_id, []).append(ref)
        duplicates = sorted(
            task_id for task_id, task_refs in result.items() if len(task_refs) > 1
        )
        if duplicates:
            raise RuntimeViewError(
                "tasks have multiple queue candidates: {}".format(", ".join(duplicates))
            )
        return {task_id: tuple(task_refs) for task_id, task_refs in result.items()}

    def list(self) -> Mapping[str, TaskSnapshot]:
        deliveries = self.deliveries()
        worktrees = self.worktrees()
        queues = self.queue_refs()
        snapshots: Dict[str, TaskSnapshot] = {}
        for task_id in sorted(self.contracts.tasks):
            if task_id in deliveries:
                snapshots[task_id] = TaskSnapshot(
                    task_id,
                    "done",
                    delivery_sha=deliveries[task_id],
                )
            elif task_id in queues:
                snapshots[task_id] = TaskSnapshot(
                    task_id,
                    "queued",
                    queue_refs=queues[task_id],
                )
            elif task_id in worktrees:
                snapshots[task_id] = TaskSnapshot(
                    task_id,
                    "active",
                    worktree=worktrees[task_id],
                )
            else:
                snapshots[task_id] = TaskSnapshot(task_id, "ready")
        return snapshots

    def read(self, task_id: str) -> TaskSnapshot:
        if task_id not in self.contracts.tasks:
            raise RuntimeViewError("unknown task {}".format(task_id))
        return self.list()[task_id]
