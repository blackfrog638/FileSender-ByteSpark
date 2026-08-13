#!/usr/bin/env python3
"""Evidence-bound reclamation for transient Harness V2 branches."""

from __future__ import annotations

import re
from dataclasses import asdict, dataclass
from typing import Any, Dict, List, Mapping, Optional

import git_ops
from model import ContractSet
from state import StateStore


class BranchReclamationError(RuntimeError):
    """Raised when a transient ref cannot be reclaimed safely."""


@dataclass(frozen=True)
class ReclaimableRef:
    kind: str
    ref: str
    sha: str
    evidence: str


class BranchReclaimer:
    def __init__(
        self,
        contracts: ContractSet,
        states: StateStore,
        remote: Optional[str],
    ) -> None:
        self.contracts = contracts
        self.states = states
        self.remote = remote
        self.root = contracts.root
        namespaces = contracts.manifest["ref_namespaces"]
        self.queue_prefix = namespaces["queue"]
        self.archive_prefix = namespaces["archive"]
        self.attest_prefix = namespaces["attest"]

    def reclaim_work_ref(self, ref: str, expected_sha: str) -> bool:
        if not ref.startswith("refs/heads/work/"):
            raise BranchReclamationError("work ref is outside the transient namespace")
        try:
            return git_ops.delete_ref_cas(self.root, ref, expected_sha)
        except git_ops.GitError as error:
            raise BranchReclamationError(str(error)) from error

    def reclaim_queue_ref(self, ref: str, expected_sha: str) -> bool:
        if not ref.startswith(self.queue_prefix):
            raise BranchReclamationError("queue ref is outside the transient namespace")
        changed = False
        try:
            if self.remote is not None:
                changed = git_ops.delete_remote_ref_cas(
                    self.root,
                    self.remote,
                    ref,
                    expected_sha,
                )
            changed = git_ops.delete_ref_cas(self.root, ref, expected_sha) or changed
        except git_ops.GitError as error:
            raise BranchReclamationError(str(error)) from error
        return changed

    def _queued_source_head(self, task_id: str) -> Optional[str]:
        for event in reversed(self.states.history(task_id)):
            if event["to"] != "queued":
                continue
            source_head = event["details"].get("source_head")
            if isinstance(source_head, str) and len(source_head) == 40:
                return source_head
        return None

    def _active_base(self, task_id: str) -> Optional[str]:
        for event in reversed(self.states.history(task_id)):
            if event["to"] != "active":
                continue
            base_sha = event["details"].get("base_sha")
            if isinstance(base_sha, str) and len(base_sha) == 40:
                return base_sha
        return None

    def _eligible_work_refs(self) -> List[ReclaimableRef]:
        result: List[ReclaimableRef] = []
        for ref, sha in git_ops.list_refs(self.root, "refs/heads/work/").items():
            task_id = ref.rsplit("/", 1)[-1]
            if task_id not in self.contracts.tasks:
                continue
            snapshot = self.states.read(task_id)
            expected: Optional[str] = None
            evidence = ""
            if snapshot.state == "queued":
                expected = self._queued_source_head(task_id)
                evidence = "immutable submission"
            elif snapshot.state == "done":
                if self.contracts.tasks[task_id]["type"] == "acceptance":
                    expected = self._active_base(task_id)
                    evidence = "acceptance closure"
                else:
                    expected = self._queued_source_head(task_id)
                    evidence = "published task state"
            if expected == sha:
                result.append(ReclaimableRef("local_work", ref, sha, evidence))
        return result

    def _queue_rejection_archive(
        self,
        task_id: str,
        queue_ref: str,
    ) -> Optional[str]:
        for event in reversed(self.states.history(task_id)):
            details = event["details"]
            if (
                event["reason"] == "queue_rejected"
                and details.get("rejected_queue_ref") == queue_ref
            ):
                archive_ref = details.get("archive_ref")
                return archive_ref if isinstance(archive_ref, str) else None
        return None

    def _eligible_task_queue_ref(
        self,
        ref: str,
        sha: str,
        archives: Mapping[str, str],
    ) -> Optional[ReclaimableRef]:
        match = re.fullmatch(
            r"{}[a-z0-9][a-z0-9-]{{0,63}}/[0-9]{{3}}-(XT-[0-9]{{3,}})".format(
                re.escape(self.queue_prefix)
            ),
            ref,
        )
        if match is None:
            return None
        task_id = match.group(1)
        if task_id not in self.contracts.tasks:
            return None
        snapshot = self.states.read(task_id)
        if (
            snapshot.state == "done"
            and snapshot.event is not None
            and snapshot.event["details"].get("published_sha") == sha
        ):
            return ReclaimableRef(
                "remote_queue",
                ref,
                sha,
                "published task state",
            )
        archive_ref = self._queue_rejection_archive(task_id, ref)
        if archive_ref is not None and archives.get(archive_ref) == sha:
            return ReclaimableRef(
                "remote_queue",
                ref,
                sha,
                "immutable rejection archive",
            )
        return None

    def _eligible_bootstrap_queue_ref(
        self,
        ref: str,
        sha: str,
        archives: Mapping[str, str],
        attestations: Mapping[str, str],
        protected_head: str,
    ) -> Optional[ReclaimableRef]:
        if not ref.startswith("{}bootstrap/".format(self.queue_prefix)):
            return None
        attestation_ref = "{}bootstrap/{}".format(self.attest_prefix, sha)
        if attestation_ref in attestations:
            try:
                if self.remote is not None:
                    git_ops.fetch_remote_object(self.root, self.remote, ref)
                if git_ops.is_ancestor(self.root, sha, protected_head):
                    return ReclaimableRef(
                        "remote_queue",
                        ref,
                        sha,
                        "published bootstrap attestation",
                    )
            except git_ops.GitError:
                return None
        if sha in set(archives.values()):
            return ReclaimableRef(
                "remote_queue",
                ref,
                sha,
                "immutable bootstrap archive",
            )
        return None

    def _eligible_queue_refs(self) -> List[ReclaimableRef]:
        if self.remote is None:
            return []
        try:
            queue_refs = git_ops.list_remote_refs(
                self.root,
                self.remote,
                self.queue_prefix,
            )
            archives = git_ops.list_remote_refs(
                self.root,
                self.remote,
                self.archive_prefix,
            )
            attestations = git_ops.list_remote_refs(
                self.root,
                self.remote,
                self.attest_prefix,
            )
            protected_head = git_ops.fetch_remote_object(
                self.root,
                self.remote,
                "refs/heads/{}".format(self.contracts.manifest["integration_branch"]),
            )
        except git_ops.GitError as error:
            raise BranchReclamationError(str(error)) from error
        result: List[ReclaimableRef] = []
        for ref, sha in queue_refs.items():
            item = self._eligible_task_queue_ref(ref, sha, archives)
            if item is None:
                item = self._eligible_bootstrap_queue_ref(
                    ref,
                    sha,
                    archives,
                    attestations,
                    protected_head,
                )
            if item is not None:
                result.append(item)
        return result

    def plan(self) -> List[ReclaimableRef]:
        return sorted(
            self._eligible_work_refs() + self._eligible_queue_refs(),
            key=lambda item: (item.kind, item.ref),
        )

    def run(self, execute: bool = False) -> Dict[str, Any]:
        eligible = self.plan()
        deleted: List[ReclaimableRef] = []
        if execute:
            for item in eligible:
                if item.kind == "local_work":
                    changed = self.reclaim_work_ref(item.ref, item.sha)
                else:
                    changed = self.reclaim_queue_ref(item.ref, item.sha)
                if changed:
                    deleted.append(item)
        return {
            "mode": "execute" if execute else "dry-run",
            "eligible": [asdict(item) for item in eligible],
            "deleted": [asdict(item) for item in deleted],
        }
