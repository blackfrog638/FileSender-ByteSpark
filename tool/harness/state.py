#!/usr/bin/env python3
"""Append-only Harness V2 runtime state backed by Git refs."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Dict, List, Mapping, Optional, Tuple

import git_ops
from model import (
    ContractError,
    ContractSet,
    TASK_ID,
    canonical_sha256,
)


STATES = {"ready", "active", "queued", "done"}
TRANSITIONS = {
    ("ready", "active"),
    ("active", "ready"),
    ("active", "queued"),
    ("active", "done"),
    ("queued", "active"),
    ("queued", "done"),
}
EVENT_FIELDS = {
    "schema_version",
    "task_id",
    "sequence",
    "previous_event_sha256",
    "from",
    "to",
    "reason",
    "actor",
    "task_spec_blob",
    "plan_blob",
    "submission_ref",
    "details",
    "created_at",
}
ACTOR_FIELDS = {"kind", "id", "name", "email"}


class StateError(RuntimeError):
    """Raised when a runtime state event is invalid or stale."""


@dataclass(frozen=True)
class StateSnapshot:
    task_id: str
    state: str
    ref: str
    commit: Optional[str]
    event: Optional[Dict[str, Any]]

    @property
    def sequence(self) -> int:
        if self.event is None:
            return 0
        return int(self.event["sequence"])


def _utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def git_actor(root: Path) -> Dict[str, str]:
    name = git_ops.git_text(root, "config", "--get", "user.name", check=False)
    email = git_ops.git_text(root, "config", "--get", "user.email", check=False)
    if not name or not email or "@" not in email:
        raise StateError("repository Git identity is incomplete")
    return {
        "kind": "user",
        "id": email,
        "name": name,
        "email": email,
    }


def _validate_actor(value: Any) -> Dict[str, str]:
    if not isinstance(value, dict) or set(value) != ACTOR_FIELDS:
        raise StateError("state actor has invalid fields")
    result: Dict[str, str] = {}
    for field in sorted(ACTOR_FIELDS):
        item = value[field]
        if not isinstance(item, str) or not item.strip() or item != item.strip():
            raise StateError("state actor {} is invalid".format(field))
        result[field] = item
    if result["kind"] not in {"user", "ci", "queue-worker", "recovery"}:
        raise StateError("state actor kind is invalid")
    if "@" not in result["email"]:
        raise StateError("state actor email is invalid")
    return result


def validate_event(
    value: Any,
    task_id: str,
    previous: Optional[Mapping[str, Any]],
) -> Dict[str, Any]:
    if not isinstance(value, dict) or set(value) != EVENT_FIELDS:
        raise StateError("{} state event has invalid fields".format(task_id))
    event = dict(value)
    if event["schema_version"] != 1 or event["task_id"] != task_id:
        raise StateError("{} state event identity is invalid".format(task_id))
    sequence = event["sequence"]
    if not isinstance(sequence, int) or isinstance(sequence, bool) or sequence < 1:
        raise StateError("{} state sequence is invalid".format(task_id))
    if event["from"] not in STATES or event["to"] not in STATES:
        raise StateError("{} state name is invalid".format(task_id))
    if (event["from"], event["to"]) not in TRANSITIONS:
        raise StateError(
            "{} illegal state transition {} -> {}".format(
                task_id, event["from"], event["to"]
            )
        )
    if not isinstance(event["reason"], str) or not event["reason"].strip():
        raise StateError("{} state reason is invalid".format(task_id))
    _validate_actor(event["actor"])
    for field in ("task_spec_blob", "plan_blob"):
        item = event[field]
        if not isinstance(item, str) or len(item) != 40:
            raise StateError("{} {} is invalid".format(task_id, field))
    submission_ref = event["submission_ref"]
    if submission_ref is not None and (
        not isinstance(submission_ref, str)
        or not submission_ref.startswith("refs/heads/submit/")
    ):
        raise StateError("{} submission_ref is invalid".format(task_id))
    if not isinstance(event["details"], dict):
        raise StateError("{} state details must be an object".format(task_id))
    if not isinstance(event["created_at"], str) or not event["created_at"].endswith(
        "Z"
    ):
        raise StateError("{} state timestamp is invalid".format(task_id))
    if previous is None:
        if sequence != 1 or event["from"] != "ready":
            raise StateError("{} initial state event is invalid".format(task_id))
        if event["previous_event_sha256"] != "":
            raise StateError("{} initial previous digest must be empty".format(task_id))
    else:
        if sequence != int(previous["sequence"]) + 1:
            raise StateError("{} state sequence is not contiguous".format(task_id))
        if event["from"] != previous["to"]:
            raise StateError("{} state chain is discontinuous".format(task_id))
        if event["previous_event_sha256"] != canonical_sha256(previous):
            raise StateError("{} previous event digest is invalid".format(task_id))
    if event["to"] == "ready" and event["reason"] != "claim_rollback":
        raise StateError(
            "{} can return to ready only for claim rollback".format(task_id)
        )
    if event["to"] == "queued" and submission_ref is None:
        raise StateError("{} queued event requires submission_ref".format(task_id))
    if event["to"] == "done":
        required = {"acceptance_attestation_sha256", "published_sha"}
        if not required.issubset(event["details"]):
            raise StateError(
                "{} done event is missing publication proof".format(task_id)
            )
    return event


class StateStore:
    def __init__(
        self,
        contracts: ContractSet,
        remote: Optional[str] = None,
        actor: Optional[Mapping[str, str]] = None,
        clock: Optional[Callable[[], str]] = None,
    ) -> None:
        self.contracts = contracts
        self.root = contracts.root
        self.remote = remote
        self.actor = dict(actor) if actor is not None else git_actor(self.root)
        _validate_actor(self.actor)
        self.clock = clock or _utc_now
        self.prefix = contracts.manifest["ref_namespaces"]["state"]

    def ref(self, task_id: str) -> str:
        if TASK_ID.fullmatch(task_id) is None:
            raise StateError("invalid task id {}".format(task_id))
        return "{}{}".format(self.prefix, task_id)

    def _refresh_ref(self, task_id: str) -> None:
        if self.remote is None:
            return
        ref = self.ref(task_id)
        remote_sha = git_ops.remote_ref_sha(self.root, self.remote, ref)
        local_sha = git_ops.ref_sha(self.root, ref)
        if remote_sha == local_sha:
            return
        if remote_sha is None:
            if local_sha is not None:
                raise StateError(
                    "{} has unpushed local state and no remote ref".format(task_id)
                )
            return
        result = git_ops.run_git(
            self.root,
            "fetch",
            "--no-tags",
            self.remote,
            "{}:{}".format(ref, ref),
            check=False,
        )
        if result.returncode != 0:
            raise StateError("cannot refresh remote state for {}".format(task_id))

    def refresh_all(self) -> None:
        if self.remote is None:
            return
        refspec = "+{}*:{}*".format(self.prefix, self.prefix)
        result = git_ops.run_git(
            self.root,
            "fetch",
            "--no-tags",
            "--prune",
            self.remote,
            refspec,
            check=False,
        )
        if result.returncode != 0:
            raise StateError("cannot refresh remote state refs")

    def read(self, task_id: str, refresh: bool = True) -> StateSnapshot:
        if task_id not in self.contracts.tasks:
            raise StateError("unknown task {}".format(task_id))
        if refresh:
            self._refresh_ref(task_id)
        ref = self.ref(task_id)
        commit = git_ops.ref_sha(self.root, ref)
        if commit is None:
            return StateSnapshot(task_id, "ready", ref, None, None)
        events = self.history(task_id, refresh=False)
        event = events[-1]
        return StateSnapshot(task_id, event["to"], ref, commit, event)

    def history(self, task_id: str, refresh: bool = True) -> List[Dict[str, Any]]:
        if refresh:
            self._refresh_ref(task_id)
        ref = self.ref(task_id)
        if git_ops.ref_sha(self.root, ref) is None:
            return []
        commits = git_ops.first_parent_history(self.root, ref)
        events: List[Dict[str, Any]] = []
        previous: Optional[Dict[str, Any]] = None
        for index, commit in enumerate(commits):
            parents = git_ops.commit_parents(self.root, commit)
            if index == 0 and parents:
                raise StateError("{} initial state commit has a parent".format(task_id))
            if index > 0 and parents != [commits[index - 1]]:
                raise StateError(
                    "{} state history is not first-parent linear".format(task_id)
                )
            raw = git_ops.read_json_object(self.root, commit)
            event = validate_event(raw, task_id, previous)
            events.append(event)
            previous = event
        return events

    def list(self, refresh: bool = True) -> Dict[str, StateSnapshot]:
        if refresh:
            self.refresh_all()
        snapshots: Dict[str, StateSnapshot] = {}
        for task_id in sorted(self.contracts.tasks):
            snapshots[task_id] = self.read(task_id, refresh=False)
        return snapshots

    def _contract_blobs(self, task_id: str) -> Tuple[str, str]:
        task = self.contracts.tasks[task_id]
        task_blob = git_ops.object_id(
            self.root, "HEAD:.agents/tasks/{}.json".format(task_id)
        )
        plan_blob = git_ops.object_id(
            self.root, "HEAD:.agents/plans/{}.json".format(task["plan"])
        )
        return task_blob, plan_blob

    def transition(
        self,
        task_id: str,
        expected_state: str,
        target_state: str,
        reason: str,
        details: Optional[Mapping[str, Any]] = None,
        submission_ref: Optional[str] = None,
    ) -> StateSnapshot:
        if (expected_state, target_state) not in TRANSITIONS:
            raise StateError(
                "illegal requested transition {} -> {}".format(
                    expected_state, target_state
                )
            )
        task_type = self.contracts.tasks[task_id]["type"]
        if expected_state == "active" and target_state == "done":
            if task_type != "acceptance" or reason != "evidence_closure":
                raise StateError(
                    "active -> done is reserved for acceptance evidence closure"
                )
        snapshot = self.read(task_id)
        if snapshot.state != expected_state:
            raise StateError(
                "{} state is {}, expected {}".format(
                    task_id, snapshot.state, expected_state
                )
            )
        task_blob, plan_blob = self._contract_blobs(task_id)
        if (
            snapshot.event is not None
            and target_state in {"queued", "done"}
            and (
                snapshot.event["task_spec_blob"] != task_blob
                or snapshot.event["plan_blob"] != plan_blob
            )
        ):
            raise StateError(
                "{} static contract changed during immutable delivery".format(task_id)
            )
        event: Dict[str, Any] = {
            "schema_version": 1,
            "task_id": task_id,
            "sequence": snapshot.sequence + 1,
            "previous_event_sha256": (
                canonical_sha256(snapshot.event) if snapshot.event is not None else ""
            ),
            "from": expected_state,
            "to": target_state,
            "reason": reason,
            "actor": dict(self.actor),
            "task_spec_blob": task_blob,
            "plan_blob": plan_blob,
            "submission_ref": submission_ref,
            "details": dict(details or {}),
            "created_at": self.clock(),
        }
        validate_event(event, task_id, snapshot.event)
        try:
            new_commit = git_ops.append_json_ref(
                self.root,
                snapshot.ref,
                event,
                "{}: {} -> {}".format(task_id, expected_state, target_state),
                snapshot.commit,
            )
        except git_ops.GitError as error:
            raise StateError(str(error)) from error
        if self.remote is not None:
            try:
                git_ops.push_ref_cas(
                    self.root,
                    self.remote,
                    new_commit,
                    snapshot.ref,
                    snapshot.commit,
                )
            except git_ops.GitError as error:
                try:
                    if snapshot.commit is None:
                        git_ops.run_git(
                            self.root,
                            "update-ref",
                            "-d",
                            snapshot.ref,
                            new_commit,
                        )
                    else:
                        git_ops.update_ref_cas(
                            self.root, snapshot.ref, snapshot.commit, new_commit
                        )
                except git_ops.GitError:
                    pass
                raise StateError(str(error)) from error
        return self.read(task_id, refresh=False)
