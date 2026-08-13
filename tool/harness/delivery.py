#!/usr/bin/env python3
"""Temporary exact-candidate queue and publication for Harness V2."""

from __future__ import annotations

import fnmatch
import hashlib
import re
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple

import ci_validation
import git_ops
import github_evidence
import tdd
from executor import GateExecutor
from gates import global_gate_plan, plan_for_platform, plan_gates
from model import ContractSet, load_json, task_required_platforms
from module_inventory import ArchitectureDeclarationError, validate_task_architecture
from runtime import RuntimeView
from workspace import ClaimedWorkspace, WorkspaceManager


TRUST_ROOT_PATTERNS = (
    ".agents/manifest.json",
    ".agents/gates.json",
    ".agents/risk-routing.json",
    ".agents/schemas/**",
    ".agents/architecture/**",
    ".agents/project/**",
    ".agents/commit-identity.json",
    ".github/workflows/**",
    "tool/harness/**",
    "AGENTS.md",
    "Makefile",
)
BOOTSTRAP_JOBS = (
    "Candidate plan",
    "Harness V2",
    "Product gates (linux)",
    "Product gates (macos)",
    "Product gates (windows)",
    "Cutover security",
    "Candidate accepted",
)
BOOTSTRAP_ARTIFACTS = (
    "candidate-evidence-linux",
    "candidate-evidence-macos",
    "candidate-evidence-windows",
)


class DeliveryError(RuntimeError):
    """Raised when a candidate cannot be reviewed, queued, or published."""


class PublishedCleanupRequired(DeliveryError):
    """Raised when publication succeeded but transient cleanup did not."""


@dataclass(frozen=True)
class QueueEntry:
    index: int
    task_id: str
    parent_sha: str
    candidate_sha: str
    queue_ref: str


@dataclass(frozen=True)
class QueueTrain:
    train_id: str
    base_sha: str
    entries: Tuple[QueueEntry, ...]


def _matches(path: str, patterns: Sequence[str]) -> bool:
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def _payload_bytes(root: Path, base: str, head: str) -> bytes:
    value = git_ops.run_git(root, "diff", "--binary", base, head).stdout
    if not value:
        raise DeliveryError("candidate payload is empty")
    return value


def _payload_sha256(root: Path, base: str, head: str) -> str:
    return hashlib.sha256(_payload_bytes(root, base, head)).hexdigest()


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
        raise DeliveryError("repository commit identity policy is invalid")
    return {
        "GIT_AUTHOR_NAME": policy["name"],
        "GIT_AUTHOR_EMAIL": policy["email"],
        "GIT_COMMITTER_NAME": policy["name"],
        "GIT_COMMITTER_EMAIL": policy["email"],
    }


def _trailer(message: str, name: str) -> List[str]:
    prefix = "{}: ".format(name)
    return [
        line.removeprefix(prefix)
        for line in message.splitlines()
        if line.startswith(prefix)
    ]


def _delete_queue_ref(
    root: Path,
    remote: Optional[str],
    queue_prefix: str,
    ref: str,
    expected_sha: str,
) -> None:
    if not ref.startswith(queue_prefix):
        raise DeliveryError("queue ref is outside the transient namespace")
    try:
        if remote is not None:
            git_ops.delete_remote_ref_cas(root, remote, ref, expected_sha)
        git_ops.delete_ref_cas(root, ref, expected_sha)
    except git_ops.GitError as error:
        raise DeliveryError(str(error)) from error


