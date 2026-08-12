#!/usr/bin/env python3
"""Harness V2 immutable submissions, merge trains, and publication."""

from __future__ import annotations

import datetime as dt
import fnmatch
import hashlib
import re
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Dict, List, Mapping, Optional, Sequence, Tuple

import attestation
import approval
import git_ops
import state
import tdd
from executor import GateExecutor
from gates import plan_for_platform, plan_gates
from model import (
    ContractSet,
    RISK_RANK,
    canonical_sha256,
    load_json,
    task_required_platforms,
)
from module_inventory import (
    ArchitectureDeclarationError,
    validate_task_architecture,
)
from workspace import WorkspaceManager


SUBMISSION_FIELDS = {
    "schema_version",
    "task_id",
    "attempt",
    "base_sha",
    "source_head",
    "source_commits",
    "payload_patch_sha256",
    "task_spec_blob",
    "plan_blob",
    "gate_policy_sha256",
    "review_attestation",
    "tdd_proof",
    "reviewer",
    "created_at",
}
TRUST_ROOT_PATTERNS = (
    ".agents/manifest.json",
    ".agents/gates.json",
    ".agents/risk-routing.json",
    ".agents/schemas/**",
    ".agents/architecture/**",
    ".agents/commit-identity.json",
    ".agents/migration-v1.json",
    ".github/workflows/**",
    "tool/harness/**",
    "AGENTS.md",
    "Makefile",
)


class QueueError(RuntimeError):
    """Raised when submission, candidate, or publication is unsafe."""


class PublicationRecoveryRequired(QueueError):
    """Raised after product publication when state finalization must resume."""


@dataclass(frozen=True)
class Submission:
    task_id: str
    ref: str
    commit: str
    manifest: Mapping[str, Any]


@dataclass(frozen=True)
class TrainEntry:
    index: int
    task_id: str
    parent_sha: str
    candidate_sha: str
    queue_ref: str
    submission_ref: str
    submission: Mapping[str, Any]


@dataclass(frozen=True)
class MergeTrain:
    train_id: str
    base_sha: str
    entries: Tuple[TrainEntry, ...]


def _utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def _payload_bytes(root: Path, base: str, head: str) -> bytes:
    result = git_ops.run_git(root, "diff", "--binary", base, head)
    if not result.stdout:
        raise QueueError("submission payload is empty")
    return result.stdout


def _payload_sha256(root: Path, base: str, head: str) -> str:
    return hashlib.sha256(_payload_bytes(root, base, head)).hexdigest()


def _matches(path: str, patterns: Sequence[str]) -> bool:
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def _trust_root_changes(paths: Sequence[str]) -> List[str]:
    return sorted(path for path in paths if _matches(path, TRUST_ROOT_PATTERNS))


def _delivery_identity(root: Path) -> Dict[str, str]:
    policy = load_json(root / ".agents" / "commit-identity.json")
    required = {"schema_version", "name", "email", "immutable"}
    if (
        set(policy) != required
        or policy["schema_version"] != 2
        or policy["immutable"] is not True
        or not isinstance(policy["name"], str)
        or not policy["name"].strip()
        or not isinstance(policy["email"], str)
        or "@" not in policy["email"]
    ):
        raise QueueError("repository commit identity policy is invalid")
    return {
        "GIT_AUTHOR_NAME": policy["name"],
        "GIT_AUTHOR_EMAIL": policy["email"],
        "GIT_COMMITTER_NAME": policy["name"],
        "GIT_COMMITTER_EMAIL": policy["email"],
    }


def _validate_actor(value: Mapping[str, Any]) -> Dict[str, str]:
    required = {"kind", "id", "name", "email"}
    if not isinstance(value, dict) or set(value) != required:
        raise QueueError("reviewer actor has invalid fields")
    actor = {key: str(value[key]) for key in required}
    if any(not item.strip() or item != item.strip() for item in actor.values()):
        raise QueueError("reviewer actor is invalid")
    if "@" not in actor["email"]:
        raise QueueError("reviewer email is invalid")
    return actor


