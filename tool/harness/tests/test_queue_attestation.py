#!/usr/bin/env python3
"""Tests for Harness V2 submissions, merge queue, and publication."""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import unittest
from pathlib import Path
from typing import Any, Dict, Optional
from unittest import mock

HARNESS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HARNESS))

import attestation  # noqa: E402
import git_ops  # noqa: E402
import merge_queue as queue_module  # noqa: E402
import model  # noqa: E402
import state  # noqa: E402
import tdd  # noqa: E402
import workspace  # noqa: E402
from test_gates_tdd import GateRepository  # noqa: E402
from test_model import task  # noqa: E402
from test_state_workspace import ACTOR  # noqa: E402


REVIEWER = {
    "kind": "user",
    "id": "reviewer@example.com",
    "name": "Independent Reviewer",
    "email": "reviewer@example.com",
}
QUEUE_ACTOR = {
    "kind": "queue-worker",
    "id": "queue@example.com",
    "name": "Queue Worker",
    "email": "queue@example.com",
}


class DeliveryFixture:
    def __init__(
        self,
        testcase: unittest.TestCase,
        with_remote: bool = False,
        multi_task: bool = False,
    ) -> None:
        self.repository = GateRepository(testcase)
        self.root = self.repository.root
        fingerprint = "FAILED: expected feature behavior is unavailable"
        code = (
            "from pathlib import Path; import sys; "
            "red=Path('native/tests/red.txt').exists(); "
            "green=Path('native/src/impl.txt').exists(); "
            "print({!r}) if red and not green else None; "
            "sys.exit(1 if red and not green else 0)"
        ).format(fingerprint)
        self.repository.configure_command(
            "feature_test", ["python3", "-c", code]
        )
        if multi_task:
            self.repository.fixture.plan["requirements"][0][
                "implementation_tasks"
            ].append("XT-103")
            self.repository.fixture.acceptance["depends_on"].append("XT-103")
            self.repository.fixture.plan["approval"][
                "content_sha256"
            ] = model.plan_content_sha256(self.repository.fixture.plan)
            documentation = task(
                "XT-103",
                "acceptance",
                [],
                ["docs/feature/**"],
            )
            documentation["type"] = "documentation"
            documentation["risk"] = {
                "functionality": "low",
                "security": "none",
                "performance": "none",
                "compatibility": "none",
                "concurrency": "none",
                "platform": "none",
                "persistence": "none",
            }
            documentation["tdd"] = {
                "mode": "not_required",
                "gate": None,
                "proof_paths": [],
                "oracle_paths": [],
                "failure_fingerprints": [],
            }
            documentation["delivery"] = {
                "commit_type": "docs",
                "scope": "feature",
                "summary": "document feature behavior",
                "architecture_change": {"mode": "none", "modules": []},
            }
            self.repository.fixture.write()
            (
                self.root / ".agents" / "tasks" / "XT-103.json"
            ).write_text(
                json.dumps(documentation, indent=2) + "\n",
                encoding="utf-8",
            )
        workflow = self.root / ".github" / "workflows"
        workflow.mkdir(parents=True)
        (workflow / "merge-queue.yml").write_text(
            "name: Merge Queue\non: push\n", encoding="utf-8"
        )
        self.base = self.repository.commit("chore: initialize")
        self.remote_path: Optional[Path] = None
        self.remote = None
        if with_remote:
            self.remote_path = Path(
                self.repository.fixture.temporary.name + "-remote.git"
            )
            testcase.addCleanup(
                lambda: shutil.rmtree(self.remote_path, ignore_errors=True)
            )
            subprocess.run(
                ["git", "init", "-q", "--bare", str(self.remote_path)],
                check=True,
            )
            self.repository.git("remote", "add", "origin", str(self.remote_path))
            self.repository.git("push", "-q", "-u", "origin", "harness")
            self.remote = "origin"
        self.contracts = self.repository.load()
        self.store = state.StateStore(
            self.contracts,
            remote=self.remote,
            actor=ACTOR,
            clock=lambda: "2026-08-12T12:00:00Z",
        )
        self.workspaces = workspace.WorkspaceManager(
            self.contracts, self.store
        )
        self.worktree = Path(
            self.repository.fixture.temporary.name + "-XT-101"
        )
        testcase.addCleanup(
            lambda: shutil.rmtree(self.worktree, ignore_errors=True)
        )
        self.workspaces.claim("XT-101", self.worktree)
        (self.worktree / "native" / "tests" / "red.txt").write_text(
            "red\n", encoding="utf-8"
        )
        self.red_sha = self.commit_worktree("test: expose missing feature")
        self.tdd = tdd.TddManager(
            self.contracts, self.store, self.workspaces
        )
        self.tdd.record_red("XT-101")
        (self.worktree / "native" / "src").mkdir()
        (self.worktree / "native" / "src" / "impl.txt").write_text(
            "green\n", encoding="utf-8"
        )
        self.green_sha = self.commit_worktree("feat: implement behavior")

    def commit_worktree(self, message: str) -> str:
        subprocess.run(
            ["git", "-C", str(self.worktree), "add", "."], check=True
        )
        subprocess.run(
            ["git", "-C", str(self.worktree), "commit", "-q", "-m", message],
            check=True,
        )
        return subprocess.run(
            ["git", "-C", str(self.worktree), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()

    def submit(self) -> queue_module.Submission:
        manager = queue_module.SubmissionManager(
            self.contracts,
            self.store,
            self.workspaces,
            REVIEWER,
            remote=self.remote,
            clock=lambda: "2026-08-12T12:30:00Z",
        )
        return manager.submit("XT-101", self.red_sha)

    def submit_documentation(self) -> queue_module.Submission:
        worktree = Path(
            self.repository.fixture.temporary.name + "-XT-103"
        )
        self.workspaces.claim("XT-103", worktree)
        (worktree / "docs" / "feature").mkdir(parents=True)
        (worktree / "docs" / "feature" / "behavior.md").write_text(
            "# Behavior\n", encoding="utf-8"
        )
        subprocess.run(
            ["git", "-C", str(worktree), "add", "."], check=True
        )
        subprocess.run(
            [
                "git",
                "-C",
                str(worktree),
                "commit",
                "-q",
                "-m",
                "docs: describe behavior",
            ],
            check=True,
        )
        manager = queue_module.SubmissionManager(
            self.contracts,
            self.store,
            self.workspaces,
            REVIEWER,
            remote=self.remote,
            clock=lambda: "2026-08-12T12:31:00Z",
        )
        return manager.submit("XT-103")

    def train(
        self, submission: queue_module.Submission
    ) -> queue_module.MergeTrain:
        queue = queue_module.MergeQueue(
            self.contracts, self.store, remote=self.remote
        )
        return queue.build_train(["XT-101"], "train-001", self.base)

    def evidence(
        self, entry: queue_module.TrainEntry
    ) -> Dict[str, Any]:
        workflow_blob = git_ops.object_id(
            self.root,
            "{}:.github/workflows/merge-queue.yml".format(
                entry.candidate_sha
            ),
        )
        return {
            "repository": "example/XnnTransfer",
            "workflow_path": ".github/workflows/merge-queue.yml",
            "workflow_blob": workflow_blob,
            "run_id": 12345,
            "run_attempt": 1,
            "head_sha": entry.candidate_sha,
            "head_branch": entry.queue_ref[len("refs/heads/") :],
            "event": "push",
            "conclusion": "success",
            "jobs": [
                {"name": "Harness V2", "conclusion": "success"},
                {"name": "Product gates", "conclusion": "success"},
            ],
            "artifacts": [
                {
                    "name": "candidate-evidence",
                    "source_sha": entry.candidate_sha,
                    "sha256": hashlib.sha256(b"artifact").hexdigest(),
                    "gate_attestations": ["a" * 64],
                    "criterion_evidence": ["b" * 64],
                }
            ],
        }


class SubmissionQueueTest(unittest.TestCase):
    def test_submission_is_immutable_and_releases_developer_worktree(self) -> None:
        fixture = DeliveryFixture(self)
        submission = fixture.submit()
        self.assertEqual(fixture.store.read("XT-101").state, "queued")
        self.assertFalse(fixture.worktree.exists())
        self.assertEqual(
            git_ops.ref_sha(fixture.root, submission.ref), submission.commit
        )
        self.assertEqual(
            submission.manifest["source_head"], fixture.green_sha
        )
        self.assertEqual(
            submission.manifest["reviewer"]["id"], REVIEWER["id"]
        )
        manager = queue_module.SubmissionManager(
            fixture.contracts,
            fixture.store,
            fixture.workspaces,
            REVIEWER,
        )
        with self.assertRaises(workspace.WorkspaceError):
            manager.submit("XT-101", fixture.red_sha)

    def test_high_risk_submission_rejects_owner_as_reviewer(self) -> None:
        fixture = DeliveryFixture(self)
        manager = queue_module.SubmissionManager(
            fixture.contracts,
            fixture.store,
            fixture.workspaces,
            ACTOR,
        )
        with self.assertRaisesRegex(queue_module.QueueError, "independent reviewer"):
            manager.submit("XT-101", fixture.red_sha)

    def test_train_candidate_matches_submission_payload(self) -> None:
        fixture = DeliveryFixture(self)
        submission = fixture.submit()
        train = fixture.train(submission)
        self.assertEqual(len(train.entries), 1)
        entry = train.entries[0]
        self.assertEqual(entry.parent_sha, fixture.base)
        self.assertEqual(
            git_ops.ref_sha(fixture.root, entry.queue_ref),
            entry.candidate_sha,
        )
        candidate_paths = git_ops.changed_paths(
            fixture.root, entry.parent_sha, entry.candidate_sha
        )
        self.assertEqual(
            sorted(candidate_paths),
            ["native/src/impl.txt", "native/tests/red.txt"],
        )
        self.assertNotIn(".agents/records/XT-101.json", candidate_paths)
        self.assertEqual(
            queue_module._payload_sha256(
                fixture.root, entry.parent_sha, entry.candidate_sha
            ),
            submission.manifest["payload_patch_sha256"],
        )

    def test_two_nonconflicting_submissions_form_cumulative_train(self) -> None:
        fixture = DeliveryFixture(self, multi_task=True)
        first = fixture.submit()
        second = fixture.submit_documentation()
        queue = queue_module.MergeQueue(
            fixture.contracts, fixture.store
        )
        train = queue.build_train(
            ["XT-101", "XT-103"], "train-002", fixture.base
        )
        self.assertEqual(len(train.entries), 2)
        self.assertEqual(train.entries[0].candidate_sha, train.entries[1].parent_sha)
        self.assertEqual(train.entries[0].submission_ref, first.ref)
        self.assertEqual(train.entries[1].submission_ref, second.ref)
        self.assertEqual(
            git_ops.changed_paths(
                fixture.root,
                train.entries[1].parent_sha,
                train.entries[1].candidate_sha,
            ),
            ["docs/feature/behavior.md"],
        )


class AttestationTest(unittest.TestCase):
    def test_rejects_wrong_sha_missing_job_and_stale_artifact(self) -> None:
        fixture = DeliveryFixture(self)
        entry = fixture.train(fixture.submit()).entries[0]
        evidence = fixture.evidence(entry)
        normalized = attestation.validate_workflow_evidence(
            evidence,
            repository="example/XnnTransfer",
            workflow_path=".github/workflows/merge-queue.yml",
            workflow_blob=evidence["workflow_blob"],
            candidate_sha=entry.candidate_sha,
            candidate_branch=entry.queue_ref[len("refs/heads/") :],
            required_jobs=["Harness V2", "Product gates"],
            required_artifacts=["candidate-evidence"],
        )
        self.assertEqual(normalized["head_sha"], entry.candidate_sha)

        wrong = dict(evidence)
        wrong["head_sha"] = "0" * 40
        with self.assertRaisesRegex(attestation.AttestationError, "head_sha"):
            attestation.validate_workflow_evidence(
                wrong,
                repository="example/XnnTransfer",
                workflow_path=".github/workflows/merge-queue.yml",
                workflow_blob=evidence["workflow_blob"],
                candidate_sha=entry.candidate_sha,
                candidate_branch=entry.queue_ref[len("refs/heads/") :],
                required_jobs=["Harness V2", "Product gates"],
                required_artifacts=["candidate-evidence"],
            )

        missing = dict(evidence)
        missing["jobs"] = [{"name": "Harness V2", "conclusion": "success"}]
        with self.assertRaisesRegex(attestation.AttestationError, "jobs are missing"):
            attestation.validate_workflow_evidence(
                missing,
                repository="example/XnnTransfer",
                workflow_path=".github/workflows/merge-queue.yml",
                workflow_blob=evidence["workflow_blob"],
                candidate_sha=entry.candidate_sha,
                candidate_branch=entry.queue_ref[len("refs/heads/") :],
                required_jobs=["Harness V2", "Product gates"],
                required_artifacts=["candidate-evidence"],
            )

        stale = dict(evidence)
        stale["artifacts"] = [dict(evidence["artifacts"][0])]
        stale["artifacts"][0]["source_sha"] = "f" * 40
        with self.assertRaisesRegex(attestation.AttestationError, "stale source"):
            attestation.validate_workflow_evidence(
                stale,
                repository="example/XnnTransfer",
                workflow_path=".github/workflows/merge-queue.yml",
                workflow_blob=evidence["workflow_blob"],
                candidate_sha=entry.candidate_sha,
                candidate_branch=entry.queue_ref[len("refs/heads/") :],
                required_jobs=["Harness V2", "Product gates"],
                required_artifacts=["candidate-evidence"],
            )

        skipped = dict(evidence)
        skipped["jobs"] = list(evidence["jobs"]) + [
            {"name": "Optional matrix", "conclusion": "skipped"}
        ]
        with self.assertRaisesRegex(attestation.AttestationError, "skipped jobs"):
            attestation.validate_workflow_evidence(
                skipped,
                repository="example/XnnTransfer",
                workflow_path=".github/workflows/merge-queue.yml",
                workflow_blob=evidence["workflow_blob"],
                candidate_sha=entry.candidate_sha,
                candidate_branch=entry.queue_ref[len("refs/heads/") :],
                required_jobs=["Harness V2", "Product gates"],
                required_artifacts=["candidate-evidence"],
            )


class PublisherTest(unittest.TestCase):
    def _publisher(
        self,
        fixture: DeliveryFixture,
        store: state.StateStore,
    ) -> queue_module.Publisher:
        return queue_module.Publisher(
            fixture.contracts,
            store,
            "origin",
            "example/XnnTransfer",
            ".github/workflows/merge-queue.yml",
            ["Harness V2", "Product gates"],
            ["candidate-evidence"],
            QUEUE_ACTOR,
            clock=lambda: "2026-08-12T13:00:00Z",
        )

    def test_publishes_exact_candidate_once_without_acceptance_commit(self) -> None:
        fixture = DeliveryFixture(self, with_remote=True)
        entry = fixture.train(fixture.submit()).entries[0]
        queue_store = state.StateStore(
            fixture.contracts,
            remote="origin",
            actor=QUEUE_ACTOR,
            clock=lambda: "2026-08-12T13:00:00Z",
        )
        acceptance_value = self._publisher(
            fixture, queue_store
        ).publish(
            entry,
            fixture.evidence(entry),
            ["a" * 64],
            ["b" * 64],
        )
        remote_head = git_ops.remote_ref_sha(
            fixture.root, "origin", "refs/heads/harness"
        )
        self.assertEqual(remote_head, entry.candidate_sha)
        self.assertEqual(queue_store.read("XT-101").state, "done")
        self.assertEqual(
            acceptance_value["candidate_sha"], entry.candidate_sha
        )
        self.assertEqual(
            git_ops.commit_parents(fixture.root, entry.candidate_sha),
            [entry.parent_sha],
        )

    def test_recovers_state_after_candidate_was_published(self) -> None:
        fixture = DeliveryFixture(self, with_remote=True)
        entry = fixture.train(fixture.submit()).entries[0]
        queue_store = state.StateStore(
            fixture.contracts,
            remote="origin",
            actor=QUEUE_ACTOR,
            clock=lambda: "2026-08-12T13:00:00Z",
        )
        publisher = self._publisher(fixture, queue_store)
        original_transition = queue_store.transition
        with mock.patch.object(
            queue_store,
            "transition",
            side_effect=state.StateError("injected finalization failure"),
        ):
            with self.assertRaises(queue_module.PublicationRecoveryRequired):
                publisher.publish(
                    entry,
                    fixture.evidence(entry),
                    ["a" * 64],
                    ["b" * 64],
                )
        self.assertEqual(
            git_ops.remote_ref_sha(
                fixture.root, "origin", "refs/heads/harness"
            ),
            entry.candidate_sha,
        )
        queue_store.transition = original_transition
        publisher.recover(entry)
        self.assertEqual(queue_store.read("XT-101").state, "done")

    def test_rejects_protected_branch_cas_race(self) -> None:
        fixture = DeliveryFixture(self, with_remote=True)
        entry = fixture.train(fixture.submit()).entries[0]
        tree = git_ops.current_tree(fixture.root, fixture.base)
        competing = git_ops.git_text(
            fixture.root,
            "commit-tree",
            tree,
            "-p",
            fixture.base,
            input_bytes=b"competing accepted commit\n",
        )
        git_ops.push_ref_cas(
            fixture.root,
            "origin",
            competing,
            "refs/heads/harness",
            fixture.base,
        )
        queue_store = state.StateStore(
            fixture.contracts,
            remote="origin",
            actor=QUEUE_ACTOR,
        )
        with self.assertRaisesRegex(queue_module.QueueError, "protected branch moved"):
            self._publisher(fixture, queue_store).publish(
                entry,
                fixture.evidence(entry),
                ["a" * 64],
                ["b" * 64],
            )
        self.assertEqual(queue_store.read("XT-101").state, "queued")


if __name__ == "__main__":
    unittest.main()
