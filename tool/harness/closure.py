#!/usr/bin/env python3
"""Payload-free criterion evidence closure for acceptance tasks."""

from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Set

import approval
import git_ops
from attestation import AcceptanceStore
from model import ContractSet, canonical_sha256
from state import StateError, StateStore
from workspace import WorkspaceError, WorkspaceManager


CLOSURE_FIELDS = {
    "schema_version",
    "task_id",
    "plan_id",
    "plan_content_sha256",
    "criteria",
    "protected_head",
    "dependencies",
    "created_by",
    "created_at",
}
DEPENDENCY_FIELDS = {
    "task_id",
    "published_sha",
    "acceptance_ref",
    "acceptance_sha256",
    "criteria",
}


class ClosureError(RuntimeError):
    """Raised when acceptance criterion evidence is incomplete."""


def validate_closure(
    value: Any,
    *,
    task_id: str,
    plan_id: str,
    plan_digest: str,
    protected_head: str,
) -> Dict[str, Any]:
    if not isinstance(value, dict) or set(value) != CLOSURE_FIELDS:
        raise ClosureError("acceptance closure has invalid fields")
    closure = dict(value)
    expected = {
        "schema_version": 1,
        "task_id": task_id,
        "plan_id": plan_id,
        "plan_content_sha256": plan_digest,
        "protected_head": protected_head,
    }
    for field, wanted in expected.items():
        if closure[field] != wanted:
            raise ClosureError("acceptance closure {} is invalid".format(field))
    criteria = closure["criteria"]
    if (
        not isinstance(criteria, list)
        or not criteria
        or len(criteria) != len(set(criteria))
    ):
        raise ClosureError("acceptance closure criteria are invalid")
    dependencies = closure["dependencies"]
    if not isinstance(dependencies, list) or not dependencies:
        raise ClosureError("acceptance closure dependencies are invalid")
    task_ids: Set[str] = set()
    covered: Set[str] = set()
    for item in dependencies:
        if not isinstance(item, dict) or set(item) != DEPENDENCY_FIELDS:
            raise ClosureError("acceptance closure dependency is invalid")
        dependency_id = item["task_id"]
        if not isinstance(dependency_id, str) or dependency_id in task_ids:
            raise ClosureError("acceptance closure dependency IDs are invalid")
        task_ids.add(dependency_id)
        if (
            not isinstance(item["published_sha"], str)
            or len(item["published_sha"]) != 40
            or not isinstance(item["acceptance_ref"], str)
            or not item["acceptance_ref"].startswith("refs/heads/attest/acceptance/")
            or not isinstance(item["acceptance_sha256"], str)
            or len(item["acceptance_sha256"]) != 64
            or not isinstance(item["criteria"], list)
            or not item["criteria"]
        ):
            raise ClosureError("acceptance closure dependency evidence is invalid")
        covered.update(item["criteria"])
    if not set(criteria).issubset(covered):
        raise ClosureError("acceptance closure criterion coverage is incomplete")
    actor = closure["created_by"]
    if not isinstance(actor, dict) or set(actor) != {
        "kind",
        "id",
        "name",
        "email",
    }:
        raise ClosureError("acceptance closure actor is invalid")
    if not isinstance(closure["created_at"], str) or not closure["created_at"].endswith(
        "Z"
    ):
        raise ClosureError("acceptance closure timestamp is invalid")
    return closure