def validate_submission(value: Any, task_id: str) -> Dict[str, Any]:
    if not isinstance(value, dict) or set(value) != SUBMISSION_FIELDS:
        raise QueueError("{} submission has invalid fields".format(task_id))
    submission = dict(value)
    if submission["schema_version"] != 1 or submission["task_id"] != task_id:
        raise QueueError("{} submission identity is invalid".format(task_id))
    if (
        not isinstance(submission["attempt"], int)
        or isinstance(submission["attempt"], bool)
        or submission["attempt"] < 1
    ):
        raise QueueError("{} submission attempt is invalid".format(task_id))
    for field in ("base_sha", "source_head", "task_spec_blob", "plan_blob"):
        if not isinstance(submission[field], str) or len(submission[field]) != 40:
            raise QueueError("{} submission {} is invalid".format(task_id, field))
    commits = submission["source_commits"]
    if (
        not isinstance(commits, list)
        or not commits
        or any(not isinstance(item, str) or len(item) != 40 for item in commits)
    ):
        raise QueueError("{} source commits are invalid".format(task_id))
    for field in ("payload_patch_sha256", "gate_policy_sha256"):
        value = submission[field]
        if not isinstance(value, str) or len(value) != 64:
            raise QueueError("{} submission {} is invalid".format(task_id, field))
    if not isinstance(submission["review_attestation"], dict):
        raise QueueError("{} review attestation is invalid".format(task_id))
    if not isinstance(submission["tdd_proof"], dict):
        raise QueueError("{} TDD proof is invalid".format(task_id))
    _validate_actor(submission["reviewer"])
    if not isinstance(submission["created_at"], str) or not submission[
        "created_at"
    ].endswith("Z"):
        raise QueueError("{} submission timestamp is invalid".format(task_id))
    return submission


def _load_submission(contracts: ContractSet, ref: str) -> Submission:
    root = contracts.root
    commit = git_ops.ref_sha(root, ref)
    if commit is None:
        raise QueueError("submission ref is missing: {}".format(ref))
    value = git_ops.read_json_object(root, commit, "submission.json")
    task_id = str(value.get("task_id", ""))
    manifest = validate_submission(value, task_id)
    if task_id not in contracts.tasks:
        raise QueueError("submission references unknown task {}".format(task_id))
    if git_ops.commit_parents(root, commit) != [manifest["source_head"]]:
        raise QueueError(
            "{} submission does not preserve source history".format(task_id)
        )
    if not git_ops.is_ancestor(root, manifest["base_sha"], manifest["source_head"]):
        raise QueueError("{} submission source is outside its base".format(task_id))
    commits = git_ops.commit_range(root, manifest["base_sha"], manifest["source_head"])
    if commits != manifest["source_commits"]:
        raise QueueError("{} submission source commit list changed".format(task_id))
    if (
        _payload_sha256(root, manifest["base_sha"], manifest["source_head"])
        != manifest["payload_patch_sha256"]
    ):
        raise QueueError("{} submission payload digest changed".format(task_id))
    task = contracts.tasks[task_id]
    expected_task_blob = git_ops.object_id(
        root,
        "{}:.agents/tasks/{}.json".format(manifest["source_head"], task_id),
    )
    expected_plan_blob = git_ops.object_id(
        root,
        "{}:.agents/plans/{}.json".format(manifest["source_head"], task["plan"]),
    )
    if (
        manifest["task_spec_blob"] != expected_task_blob
        or manifest["plan_blob"] != expected_plan_blob
        or manifest["gate_policy_sha256"] != canonical_sha256(contracts.gate_policy)
    ):
        raise QueueError("{} submission governance context changed".format(task_id))
    return Submission(task_id, ref, commit, manifest)


