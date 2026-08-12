#!/usr/bin/env python3
"""Immutable project-owner approval refs for Harness V2 plans."""

from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, Mapping, Optional

import git_ops
from model import ContractSet, PLAN_ID, SHA256


APPROVAL_FIELDS = {
    "schema_version",
    "plan_id",
    "content_sha256",
    "approved_by",
    "created_at",
}
ACTOR_FIELDS = {"id", "name", "email"}


class ApprovalError(RuntimeError):
    """Raised when a Plan lacks authoritative project-owner approval."""


def _actor(root: Path, owner: Mapping[str, Any]) -> Dict[str, str]:
    name = git_ops.git_text(root, "config", "--get", "user.name", check=False)
    email = git_ops.git_text(root, "config", "--get", "user.email", check=False)
    if name != owner["name"] or email != owner["email"]:
        raise ApprovalError("current Git identity is not the configured project owner")
    return {
        "id": str(owner["id"]),
        "name": name,
        "email": email,
    }


def validate_approval(
    value: Any,
    *,
    plan_id: str,
    content_sha256: str,
    owner: Mapping[str, Any],
) -> Dict[str, Any]:
    if not isinstance(value, dict) or set(value) != APPROVAL_FIELDS:
        raise ApprovalError("Plan approval has invalid fields")
    approval = dict(value)
    if (
        approval["schema_version"] != 1
        or approval["plan_id"] != plan_id
        or approval["content_sha256"] != content_sha256
        or PLAN_ID.fullmatch(plan_id) is None
        or SHA256.fullmatch(content_sha256) is None
    ):
        raise ApprovalError("{} approval identity is invalid".format(plan_id))
    actor = approval["approved_by"]
    if not isinstance(actor, dict) or set(actor) != ACTOR_FIELDS:
        raise ApprovalError("{} approval actor is invalid".format(plan_id))
    if actor != {
        "id": owner["id"],
        "name": owner["name"],
        "email": owner["email"],
    }:
        raise ApprovalError(
            "{} was not approved by the configured project owner".format(plan_id)
        )
    if not isinstance(approval["created_at"], str) or not approval[
        "created_at"
    ].endswith("Z"):
        raise ApprovalError("{} approval timestamp is invalid".format(plan_id))
    return approval


class ApprovalStore:
    def __init__(
        self,
        root: Path,
        manifest: Mapping[str, Any],
        remote: Optional[str],
    ) -> None:
        self.root = root
        self.manifest = manifest
        self.remote = remote
        self.prefix = manifest["ref_namespaces"]["approve"]

    def ref(self, plan_id: str, content_sha256: str) -> str:
        return "{}{}/{}".format(self.prefix, plan_id, content_sha256)

    def write(
        self,
        plan: Mapping[str, Any],
        created_at: str,
    ) -> str:
        plan_id = str(plan["id"])
        embedded = plan.get("approval")
        if plan.get("status") != "approved" or not isinstance(embedded, dict):
            raise ApprovalError("{} is not approved".format(plan_id))
        content_sha256 = str(embedded.get("content_sha256", ""))
        value = {
            "schema_version": 1,
            "plan_id": plan_id,
            "content_sha256": content_sha256,
            "approved_by": _actor(self.root, self.manifest["project_owner"]),
            "created_at": created_at,
        }
        validate_approval(
            value,
            plan_id=plan_id,
            content_sha256=content_sha256,
            owner=self.manifest["project_owner"],
        )
        ref = self.ref(plan_id, content_sha256)
        existing = git_ops.ref_sha(self.root, ref)
        if existing is not None:
            previous = git_ops.read_json_object(self.root, existing, "approval.json")
            validate_approval(
                previous,
                plan_id=plan_id,
                content_sha256=content_sha256,
                owner=self.manifest["project_owner"],
            )
            if self.remote is not None:
                remote_sha = git_ops.remote_ref_sha(self.root, self.remote, ref)
                if remote_sha is None:
                    try:
                        git_ops.push_ref_cas(
                            self.root,
                            self.remote,
                            existing,
                            ref,
                            None,
                        )
                    except git_ops.GitError as error:
                        raise ApprovalError(str(error)) from error
                elif remote_sha != existing:
                    raise ApprovalError(
                        "{} remote approval ref differs".format(plan_id)
                    )
            return existing
        commit = git_ops.commit_json(
            self.root,
            value,
            "{} project-owner approval".format(plan_id),
            filename="approval.json",
        )
        try:
            git_ops.update_ref_cas(self.root, ref, commit, None)
            if self.remote is not None:
                git_ops.push_ref_cas(self.root, self.remote, commit, ref, None)
        except git_ops.GitError as error:
            git_ops.run_git(self.root, "update-ref", "-d", ref, commit, check=False)
            raise ApprovalError(str(error)) from error
        return commit

    def require(self, plan: Mapping[str, Any]) -> Mapping[str, Any]:
        plan_id = str(plan["id"])
        embedded = plan.get("approval")
        if plan.get("status") != "approved" or not isinstance(embedded, dict):
            raise ApprovalError("{} is not approved".format(plan_id))
        content_sha256 = str(embedded.get("content_sha256", ""))
        ref = self.ref(plan_id, content_sha256)
        if self.remote is not None:
            try:
                git_ops.fetch_immutable_ref(self.root, self.remote, ref)
            except git_ops.GitError as error:
                raise ApprovalError(str(error)) from error
        commit = git_ops.ref_sha(self.root, ref)
        if commit is None:
            raise ApprovalError("{} has no authoritative approval ref".format(plan_id))
        value = git_ops.read_json_object(self.root, commit, "approval.json")
        return validate_approval(
            value,
            plan_id=plan_id,
            content_sha256=content_sha256,
            owner=self.manifest["project_owner"],
        )


def require_task_plan(
    contracts: ContractSet,
    task_id: str,
    remote: Optional[str],
) -> Mapping[str, Any]:
    task = contracts.tasks[task_id]
    plan = contracts.plans[task["plan"]]
    return ApprovalStore(contracts.root, contracts.manifest, remote).require(plan)