class QueueManager:
    def __init__(
        self,
        contracts: ContractSet,
        runtime: RuntimeView,
        workspaces: WorkspaceManager,
        remote: Optional[str],
    ) -> None:
        self.contracts = contracts
        self.runtime = runtime
        self.workspaces = workspaces
        self.remote = remote
        self.root = contracts.root
        self.queue_prefix = contracts.manifest["queue_namespace"]

    def _review(
        self,
        task_id: str,
        red_sha: Optional[str],
    ) -> Tuple[ClaimedWorkspace, str]:
        active = self.workspaces.active_workspace(task_id)
        self.workspaces.require_fresh(task_id)
        if not git_ops.is_clean(active.path):
            raise DeliveryError("{} worktree must be clean".format(task_id))
        source_head = git_ops.object_id(active.path, "HEAD")
        if source_head == active.base_sha:
            raise DeliveryError("{} has no payload".format(task_id))
        task = self.contracts.tasks[task_id]
        changed = git_ops.changed_paths(active.path, active.base_sha, source_head)
        outside = [path for path in changed if not _matches(path, task["owned_paths"])]
        if outside:
            raise DeliveryError(
                "{} payload is outside owned paths: {}".format(
                    task_id,
                    ", ".join(outside),
                )
            )
        trust_root = sorted(
            path for path in changed if _matches(path, TRUST_ROOT_PATTERNS)
        )
        if trust_root:
            raise DeliveryError(
                "{} standard queue payload changes its verification trust root: "
                "{}".format(task_id, ", ".join(trust_root))
            )
        try:
            validate_task_architecture(active.path, task, changed)
        except ArchitectureDeclarationError as error:
            raise DeliveryError(str(error)) from error
        if task["tdd"]["mode"] in tdd.RED_MODES:
            if red_sha is None:
                raise DeliveryError("{} submission requires Red SHA".format(task_id))
            tdd.TddManager(self.contracts, self.workspaces).review_green(
                task_id,
                red_sha,
            )
        review_plan = plan_gates(self.contracts, task_id, "review", changed)
        review = GateExecutor(
            self.contracts,
            execution_root=active.path,
        ).execute(review_plan)
        review.require_success()
        return active, source_head

    def build_train(
        self,
        task_ids: Sequence[str],
        train_id: str,
        red_shas: Optional[Mapping[str, str]] = None,
        base_sha: Optional[str] = None,
    ) -> QueueTrain:
        if re.fullmatch(r"[a-z0-9][a-z0-9-]{0,63}", train_id) is None:
            raise DeliveryError("train id is invalid")
        if not task_ids or len(task_ids) != len(set(task_ids)):
            raise DeliveryError("train task list is empty or duplicated")
        unknown = sorted(set(task_ids) - set(self.contracts.tasks))
        if unknown:
            raise DeliveryError("unknown tasks: {}".format(", ".join(unknown)))
        protected = self.runtime.integration_sha()
        if base_sha is not None and base_sha != protected:
            raise DeliveryError("requested train base is not the protected head")
        snapshots = self.runtime.list()
        accepted_in_train: List[str] = []
        reviewed: Dict[str, Tuple[ClaimedWorkspace, str]] = {}
        for task_id in task_ids:
            if snapshots[task_id].state != "active":
                raise DeliveryError("{} is not active".format(task_id))
            for dependency in self.contracts.tasks[task_id]["depends_on"]:
                if (
                    snapshots[dependency].state != "done"
                    and dependency not in accepted_in_train
                ):
                    raise DeliveryError(
                        "{} dependency {} is not published or earlier in train".format(
                            task_id,
                            dependency,
                        )
                    )
            reviewed[task_id] = self._review(
                task_id,
                (red_shas or {}).get(task_id),
            )
            accepted_in_train.append(task_id)

        parent_dir = Path(tempfile.mkdtemp(prefix="xnn-train-"))
        candidate_worktree = parent_dir / "worktree"
        entries: List[QueueEntry] = []
        try:
            git_ops.run_git(
                self.root,
                "worktree",
                "add",
                "--detach",
                str(candidate_worktree),
                protected,
            )
            for index, task_id in enumerate(task_ids, start=1):
                active, source_head = reviewed[task_id]
                current_parent = git_ops.object_id(candidate_worktree, "HEAD")
                patch = _payload_bytes(active.path, active.base_sha, source_head)
                applied = git_ops.run_git(
                    candidate_worktree,
                    "apply",
                    "--index",
                    "--whitespace=nowarn",
                    input_bytes=patch,
                    check=False,
                )
                if applied.returncode != 0:
                    raise DeliveryError(
                        "{} payload does not apply to train parent".format(task_id)
                    )
                staged = git_ops.git_text(
                    candidate_worktree,
                    "diff",
                    "--cached",
                    "--name-only",
                ).splitlines()
                task = self.contracts.tasks[task_id]
                outside = [
                    path for path in staged if not _matches(path, task["owned_paths"])
                ]
                if outside:
                    raise DeliveryError(
                        "{} candidate contains paths outside ownership".format(task_id)
                    )
                delivery = task["delivery"]
                subject = "{}({}): {}".format(
                    delivery["commit_type"],
                    delivery["scope"],
                    delivery["summary"],
                )
                payload_sha = hashlib.sha256(patch).hexdigest()
                trailers = (
                    "Xnn-Task: {}\n"
                    "Xnn-Lifecycle: delivery\n"
                    "Xnn-Payload-SHA256: {}"
                ).format(task_id, payload_sha)
                committed = git_ops.run_git(
                    candidate_worktree,
                    "commit",
                    "-m",
                    subject,
                    "-m",
                    trailers,
                    environment=_delivery_identity(self.root),
                    check=False,
                )
                if committed.returncode != 0:
                    diagnostic = committed.stderr.decode(
                        "utf-8",
                        errors="replace",
                    ).strip()
                    raise DeliveryError(
                        "{} candidate commit failed: {}".format(task_id, diagnostic)
                    )
                candidate = git_ops.object_id(candidate_worktree, "HEAD")
                if (
                    _payload_sha256(
                        candidate_worktree,
                        current_parent,
                        candidate,
                    )
                    != payload_sha
                ):
                    raise DeliveryError(
                        "{} candidate differs from reviewed payload".format(task_id)
                    )
                queue_ref = "{}{}/{:03d}-{}".format(
                    self.queue_prefix,
                    train_id,
                    index,
                    task_id,
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
                    git_ops.run_git(
                        self.root,
                        "update-ref",
                        "-d",
                        queue_ref,
                        candidate,
                        check=False,
                    )
                    raise DeliveryError(str(error)) from error
                entries.append(
                    QueueEntry(
                        index=index,
                        task_id=task_id,
                        parent_sha=current_parent,
                        candidate_sha=candidate,
                        queue_ref=queue_ref,
                    )
                )
            for task_id in task_ids:
                active, source_head = reviewed[task_id]
                self.workspaces.release(active, source_head)
        except Exception:
            for entry in reversed(entries):
                try:
                    _delete_queue_ref(
                        self.root,
                        self.remote,
                        self.queue_prefix,
                        entry.queue_ref,
                        entry.candidate_sha,
                    )
                except DeliveryError:
                    pass
            raise
        finally:
            if candidate_worktree.exists():
                git_ops.run_git(
                    self.root,
                    "worktree",
                    "remove",
                    "--force",
                    str(candidate_worktree),
                    check=False,
                )
            shutil.rmtree(parent_dir, ignore_errors=True)
        return QueueTrain(train_id, protected, tuple(entries))

    def entry_from_ref(self, task_id: str, queue_ref: str) -> QueueEntry:
        pattern = r"{}[a-z0-9][a-z0-9-]{{0,63}}/([0-9]{{3}})-{}".format(
            re.escape(self.queue_prefix),
            re.escape(task_id),
        )
        match = re.fullmatch(pattern, queue_ref)
        if match is None:
            raise DeliveryError("queue ref does not identify the requested task")
        try:
            if self.remote is None:
                candidate = git_ops.ref_sha(self.root, queue_ref)
            else:
                candidate = git_ops.fetch_remote_object(
                    self.root,
                    self.remote,
                    queue_ref,
                )
        except git_ops.GitError as error:
            raise DeliveryError(str(error)) from error
        if candidate is None:
            raise DeliveryError("queue ref is missing: {}".format(queue_ref))
        parents = git_ops.commit_parents(self.root, candidate)
        if len(parents) != 1:
            raise DeliveryError("queue candidate must have exactly one parent")
        message = git_ops.git_text(
            self.root,
            "show",
            "-s",
            "--format=%B",
            candidate,
        )
        if _trailer(message, "Xnn-Task") != [task_id]:
            raise DeliveryError("queue candidate has invalid task provenance")
        if _trailer(message, "Xnn-Lifecycle") != ["delivery"]:
            raise DeliveryError("queue candidate has invalid lifecycle provenance")
        payload = _trailer(message, "Xnn-Payload-SHA256")
        if payload != [_payload_sha256(self.root, parents[0], candidate)]:
            raise DeliveryError("queue candidate payload digest is invalid")
        return QueueEntry(
            index=int(match.group(1)),
            task_id=task_id,
            parent_sha=parents[0],
            candidate_sha=candidate,
            queue_ref=queue_ref,
        )

    def drop(self, entry: QueueEntry) -> None:
        _delete_queue_ref(
            self.root,
            self.remote,
            self.queue_prefix,
            entry.queue_ref,
            entry.candidate_sha,
        )

    def reopen(self, entry: QueueEntry) -> Path:
        if self.runtime.read(entry.task_id).state != "queued":
            raise DeliveryError("{} is not queued".format(entry.task_id))
        branch = "work/{}".format(entry.task_id)
        branch_ref = "refs/heads/{}".format(branch)
        if git_ops.ref_sha(self.root, branch_ref) is not None:
            raise DeliveryError("{} work branch already exists".format(entry.task_id))
        common = git_ops.common_git_dir(self.root)
        primary = common.parent
        path = primary.parent / "{}-{}".format(primary.name, entry.task_id)
        if path.exists():
            raise DeliveryError("worktree path already exists: {}".format(path))
        base = self.runtime.integration_sha()
        try:
            git_ops.add_worktree(self.root, path, branch, base)
            patch = _payload_bytes(
                self.root,
                entry.parent_sha,
                entry.candidate_sha,
            )
            applied = git_ops.run_git(
                path,
                "apply",
                "--index",
                "--whitespace=nowarn",
                input_bytes=patch,
                check=False,
            )
            if applied.returncode != 0:
                raise DeliveryError("candidate payload cannot be reopened")
            committed = git_ops.run_git(
                path,
                "commit",
                "-m",
                "chore(harness): reopen {}".format(entry.task_id.lower()),
                check=False,
            )
            if committed.returncode != 0:
                raise DeliveryError("reopened payload cannot be committed")
            self.drop(entry)
        except (DeliveryError, git_ops.GitError):
            if path.exists():
                git_ops.run_git(
                    self.root,
                    "worktree",
                    "remove",
                    "--force",
                    str(path),
                    check=False,
                )
            branch_sha = git_ops.ref_sha(self.root, branch_ref)
            if branch_sha is not None:
                git_ops.run_git(
                    self.root,
                    "update-ref",
                    "-d",
                    branch_ref,
                    branch_sha,
                    check=False,
                )
            raise
        return path


class Publisher:
    def __init__(
        self,
        contracts: ContractSet,
        runtime: RuntimeView,
        remote: str,
        repository: str,
        workflow_path: str,
    ) -> None:
        self.contracts = contracts
        self.runtime = runtime
        self.remote = remote
        self.repository = repository
        self.workflow_path = workflow_path
        self.root = contracts.root
        self.queue_prefix = contracts.manifest["queue_namespace"]

    def _workflow_blob(self, candidate_sha: str) -> str:
        return git_ops.object_id(
            self.root,
            "{}:{}".format(candidate_sha, self.workflow_path),
        )

    def _required_jobs(self, task_ids: Sequence[str]) -> List[str]:
        return [
            "Candidate plan",
            "Harness V2",
            *(
                "Product gates ({})".format(platform)
                for platform in task_required_platforms(self.contracts, task_ids)
            ),
            "Candidate accepted",
        ]

    def _required_artifacts(self, task_ids: Sequence[str]) -> List[str]:
        return [
            "candidate-evidence-{}".format(platform)
            for platform in task_required_platforms(self.contracts, task_ids)
        ]

    def _candidate_task_ids(self, candidate: str) -> List[str]:
        protected = self.runtime.integration_sha()
        if candidate == protected or not git_ops.is_ancestor(
            self.root,
            protected,
            candidate,
        ):
            raise DeliveryError("candidate is not ahead of the integration branch")
        result: List[str] = []
        for commit in git_ops.commit_range(self.root, protected, candidate):
            message = git_ops.git_text(
                self.root,
                "show",
                "-s",
                "--format=%B",
                commit,
            )
            tasks = _trailer(message, "Xnn-Task")
            if len(tasks) != 1 or tasks[0] not in self.contracts.tasks:
                raise DeliveryError("candidate has invalid task provenance")
            result.append(tasks[0])
        return result

    def _validate_entry(self, entry: QueueEntry) -> None:
        if git_ops.commit_parents(self.root, entry.candidate_sha) != [entry.parent_sha]:
            raise DeliveryError("queue candidate parent changed")
        changed = git_ops.changed_paths(
            self.root,
            entry.parent_sha,
            entry.candidate_sha,
        )
        task = self.contracts.tasks[entry.task_id]
        outside = [path for path in changed if not _matches(path, task["owned_paths"])]
        if outside:
            raise DeliveryError(
                "queue candidate contains paths outside ownership: {}".format(
                    ", ".join(outside)
                )
            )
        trust_root = sorted(
            path for path in changed if _matches(path, TRUST_ROOT_PATTERNS)
        )
        if trust_root:
            raise DeliveryError(
                "standard queue candidate changes its verification trust root: "
                + ", ".join(trust_root)
            )
        if self._workflow_blob(entry.parent_sha) != self._workflow_blob(
            entry.candidate_sha
        ):
            raise DeliveryError("candidate workflow differs from its trusted parent")

    def publish(self, entry: QueueEntry) -> Mapping[str, Any]:
        self._validate_entry(entry)
        protected_ref = "refs/heads/{}".format(
            self.contracts.manifest["integration_branch"]
        )
        remote_head = git_ops.remote_ref_sha(self.root, self.remote, protected_ref)
        if remote_head != entry.parent_sha:
            raise DeliveryError(
                "protected branch moved: expected {}, found {}".format(
                    entry.parent_sha,
                    remote_head or "<missing>",
                )
            )
        task_ids = self._candidate_task_ids(entry.candidate_sha)
        required_jobs = self._required_jobs(task_ids)
        required_artifacts = self._required_artifacts(task_ids)
        client = github_evidence.GitHubClient(
            self.repository,
            github_evidence.credential_token(self.root),
        )
        branch = entry.queue_ref[len("refs/heads/") :]
        raw = github_evidence.collect_workflow_evidence(
            client,
            workflow_path=self.workflow_path,
            workflow_blob=self._workflow_blob(entry.candidate_sha),
            branch=branch,
            candidate_sha=entry.candidate_sha,
            required_artifacts=required_artifacts,
        )
        workflow = ci_validation.validate_workflow_result(
            raw,
            repository=self.repository,
            workflow_path=self.workflow_path,
            workflow_blob=self._workflow_blob(entry.candidate_sha),
            candidate_sha=entry.candidate_sha,
            candidate_branch=branch,
            required_jobs=required_jobs,
            required_artifacts=required_artifacts,
        )
        artifacts = {artifact["name"]: artifact for artifact in workflow["artifacts"]}
        required_platforms = set(task_required_platforms(self.contracts, task_ids))
        actual_platforms = {artifacts[name]["platform"] for name in required_artifacts}
        if actual_platforms != required_platforms:
            raise DeliveryError("workflow platform evidence is incomplete")
        changed = git_ops.changed_paths(
            self.root,
            self.runtime.integration_sha(),
            entry.candidate_sha,
        )
        for task_id in task_ids:
            queue_plan = plan_gates(
                self.contracts,
                task_id,
                "queue",
                changed,
            )
            for platform in required_platforms:
                expected = plan_for_platform(
                    self.contracts,
                    queue_plan,
                    platform,
                )
                artifact = next(
                    value
                    for value in artifacts.values()
                    if value["platform"] == platform
                )
                missing = sorted(set(expected.leaves) - set(artifact["gate_ids"]))
                if missing:
                    raise DeliveryError(
                        "{} evidence lacks required Gates: {}".format(
                            platform,
                            ", ".join(missing),
                        )
                    )
        criteria = sorted(
            {
                criterion
                for task_id in task_ids
                for criterion in self.contracts.tasks[task_id]["criteria"]
            }
        )
        ci_validation.validate_criterion_results(
            self.contracts,
            workflow,
            candidate_sha=entry.candidate_sha,
            required_artifacts=required_artifacts,
            criterion_ids=criteria,
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
            raise DeliveryError(str(error)) from error
        if (
            git_ops.remote_ref_sha(self.root, self.remote, protected_ref)
            != entry.candidate_sha
        ):
            raise PublishedCleanupRequired(
                "protected branch changed immediately after publication"
            )
        try:
            _delete_queue_ref(
                self.root,
                self.remote,
                self.queue_prefix,
                entry.queue_ref,
                entry.candidate_sha,
            )
        except DeliveryError as error:
            raise PublishedCleanupRequired(
                "candidate published but queue cleanup failed: {}".format(error)
            ) from error
        return {
            "candidate_sha": entry.candidate_sha,
            "run_id": workflow["run_id"],
            "run_attempt": workflow["run_attempt"],
            "result": "published",
        }

    def recover_cleanup(self, entry: QueueEntry) -> None:
        protected_ref = "refs/heads/{}".format(
            self.contracts.manifest["integration_branch"]
        )
        protected = git_ops.remote_ref_sha(self.root, self.remote, protected_ref)
        if protected is None or not git_ops.is_ancestor(
            self.root,
            entry.candidate_sha,
            protected,
        ):
            raise DeliveryError("candidate is not accepted on the protected branch")
        _delete_queue_ref(
            self.root,
            self.remote,
            self.queue_prefix,
            entry.queue_ref,
            entry.candidate_sha,
        )

    def publish_bootstrap(self, queue_ref: str) -> Mapping[str, Any]:
        prefix = "{}bootstrap/".format(self.queue_prefix)
        if not queue_ref.startswith(prefix):
            raise DeliveryError(
                "bootstrap queue ref is outside the transient namespace"
            )
        candidate = git_ops.fetch_remote_object(self.root, self.remote, queue_ref)
        parents = git_ops.commit_parents(self.root, candidate)
        if not parents:
            raise DeliveryError("bootstrap candidate has no integration parent")
        protected_ref = "refs/heads/{}".format(
            self.contracts.manifest["integration_branch"]
        )
        protected = git_ops.remote_ref_sha(self.root, self.remote, protected_ref)
        if protected != parents[0]:
            raise DeliveryError("bootstrap candidate is not based on protected head")
        tree = git_ops.current_tree(self.root, candidate)
        workflow_blob = self._workflow_blob(candidate)
        branch = queue_ref[len("refs/heads/") :]
        client = github_evidence.GitHubClient(
            self.repository,
            github_evidence.credential_token(self.root),
        )
        raw = github_evidence.collect_bootstrap_workflow_evidence(
            client,
            workflow_path=self.workflow_path,
            workflow_blob=workflow_blob,
            branch=branch,
            candidate_sha=candidate,
            candidate_tree=tree,
            required_artifacts=BOOTSTRAP_ARTIFACTS,
        )
        global_plan = global_gate_plan(self.contracts)
        expected_gates = {
            platform: plan_for_platform(
                self.contracts,
                global_plan,
                platform,
            ).leaves
            for platform in ("linux", "macos", "windows")
        }
        workflow = ci_validation.validate_bootstrap_workflow_result(
            raw,
            repository=self.repository,
            workflow_path=self.workflow_path,
            workflow_blob=workflow_blob,
            candidate_sha=candidate,
            candidate_tree=tree,
            candidate_branch=branch,
            required_jobs=BOOTSTRAP_JOBS,
            required_artifacts=BOOTSTRAP_ARTIFACTS,
            expected_plan_sha256=global_plan.digest,
            expected_gates=expected_gates,
        )
        try:
            git_ops.push_ref_cas(
                self.root,
                self.remote,
                candidate,
                protected_ref,
                protected,
            )
        except git_ops.GitError as error:
            raise DeliveryError(str(error)) from error
        try:
            _delete_queue_ref(
                self.root,
                self.remote,
                self.queue_prefix,
                queue_ref,
                candidate,
            )
        except DeliveryError as error:
            raise PublishedCleanupRequired(
                "bootstrap published but queue cleanup failed: {}".format(error)
            ) from error
        return {
            "candidate_sha": candidate,
            "run_id": workflow["run_id"],
            "run_attempt": workflow["run_attempt"],
            "result": "published",
        }