class SubmissionManager:
    def __init__(
        self,
        contracts: ContractSet,
        states: state.StateStore,
        workspaces: WorkspaceManager,
        reviewer: Mapping[str, Any],
        remote: Optional[str] = None,
        clock: Optional[Callable[[], str]] = None,
    ) -> None:
        self.contracts = contracts
        self.states = states
        self.workspaces = workspaces
        self.reviewer = _validate_actor(reviewer)
        self.remote = remote
        self.clock = clock or _utc_now
        self.root = contracts.root
        self.prefix = contracts.manifest["ref_namespaces"]["submit"]

    def _attempt(self, task_id: str) -> int:
        prefix = "{}{}/".format(self.prefix, task_id)
        refs = git_ops.list_refs(self.root, prefix)
        attempts = []
        for ref in refs:
            suffix = ref[len(prefix) :]
            if suffix.isdigit():
                attempts.append(int(suffix))
        return max(attempts, default=0) + 1

    def _require_reviewer_independence(self, task_id: str, owner: str) -> None:
        task = self.contracts.tasks[task_id]
        high_risk = any(
            RISK_RANK[level] >= RISK_RANK["high"] for level in task["risk"].values()
        )
        if high_risk and self.reviewer["id"] == owner:
            raise QueueError(
                "{} high-risk submission requires an independent reviewer".format(
                    task_id
                )
            )

    def _reject_inflight_contract_changes(
        self, task_id: str, changed: Sequence[str]
    ) -> None:
        changed_set = set(changed)
        for other_id, snapshot in self.states.list().items():
            if snapshot.state not in {"active", "queued"}:
                continue
            task_path = ".agents/tasks/{}.json".format(other_id)
            plan_path = ".agents/plans/{}.json".format(
                self.contracts.tasks[other_id]["plan"]
            )
            protected = sorted(changed_set & {task_path, plan_path})
            if protected:
                raise QueueError(
                    "{} changes the contract of {} while it is {}: {}".format(
                        task_id,
                        other_id,
                        snapshot.state,
                        ", ".join(protected),
                    )
                )

    def submit(self, task_id: str, red_sha: Optional[str] = None) -> Submission:
        if self.contracts.tasks[task_id]["type"] == "acceptance":
            raise QueueError("{} uses payload-free acceptance-close".format(task_id))
        active = self.workspaces.active_workspace(task_id)
        self.workspaces.require_fresh(task_id)
        if not git_ops.is_clean(active.path):
            raise QueueError("{} worktree must be clean".format(task_id))
        source_head = git_ops.object_id(active.path, "HEAD")
        if source_head == active.base_sha:
            raise QueueError("{} has no payload".format(task_id))
        task = self.contracts.tasks[task_id]
        changed = git_ops.changed_paths(active.path, active.base_sha, source_head)
        outside = [path for path in changed if not _matches(path, task["owned_paths"])]
        if outside:
            raise QueueError(
                "{} payload is outside owned paths: {}".format(
                    task_id, ", ".join(outside)
                )
            )
        self._reject_inflight_contract_changes(task_id, changed)
        trust_root = _trust_root_changes(changed)
        if trust_root:
            raise QueueError(
                "{} standard queue payload changes its verification trust root: "
                "{}".format(task_id, ", ".join(trust_root))
            )
        try:
            validate_task_architecture(active.path, task, changed)
        except ArchitectureDeclarationError as error:
            raise QueueError(str(error)) from error
        active_snapshot = self.states.read(task_id)
        owner = str(active_snapshot.event["details"]["owner"])
        self._require_reviewer_independence(task_id, owner)
        tdd_manager = tdd.TddManager(self.contracts, self.states, self.workspaces)
        if task["tdd"]["mode"] in tdd.RED_MODES:
            if red_sha is None:
                raise QueueError("{} submission requires Red SHA".format(task_id))
            tdd_proof = dict(tdd_manager.review_green(task_id, red_sha))
        else:
            tdd_proof = {
                "schema_version": 1,
                "task_id": task_id,
                "mode": task["tdd"]["mode"],
                "status": "not_applicable",
            }
        review_plan = plan_gates(self.contracts, task_id, "review", changed)
        review_result = GateExecutor(
            self.contracts, execution_root=active.path
        ).execute(review_plan)
        review_result.require_success()
        task_blob = git_ops.object_id(
            self.root, "HEAD:.agents/tasks/{}.json".format(task_id)
        )
        plan_blob = git_ops.object_id(
            self.root,
            "HEAD:.agents/plans/{}.json".format(task["plan"]),
        )
        source_commits = git_ops.commit_range(active.path, active.base_sha, source_head)
        attempt = self._attempt(task_id)
        manifest: Dict[str, Any] = {
            "schema_version": 1,
            "task_id": task_id,
            "attempt": attempt,
            "base_sha": active.base_sha,
            "source_head": source_head,
            "source_commits": source_commits,
            "payload_patch_sha256": _payload_sha256(
                active.path, active.base_sha, source_head
            ),
            "task_spec_blob": task_blob,
            "plan_blob": plan_blob,
            "gate_policy_sha256": canonical_sha256(self.contracts.gate_policy),
            "review_attestation": {
                "plan_sha256": review_plan.digest,
                "gate_attestations": [
                    dict(result.attestation) for result in review_result.results
                ],
            },
            "tdd_proof": tdd_proof,
            "reviewer": dict(self.reviewer),
            "created_at": self.clock(),
        }
        validate_submission(manifest, task_id)
        ref = "{}{}/{:06d}".format(self.prefix, task_id, attempt)
        if git_ops.ref_sha(self.root, ref) is not None:
            raise QueueError("{} submission ref already exists".format(task_id))
        commit = git_ops.commit_json(
            self.root,
            manifest,
            "{} immutable submission attempt {}".format(task_id, attempt),
            parent=source_head,
            filename="submission.json",
        )
        try:
            git_ops.update_ref_cas(self.root, ref, commit, None)
            if self.remote is not None:
                git_ops.push_ref_cas(self.root, self.remote, commit, ref, None)
        except git_ops.GitError as error:
            git_ops.run_git(self.root, "update-ref", "-d", ref, commit, check=False)
            raise QueueError(str(error)) from error
        try:
            self.states.transition(
                task_id,
                "active",
                "queued",
                "reviewed_submission",
                details={
                    "attempt": attempt,
                    "source_head": source_head,
                    "submission_sha256": canonical_sha256(manifest),
                },
                submission_ref=ref,
            )
        except state.StateError as error:
            raise QueueError(
                "submission ref was created but state transition failed: {}".format(
                    error
                )
            ) from error
        self.workspaces.release_queued_worktree(task_id)
        return Submission(task_id, ref, commit, manifest)

    def read(self, ref: str) -> Submission:
        return _load_submission(self.contracts, ref)


