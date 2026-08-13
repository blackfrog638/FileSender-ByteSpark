#!/usr/bin/env python3
"""Conservative cleanup of transient Harness V2 refs."""

from __future__ import annotations

from dataclasses import asdict, dataclass
from typing import Any, Dict, List, Optional

import git_ops
from model import ContractSet
from runtime import RuntimeView


class CleanupError(RuntimeError):
    """Raised when a transient ref cannot be inspected or deleted safely."""


@dataclass(frozen=True)
class CleanupCandidate:
    kind: str
    ref: str
    sha: str
    reason: str


class BranchCleanup:
    def __init__(
        self,
        contracts: ContractSet,
        runtime: RuntimeView,
        remote: Optional[str],
    ) -> None:
        self.contracts = contracts
        self.runtime = runtime
        self.remote = remote
        self.root = contracts.root
        self.queue_prefix = contracts.manifest["queue_namespace"]

    def _work_candidates(self, protected: str) -> List[CleanupCandidate]:
        attached = {
            "refs/heads/{}".format(worktree.branch)
            for worktree in self.runtime.worktrees().values()
        }
        result: List[CleanupCandidate] = []
        for ref, sha in git_ops.list_refs(self.root, "refs/heads/work/").items():
            if ref in attached:
                continue
            if git_ops.is_ancestor(self.root, sha, protected):
                result.append(
                    CleanupCandidate(
                        "local_work",
                        ref,
                        sha,
                        "unattached branch is already in protected history",
                    )
                )
        return result

    def _queue_candidates(self, protected: str) -> List[CleanupCandidate]:
        if self.remote is None:
            refs = git_ops.list_refs(self.root, self.queue_prefix)
        else:
            refs = git_ops.list_remote_refs(
                self.root,
                self.remote,
                self.queue_prefix,
            )
        result: List[CleanupCandidate] = []
        for ref, sha in refs.items():
            if self.remote is not None:
                git_ops.fetch_remote_object(self.root, self.remote, ref)
            if git_ops.is_ancestor(self.root, sha, protected):
                result.append(
                    CleanupCandidate(
                        "queue",
                        ref,
                        sha,
                        "candidate is already in protected history",
                    )
                )
        return result

    def plan(self) -> List[CleanupCandidate]:
        try:
            protected = self.runtime.integration_sha()
            values = self._work_candidates(protected) + self._queue_candidates(
                protected
            )
        except git_ops.GitError as error:
            raise CleanupError(str(error)) from error
        return sorted(values, key=lambda item: (item.kind, item.ref))

    def run(self, execute: bool = False) -> Dict[str, Any]:
        candidates = self.plan()
        deleted: List[CleanupCandidate] = []
        if execute:
            for item in candidates:
                try:
                    if item.kind == "local_work":
                        changed = git_ops.delete_ref_cas(
                            self.root,
                            item.ref,
                            item.sha,
                        )
                    else:
                        changed = False
                        if self.remote is not None:
                            changed = git_ops.delete_remote_ref_cas(
                                self.root,
                                self.remote,
                                item.ref,
                                item.sha,
                            )
                        changed = (
                            git_ops.delete_ref_cas(
                                self.root,
                                item.ref,
                                item.sha,
                            )
                            or changed
                        )
                except git_ops.GitError as error:
                    raise CleanupError(str(error)) from error
                if changed:
                    deleted.append(item)
        return {
            "mode": "execute" if execute else "dry-run",
            "eligible": [asdict(item) for item in candidates],
            "deleted": [asdict(item) for item in deleted],
        }