class AcceptanceCloser:
    def __init__(
        self,
        contracts: ContractSet,
        states: StateStore,
        workspaces: WorkspaceManager,
        remote: str,
    ) -> None:
        self.contracts = contracts
        self.states = states
        self.workspaces = workspaces
        self.remote = remote
        self.root = contracts.root
        self.attestations = AcceptanceStore(contracts, remote=remote)
        self.prefix = contracts.manifest["ref_namespaces"]["attest"]

    def _ref(self, task_id: str, protected_head: str) -> str:
        return "{}closure/{}/{}".format(self.prefix, task_id, protected_head)

    def _requirements(self, task_id: str) -> List[Mapping[str, Any]]:
        task = self.contracts.tasks[task_id]
        plan = self.contracts.plans[task["plan"]]
        return [
            requirement
            for requirement in plan["requirements"]
            if requirement["acceptance_owner"] == task_id
        ]

    def close(self, task_id: str) -> Mapping[str, Any]:
        task = self.contracts.tasks[task_id]
        if task["type"] != "acceptance":
            raise ClosureError("{} is not an acceptance task".format(task_id))
        try:
            approval.require_task_plan(self.contracts, task_id, self.remote)
        except approval.ApprovalError as error:
            raise ClosureError(str(error)) from error
        active = self.workspaces.active_workspace(task_id)
        if not git_ops.is_clean(active.path):
            raise ClosureError("acceptance worktree must be clean")
        if git_ops.object_id(active.path, "HEAD") != active.base_sha:
            raise ClosureError("acceptance closure must have zero payload")
        requirements = self._requirements(task_id)
        if not requirements:
            raise ClosureError("{} owns no Plan requirement".format(task_id))
        plan = self.contracts.plans[task["plan"]]
        plan_digest = plan["approval"]["content_sha256"]
        protected_ref = "refs/heads/{}".format(
            self.contracts.manifest["integration_branch"]
        )
        protected_head = git_ops.fetch_remote_object(
            self.root, self.remote, protected_ref
        )
        required_criteria = sorted(
            {
                criterion["id"]
                for requirement in requirements
                for criterion in requirement["criteria"]
            }
        )
        implementation_tasks = sorted(
            {
                implementation
                for requirement in requirements
                for implementation in requirement["implementation_tasks"]
            }
        )
        dependencies: List[Dict[str, Any]] = []
        covered: Set[str] = set()
        for dependency_id in implementation_tasks:
            snapshot = self.states.read(dependency_id)
            if snapshot.state != "done" or snapshot.event is None:
                raise ClosureError(
                    "{} implementation task {} is not done".format(
                        task_id, dependency_id
                    )
                )
            details = snapshot.event["details"]
            published_sha = details.get("published_sha")
            acceptance_ref = details.get("acceptance_ref")
            expected_digest = details.get("acceptance_attestation_sha256")
            if (
                not isinstance(published_sha, str)
                or not isinstance(acceptance_ref, str)
                or not isinstance(expected_digest, str)
                or not git_ops.is_ancestor(self.root, published_sha, protected_head)
            ):
                raise ClosureError(
                    "{} publication evidence is incomplete".format(dependency_id)
                )
            acceptance = self.attestations.read(dependency_id, published_sha)
            if (
                canonical_sha256(acceptance) != expected_digest
                or self.attestations.ref(dependency_id, published_sha) != acceptance_ref
            ):
                raise ClosureError(
                    "{} acceptance evidence does not match state".format(dependency_id)
                )
            evidenced = {
                criterion_id
                for artifact in acceptance["workflow"]["artifacts"]
                for criterion_id in artifact["criterion_ids"]
            }
            owned = sorted(
                set(self.contracts.tasks[dependency_id]["criteria"])
                & set(required_criteria)
            )
            if not set(owned).issubset(evidenced):
                raise ClosureError("{} lacks criterion evidence".format(dependency_id))
            covered.update(owned)
            dependencies.append(
                {
                    "task_id": dependency_id,
                    "published_sha": published_sha,
                    "acceptance_ref": acceptance_ref,
                    "acceptance_sha256": expected_digest,
                    "criteria": owned,
                }
            )
        if not set(required_criteria).issubset(covered):
            raise ClosureError("{} criterion evidence is incomplete".format(task_id))
        value: Dict[str, Any] = {
            "schema_version": 1,
            "task_id": task_id,
            "plan_id": task["plan"],
            "plan_content_sha256": plan_digest,
            "criteria": required_criteria,
            "protected_head": protected_head,
            "dependencies": dependencies,
            "created_by": dict(self.states.actor),
            "created_at": self.states.clock(),
        }
        validate_closure(
            value,
            task_id=task_id,
            plan_id=task["plan"],
            plan_digest=plan_digest,
            protected_head=protected_head,
        )
        ref = self._ref(task_id, protected_head)
        remote_sha = git_ops.remote_ref_sha(self.root, self.remote, ref)
        if remote_sha is not None:
            try:
                git_ops.fetch_immutable_ref(self.root, self.remote, ref)
            except git_ops.GitError as error:
                raise ClosureError(str(error)) from error
        existing = git_ops.ref_sha(self.root, ref)
        if existing is None:
            commit = git_ops.commit_json(
                self.root,
                value,
                "{} acceptance evidence closure".format(task_id),
                filename="closure.json",
            )
            try:
                git_ops.update_ref_cas(self.root, ref, commit, None)
                git_ops.push_ref_cas(self.root, self.remote, commit, ref, None)
            except git_ops.GitError as error:
                git_ops.run_git(
                    self.root,
                    "update-ref",
                    "-d",
                    ref,
                    commit,
                    check=False,
                )
                raise ClosureError(str(error)) from error
        else:
            previous = git_ops.read_json_object(self.root, existing, "closure.json")
            validate_closure(
                previous,
                task_id=task_id,
                plan_id=task["plan"],
                plan_digest=plan_digest,
                protected_head=protected_head,
            )
            value = previous
            commit = existing
            if remote_sha is None:
                try:
                    git_ops.push_ref_cas(
                        self.root,
                        self.remote,
                        commit,
                        ref,
                        None,
                    )
                except git_ops.GitError as error:
                    raise ClosureError(str(error)) from error
        try:
            self.states.transition(
                task_id,
                "active",
                "done",
                "evidence_closure",
                details={
                    "acceptance_attestation_sha256": canonical_sha256(value),
                    "acceptance_ref": ref,
                    "acceptance_commit": commit,
                    "published_sha": protected_head,
                },
            )
        except StateError as error:
            raise ClosureError(str(error)) from error
        try:
            self.workspaces.release_done_worktree(
                task_id,
                active.path,
                active.branch,
                active.base_sha,
            )
        except WorkspaceError as error:
            raise ClosureError(
                "acceptance closed but work branch cleanup failed: {}".format(error)
            ) from error
        return value