class MergeQueue:
    def __init__(
        self,
        contracts: ContractSet,
        states: state.StateStore,
        remote: Optional[str] = None,
    ) -> None:
        self.contracts = contracts
        self.states = states
        self.remote = remote
        self.root = contracts.root
        self.queue_prefix = contracts.manifest["ref_namespaces"]["queue"]

    def _read_submission(self, ref: str) -> Submission:
        if self.remote is not None:
            git_ops.fetch_immutable_ref(self.root, self.remote, ref)
        return _load_submission(self.contracts, ref)

    def entry_from_ref(self, task_id: str, queue_ref: str) -> TrainEntry:
        if not queue_ref.startswith(self.queue_prefix):
            raise QueueError("queue ref is outside the configured namespace")
        if self.remote is not None:
            git_ops.fetch_immutable_ref(self.root, self.remote, queue_ref)
        candidate = git_ops.ref_sha(self.root, queue_ref)
        if candidate is None:
            raise QueueError("queue ref is missing: {}".format(queue_ref))
        parents = git_ops.commit_parents(self.root, candidate)
        if len(parents) != 1:
            raise QueueError("queue candidate must have exactly one parent")
        snapshot = self.states.read(task_id)
        if snapshot.state != "queued" or snapshot.event is None:
            raise QueueError("{} is not queued".format(task_id))
        submission_ref = snapshot.event["submission_ref"]
        submission = self._read_submission(submission_ref)
        match = re.fullmatch(
            r"{}[a-z0-9][a-z0-9-]{{0,63}}/([0-9]{{3}})-{}".format(
                re.escape(self.queue_prefix), re.escape(task_id)
            ),
            queue_ref,
        )
        if match is None:
            raise QueueError("queue ref does not identify the requested task")
        index = int(match.group(1))
        return TrainEntry(
            index,
            task_id,
            parents[0],
            candidate,
            queue_ref,
            submission_ref,
            submission.manifest,
        )

    def build_train(
        self,
        task_ids: Sequence[str],
        train_id: str,
        base_sha: Optional[str] = None,
    ) -> MergeTrain:
        if re.fullmatch(r"[a-z0-9][a-z0-9-]{0,63}", train_id) is None:
            raise QueueError("train id is invalid")
        if not task_ids or len(task_ids) != len(set(task_ids)):
            raise QueueError("train task list is empty or duplicated")
        protected_ref = "refs/heads/{}".format(
            self.contracts.manifest["integration_branch"]
        )
        if self.remote is not None:
            remote_base = git_ops.fetch_remote_object(
                self.root, self.remote, protected_ref
            )
            if base_sha is not None and base_sha != remote_base:
                raise QueueError("requested train base is not the protected head")
            base = remote_base
        else:
            base = base_sha or git_ops.object_id(self.root, protected_ref)
        parent_dir = Path(tempfile.mkdtemp(prefix="xnn-train-"))
        worktree = parent_dir / "worktree"
        entries: List[TrainEntry] = []
        accepted_in_train: List[str] = []
        try:
            git_ops.run_git(
                self.root,
                "worktree",
                "add",
                "--detach",
                str(worktree),
                base,
            )
            for index, task_id in enumerate(task_ids, start=1):
                if self.remote is not None:
                    approval.require_task_plan(self.contracts, task_id, self.remote)
                snapshot = self.states.read(task_id)
                if snapshot.state != "queued" or snapshot.event is None:
                    raise QueueError("{} is not queued".format(task_id))
                submission_ref = snapshot.event["submission_ref"]
                submission = self._read_submission(submission_ref)
                task = self.contracts.tasks[task_id]
                for dependency in task["depends_on"]:
                    if dependency in self.contracts.legacy_accepted:
                        continue
                    dependency_state = self.states.read(dependency).state
                    if (
                        dependency_state != "done"
                        and dependency not in accepted_in_train
                    ):
                        raise QueueError(
                            "{} dependency {} is not published or earlier in train".format(
                                task_id, dependency
                            )
                        )
                current_parent = git_ops.object_id(worktree, "HEAD")
                patch = _payload_bytes(
                    self.root,
                    submission.manifest["base_sha"],
                    submission.manifest["source_head"],
                )
                if (
                    hashlib.sha256(patch).hexdigest()
                    != submission.manifest["payload_patch_sha256"]
                ):
                    raise QueueError(
                        "{} submission payload digest changed".format(task_id)
                    )
                apply_result = git_ops.run_git(
                    worktree,
                    "apply",
                    "--index",
                    "--whitespace=nowarn",
                    input_bytes=patch,
                    check=False,
                )
                if apply_result.returncode != 0:
                    raise QueueError(
                        "{} payload does not apply to train parent".format(task_id)
                    )
                staged = git_ops.git_text(
                    worktree, "diff", "--cached", "--name-only"
                ).splitlines()
                outside = [
                    path for path in staged if not _matches(path, task["owned_paths"])
                ]
                if outside:
                    raise QueueError(
                        "{} candidate contains paths outside ownership".format(task_id)
                    )
                delivery = task["delivery"]
                subject = "{}({}): {}".format(
                    delivery["commit_type"],
                    delivery["scope"],
                    delivery["summary"],
                )
                trailers = (
                    "Xnn-Task: {}\n"
                    "Xnn-Lifecycle: delivery\n"
                    "Xnn-Submission-SHA256: {}\n"
                    "Xnn-Payload-SHA256: {}"
                ).format(
                    task_id,
                    canonical_sha256(submission.manifest),
                    submission.manifest["payload_patch_sha256"],
                )
                result = git_ops.run_git(
                    worktree,
                    "commit",
                    "-m",
                    subject,
                    "-m",
                    trailers,
                    environment=_delivery_identity(self.root),
                    check=False,
                )
                if result.returncode != 0:
                    raise QueueError(
                        "{} candidate commit failed: {}".format(
                            task_id,
                            result.stderr.decode("utf-8", errors="replace").strip(),
                        )
                    )
                candidate = git_ops.object_id(worktree, "HEAD")
                candidate_digest = _payload_sha256(worktree, current_parent, candidate)
                if candidate_digest != submission.manifest["payload_patch_sha256"]:
                    raise QueueError(
                        "{} candidate patch differs from submission".format(task_id)
                    )
                queue_ref = "{}{}/{:03d}-{}".format(
                    self.queue_prefix, train_id, index, task_id
                )
                try:
                    git_ops.update_ref_cas(self.root, queue_ref, candidate, None)
                    if self.remote is not None:
                        git_ops.push_ref_cas(
                            self.root,
                            self.remote,
                            candidate,
                            queue_ref,
                            None,
                        )
                except git_ops.GitError as error:
                    raise QueueError(str(error)) from error
                entries.append(
                    TrainEntry(
                        index,
                        task_id,
                        current_parent,
                        candidate,
                        queue_ref,
                        submission_ref,
                        submission.manifest,
                    )
                )
                accepted_in_train.append(task_id)
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
            shutil.rmtree(parent_dir, ignore_errors=True)
        return MergeTrain(train_id, base, tuple(entries))

    def _archive_entry(self, entry: TrainEntry) -> str:
        archive_prefix = self.contracts.manifest["ref_namespaces"]["archive"]
        queue_suffix = entry.queue_ref[len(self.queue_prefix) :]
        archive_ref = "{}queue/{}".format(archive_prefix, queue_suffix)
        local_sha = git_ops.ref_sha(self.root, archive_ref)
        if local_sha is None:
            try:
                git_ops.update_ref_cas(
                    self.root, archive_ref, entry.candidate_sha, None
                )
            except git_ops.GitError as error:
                raise QueueError(str(error)) from error
        elif local_sha != entry.candidate_sha:
            raise QueueError("archive ref contains another candidate")
        if self.remote is not None:
            remote_sha = git_ops.remote_ref_sha(self.root, self.remote, archive_ref)
            if remote_sha is None:
                try:
                    git_ops.push_ref_cas(
                        self.root,
                        self.remote,
                        entry.candidate_sha,
                        archive_ref,
                        None,
                    )
                except git_ops.GitError as error:
                    raise QueueError(str(error)) from error
            elif remote_sha != entry.candidate_sha:
                raise QueueError("remote archive ref contains another candidate")
        return archive_ref

    def reopen(self, entry: TrainEntry, reason: str) -> Path:
        if not isinstance(reason, str) or not reason.strip():
            raise QueueError("queue rejection reason must not be empty")
        snapshot = self.states.read(entry.task_id)
        if (
            snapshot.state != "queued"
            or snapshot.event is None
            or snapshot.event["submission_ref"] != entry.submission_ref
        ):
            raise QueueError(
                "{} is not queued for this submission".format(entry.task_id)
            )
        archive_ref = self._archive_entry(entry)
        active_events = [
            event
            for event in self.states.history(entry.task_id)
            if event["to"] == "active"
        ]
        if not active_events:
            raise QueueError("{} has no claim workspace".format(entry.task_id))
        original = active_events[-1]["details"]
        required = {"base_sha", "branch", "worktree", "owner"}
        if not required.issubset(original):
            raise QueueError("{} claim event is incomplete".format(entry.task_id))
        path = Path(original["worktree"])
        if path.exists():
            raise QueueError("reopened worktree path already exists: {}".format(path))
        branch_ref = "refs/heads/{}".format(original["branch"])
        branch_sha = git_ops.ref_sha(self.root, branch_ref)
        source_head = str(entry.submission["source_head"])
        if branch_sha is None:
            try:
                git_ops.update_ref_cas(self.root, branch_ref, source_head, None)
            except git_ops.GitError as error:
                raise QueueError(str(error)) from error
        elif branch_sha != source_head:
            raise QueueError("task branch changed after immutable submission")
        result = git_ops.run_git(
            self.root,
            "worktree",
            "add",
            str(path),
            original["branch"],
            check=False,
        )
        if result.returncode != 0:
            raise QueueError("cannot restore queued task worktree")
        try:
            self.states.transition(
                entry.task_id,
                "queued",
                "active",
                "queue_rejected",
                details={
                    "base_sha": original["base_sha"],
                    "branch": original["branch"],
                    "worktree": str(path),
                    "owner": original["owner"],
                    "rejected_submission_ref": entry.submission_ref,
                    "rejected_queue_ref": entry.queue_ref,
                    "archive_ref": archive_ref,
                    "reason": reason.strip(),
                },
                submission_ref=entry.submission_ref,
            )
        except state.StateError as error:
            git_ops.run_git(
                self.root,
                "worktree",
                "remove",
                "--force",
                str(path),
                check=False,
            )
            raise QueueError(str(error)) from error
        return path


class Publisher:
    def __init__(
        self,
        contracts: ContractSet,
        states: state.StateStore,
        remote: str,
        repository: str,
        workflow_path: str,
        required_jobs: Sequence[str],
        required_artifacts: Sequence[str],
        actor: Mapping[str, Any],
        clock: Optional[Callable[[], str]] = None,
    ) -> None:
        self.contracts = contracts
        self.states = states
        self.remote = remote
        self.repository = repository
        self.workflow_path = workflow_path
        self.required_jobs = list(required_jobs)
        self.required_artifacts = list(required_artifacts)
        self.actor = _validate_actor(actor)
        self.clock = clock or _utc_now
        self.root = contracts.root
        self.acceptance = attestation.AcceptanceStore(contracts, remote=remote)

    def _protected_ref(self) -> str:
        return "refs/heads/{}".format(self.contracts.manifest["integration_branch"])

    def _workflow_blob(self, candidate_sha: str) -> str:
        return git_ops.object_id(
            self.root,
            "{}:{}".format(candidate_sha, self.workflow_path),
        )

    def _validate_entry(self, entry: TrainEntry) -> None:
        approval.require_task_plan(self.contracts, entry.task_id, self.remote)
        if git_ops.commit_parents(self.root, entry.candidate_sha) != [entry.parent_sha]:
            raise QueueError("queue candidate parent does not match train entry")
        payload_digest = _payload_sha256(
            self.root, entry.parent_sha, entry.candidate_sha
        )
        if payload_digest != entry.submission["payload_patch_sha256"]:
            raise QueueError("queue candidate payload differs from submission")
        changed = git_ops.changed_paths(
            self.root, entry.parent_sha, entry.candidate_sha
        )
        task = self.contracts.tasks[entry.task_id]
        outside = [path for path in changed if not _matches(path, task["owned_paths"])]
        if outside:
            raise QueueError(
                "queue candidate contains paths outside ownership: {}".format(
                    ", ".join(outside)
                )
            )
        trust_root = _trust_root_changes(changed)
        if trust_root:
            raise QueueError(
                "standard queue candidate changes its verification trust root: "
                + ", ".join(trust_root)
            )
        stored = _load_submission(self.contracts, entry.submission_ref)
        if stored.task_id != entry.task_id or stored.manifest != entry.submission:
            raise QueueError("train entry does not match its immutable submission")
        parent_workflow = self._workflow_blob(entry.parent_sha)
        candidate_workflow = self._workflow_blob(entry.candidate_sha)
        if candidate_workflow != parent_workflow:
            raise QueueError("candidate workflow is not the trusted parent workflow")
        if entry.index > 1:
            message = git_ops.git_text(
                self.root,
                "show",
                "-s",
                "--format=%B",
                entry.parent_sha,
            )
            parent_tasks = [
                line.removeprefix("Xnn-Task: ")
                for line in message.splitlines()
                if line.startswith("Xnn-Task: ")
            ]
            if len(parent_tasks) != 1 or parent_tasks[0] not in self.contracts.tasks:
                raise QueueError("train predecessor has invalid task provenance")
            predecessor = self.states.read(parent_tasks[0])
            if (
                predecessor.state != "done"
                or predecessor.event is None
                or predecessor.event["details"].get("published_sha") != entry.parent_sha
            ):
                raise QueueError(
                    "train predecessor {} is not durably done".format(parent_tasks[0])
                )

    def _validate_platform_coverage(
        self,
        entry: TrainEntry,
        workflow: Mapping[str, Any],
    ) -> None:
        required_platforms = set(
            task_required_platforms(self.contracts, [entry.task_id])
        )
        artifacts_by_platform: Dict[str, Mapping[str, Any]] = {}
        for artifact in workflow["artifacts"]:
            platform = artifact["platform"]
            if platform in artifacts_by_platform:
                raise QueueError(
                    "workflow has multiple evidence artifacts for {}".format(platform)
                )
            artifacts_by_platform[platform] = artifact
        missing_platforms = sorted(required_platforms - set(artifacts_by_platform))
        if missing_platforms:
            raise QueueError(
                "workflow lacks required platform evidence: {}".format(
                    ", ".join(missing_platforms)
                )
            )
        changed = git_ops.changed_paths(
            self.root, entry.parent_sha, entry.candidate_sha
        )
        queue_plan = plan_gates(self.contracts, entry.task_id, "queue", changed)
        for platform in sorted(required_platforms):
            expected = plan_for_platform(self.contracts, queue_plan, platform)
            missing_gates = sorted(
                set(expected.leaves) - set(artifacts_by_platform[platform]["gate_ids"])
            )
            if missing_gates:
                raise QueueError(
                    "{} evidence lacks required Gates: {}".format(
                        platform, ", ".join(missing_gates)
                    )
                )

    def _validate_stored_acceptance(
        self,
        entry: TrainEntry,
        value: Mapping[str, Any],
    ) -> None:
        expected = {
            "submission_sha256": canonical_sha256(entry.submission),
            "candidate_tree": git_ops.current_tree(self.root, entry.candidate_sha),
            "integration_base": entry.parent_sha,
            "payload_patch_sha256": entry.submission["payload_patch_sha256"],
        }
        for field, wanted in expected.items():
            if value.get(field) != wanted:
                raise QueueError(
                    "stored acceptance {} does not match candidate".format(field)
                )
        if value.get("required_jobs") != self.required_jobs:
            raise QueueError("stored acceptance required jobs changed")
        if value.get("required_artifacts") != self.required_artifacts:
            raise QueueError("stored acceptance required artifacts changed")
        queue_branch = entry.queue_ref[len("refs/heads/") :]
        normalized = attestation.validate_workflow_evidence(
            value["workflow"],
            repository=self.repository,
            workflow_path=self.workflow_path,
            workflow_blob=self._workflow_blob(entry.candidate_sha),
            candidate_sha=entry.candidate_sha,
            candidate_branch=queue_branch,
            required_jobs=self.required_jobs,
            required_artifacts=self.required_artifacts,
        )
        self._validate_platform_coverage(entry, normalized)
        attestation.validate_criterion_evidence(
            self.contracts,
            normalized,
            candidate_sha=entry.candidate_sha,
            required_artifacts=self.required_artifacts,
            criterion_ids=self.contracts.tasks[entry.task_id]["criteria"],
            gate_attestations=value["required_gate_attestations"],
            criterion_evidence=value["criterion_evidence"],
        )

    def publish(
        self,
        entry: TrainEntry,
        workflow_evidence: Mapping[str, Any],
        gate_attestations: Sequence[str],
        criterion_evidence: Sequence[str],
    ) -> Mapping[str, Any]:
        snapshot = self.states.read(entry.task_id)
        if snapshot.state != "queued":
            raise QueueError("{} is not queued".format(entry.task_id))
        self._validate_entry(entry)
        queue_branch = entry.queue_ref[len("refs/heads/") :]
        workflow_blob = self._workflow_blob(entry.candidate_sha)
        normalized = attestation.validate_workflow_evidence(
            workflow_evidence,
            repository=self.repository,
            workflow_path=self.workflow_path,
            workflow_blob=workflow_blob,
            candidate_sha=entry.candidate_sha,
            candidate_branch=queue_branch,
            required_jobs=self.required_jobs,
            required_artifacts=self.required_artifacts,
        )
        self._validate_platform_coverage(entry, normalized)
        attestation.validate_criterion_evidence(
            self.contracts,
            normalized,
            candidate_sha=entry.candidate_sha,
            required_artifacts=self.required_artifacts,
            criterion_ids=self.contracts.tasks[entry.task_id]["criteria"],
            gate_attestations=gate_attestations,
            criterion_evidence=criterion_evidence,
        )
        existing = self.acceptance.maybe_read(entry.task_id, entry.candidate_sha)
        if existing is not None:
            self._validate_stored_acceptance(entry, existing)
            acceptance_value = dict(existing)
            acceptance_commit = git_ops.ref_sha(
                self.root,
                self.acceptance.ref(entry.task_id, entry.candidate_sha),
            )
        else:
            acceptance_value = attestation.create_acceptance_attestation(
                task_id=entry.task_id,
                submission=entry.submission,
                candidate_sha=entry.candidate_sha,
                candidate_tree=git_ops.current_tree(self.root, entry.candidate_sha),
                integration_base=entry.parent_sha,
                payload_patch_sha256=entry.submission["payload_patch_sha256"],
                workflow=normalized,
                required_jobs=self.required_jobs,
                required_artifacts=self.required_artifacts,
                gate_attestations=gate_attestations,
                criterion_evidence=criterion_evidence,
                actor=self.actor,
                created_at=self.clock(),
            )
            acceptance_commit = self.acceptance.write(acceptance_value)
        if acceptance_commit is None:
            raise QueueError("acceptance attestation ref is missing")
        protected_ref = self._protected_ref()
        remote_head = git_ops.remote_ref_sha(self.root, self.remote, protected_ref)
        if remote_head != entry.parent_sha:
            raise QueueError(
                "protected branch moved: expected {}, found {}".format(
                    entry.parent_sha, remote_head
                )
            )
        try:
            git_ops.push_ref_cas(
                self.root,
                self.remote,
                entry.candidate_sha,
                protected_ref,
                entry.parent_sha,
            )
        except git_ops.GitError as error:
            raise QueueError(str(error)) from error
        published_head = git_ops.remote_ref_sha(self.root, self.remote, protected_ref)
        if published_head != entry.candidate_sha:
            raise PublicationRecoveryRequired(
                "protected branch changed immediately after publication"
            )
        try:
            self.states.transition(
                entry.task_id,
                "queued",
                "done",
                "published",
                details={
                    "acceptance_attestation_sha256": canonical_sha256(acceptance_value),
                    "acceptance_ref": self.acceptance.ref(
                        entry.task_id, entry.candidate_sha
                    ),
                    "acceptance_commit": acceptance_commit,
                    "published_sha": entry.candidate_sha,
                },
                submission_ref=entry.submission_ref,
            )
        except state.StateError as error:
            raise PublicationRecoveryRequired(
                "candidate published but state finalization failed: {}".format(error)
            ) from error
        return acceptance_value

    def recover(self, entry: TrainEntry) -> None:
        self._validate_entry(entry)
        protected_ref = self._protected_ref()
        remote_head = git_ops.remote_ref_sha(self.root, self.remote, protected_ref)
        if remote_head != entry.candidate_sha:
            raise QueueError("candidate is not present on protected branch")
        acceptance_value = self.acceptance.read(entry.task_id, entry.candidate_sha)
        self._validate_stored_acceptance(entry, acceptance_value)
        snapshot = self.states.read(entry.task_id)
        if snapshot.state == "done":
            return
        if snapshot.state != "queued":
            raise QueueError(
                "{} cannot recover from {}".format(entry.task_id, snapshot.state)
            )
        try:
            self.states.transition(
                entry.task_id,
                "queued",
                "done",
                "published",
                details={
                    "acceptance_attestation_sha256": canonical_sha256(acceptance_value),
                    "acceptance_ref": self.acceptance.ref(
                        entry.task_id, entry.candidate_sha
                    ),
                    "acceptance_commit": git_ops.ref_sha(
                        self.root,
                        self.acceptance.ref(entry.task_id, entry.candidate_sha),
                    ),
                    "published_sha": entry.candidate_sha,
                },
                submission_ref=entry.submission_ref,
            )
        except state.StateError as error:
            raise QueueError(str(error)) from error
