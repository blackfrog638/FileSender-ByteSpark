#!/usr/bin/env python3
"""Tests for Harness V2 submissions, merge queue, and publication."""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import unittest
from dataclasses import replace
from pathlib import Path
from typing import Any, Dict, Optional
from unittest import mock

HARNESS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HARNESS))

import attestation  # noqa: E402
import approval  # noqa: E402
import closure  # noqa: E402
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
        self.repository.configure_command("feature_test", ["python3", "-c", code])
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
                "architecture_change": {
                    "mode": "none",
                    "modules": [],
                    "supersedes": {
                        "paths": [],
                        "symbols": [],
                        "targets": [],
                    },
                    "temporary_leases": [],
                    "retires_leases": [],
                },
            }
            self.repository.fixture.write()
            (self.root / ".agents" / "tasks" / "XT-103.json").write_text(
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
            approval.ApprovalStore(
                self.root,
                self.repository.fixture.manifest,
                self.remote,
            ).write(
                self.repository.fixture.plan,
                "2026-08-12T11:00:00Z",
            )
        self.contracts = self.repository.load()
        self.store = state.StateStore(
            self.contracts,
            remote=self.remote,
            actor=ACTOR,
            clock=lambda: "2026-08-12T12:00:00Z",
        )
        self.workspaces = workspace.WorkspaceManager(self.contracts, self.store)
        self.worktree = Path(self.repository.fixture.temporary.name + "-XT-101")
        testcase.addCleanup(lambda: shutil.rmtree(self.worktree, ignore_errors=True))
        self.workspaces.claim("XT-101", self.worktree)
        (self.worktree / "native" / "tests" / "red.txt").write_text(
            "red\n", encoding="utf-8"
        )
        self.red_sha = self.commit_worktree("test: expose missing feature")
        self.tdd = tdd.TddManager(self.contracts, self.store, self.workspaces)
        self.tdd.record_red("XT-101")
        (self.worktree / "native" / "src").mkdir()
        (self.worktree / "native" / "src" / "impl.txt").write_text(
            "green\n", encoding="utf-8"
        )
        self.green_sha = self.commit_worktree("feat: implement behavior")

    def commit_worktree(self, message: str) -> str:
        subprocess.run(["git", "-C", str(self.worktree), "add", "."], check=True)
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
        worktree = Path(self.repository.fixture.temporary.name + "-XT-103")
        self.workspaces.claim("XT-103", worktree)
        (worktree / "docs" / "feature").mkdir(parents=True)
        (worktree / "docs" / "feature" / "behavior.md").write_text(
            "# Behavior\n", encoding="utf-8"
        )
        subprocess.run(["git", "-C", str(worktree), "add", "."], check=True)
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

    def train(self, submission: queue_module.Submission) -> queue_module.MergeTrain:
        queue = queue_module.MergeQueue(self.contracts, self.store, remote=self.remote)
        return queue.build_train(["XT-101"], "train-001", self.base)

    def evidence(self, entry: queue_module.TrainEntry) -> Dict[str, Any]:
        gate_ids = ["feature_test", "governance"]
        gate_attestations = ["a" * 64, "e" * 64]
        criterion_evidence = [
            attestation.criterion_evidence_digest(
                self.contracts,
                criterion_id,
                entry.candidate_sha,
                gate_attestations,
            )
            for criterion_id in self.contracts.tasks[entry.task_id]["criteria"]
        ]
        workflow_blob = git_ops.object_id(
            self.root,
            "{}:.github/workflows/merge-queue.yml".format(entry.candidate_sha),
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
                    "platform": "linux",
                    "gate_ids": gate_ids,
                    "gate_attestations": gate_attestations,
                    "criterion_ids": self.contracts.tasks[entry.task_id]["criteria"],
                    "criterion_evidence": criterion_evidence,
                }
            ],
        }


class SubmissionQueueTest(unittest.TestCase):
    def test_submission_is_immutable_and_releases_developer_worktree(self) -> None:
        fixture = DeliveryFixture(self)
        submission = fixture.submit()
        self.assertEqual(fixture.store.read("XT-101").state, "queued")
        self.assertFalse(fixture.worktree.exists())
        self.assertIsNone(git_ops.ref_sha(fixture.root, "refs/heads/work/XT-101"))
        self.assertEqual(
            git_ops.ref_sha(fixture.root, submission.ref), submission.commit
        )
        self.assertEqual(submission.manifest["source_head"], fixture.green_sha)
        self.assertEqual(
            git_ops.commit_parents(fixture.root, submission.commit),
            [fixture.green_sha],
        )
        self.assertEqual(submission.manifest["reviewer"]["id"], REVIEWER["id"])
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

    def test_rejects_changes_to_active_task_contract(self) -> None:
        fixture = DeliveryFixture(self)
        manager = queue_module.SubmissionManager(
            fixture.contracts,
            fixture.store,
            fixture.workspaces,
            REVIEWER,
        )
        with self.assertRaisesRegex(queue_module.QueueError, "while it is active"):
            manager._reject_inflight_contract_changes(
                "XT-101", [".agents/tasks/XT-101.json"]
            )

    def test_remote_submission_preserves_source_objects_for_queue_worker(
        self,
    ) -> None:
        fixture = DeliveryFixture(self, with_remote=True)
        submission = fixture.submit()
        worker = fixture.repository.external / "queue-worker"
        git_ops.run_git(worker.parent, "init", "-q", str(worker))
        git_ops.run_git(
            worker,
            "remote",
            "add",
            "origin",
            str(fixture.remote_path),
        )
        git_ops.run_git(
            worker,
            "fetch",
            "origin",
            "{}:{}".format(submission.ref, submission.ref),
        )
        self.assertEqual(
            git_ops.object_id(worker, "{}^".format(submission.ref)),
            submission.manifest["source_head"],
        )
        self.assertEqual(
            git_ops.object_id(worker, submission.manifest["source_head"]),
            fixture.green_sha,
        )

    def test_fresh_queue_worker_fetches_state_submission_and_source(
        self,
    ) -> None:
        fixture = DeliveryFixture(self, with_remote=True)
        fixture.submit()
        worker = fixture.repository.external / "fresh-queue-worker"
        subprocess.run(
            [
                "git",
                "clone",
                "-q",
                "--branch",
                "harness",
                str(fixture.remote_path),
                str(worker),
            ],
            check=True,
        )
        git_ops.run_git(worker, "config", "user.name", "Queue Worker")
        git_ops.run_git(worker, "config", "user.email", "queue@example.com")
        contracts = model.load_contracts(worker)
        store = state.StateStore(
            contracts,
            remote="origin",
            actor=QUEUE_ACTOR,
        )
        train = queue_module.MergeQueue(contracts, store, remote="origin").build_train(
            ["XT-101"], "fresh-worker"
        )
        self.assertEqual(len(train.entries), 1)
        self.assertEqual(train.entries[0].task_id, "XT-101")
        self.assertEqual(
            git_ops.remote_ref_sha(worker, "origin", train.entries[0].queue_ref),
            train.entries[0].candidate_sha,
        )

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
        self.assertEqual(
            git_ops.git_text(
                fixture.root,
                "show",
                "-s",
                "--format=%an <%ae>|%cn <%ce>",
                entry.candidate_sha,
            ),
            "Project Owner <owner@example.com>|" "Project Owner <owner@example.com>",
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

    def test_reopens_failed_queue_candidate_from_archived_source(self) -> None:
        fixture = DeliveryFixture(self, with_remote=True)
        entry = fixture.train(fixture.submit()).entries[0]
        queue = queue_module.MergeQueue(
            fixture.contracts, fixture.store, remote="origin"
        )
        path = queue.reopen(entry, "exact candidate CI failed")
        self.assertEqual(path.resolve(), fixture.worktree.resolve())
        self.assertTrue(path.is_dir())
        self.assertEqual(
            git_ops.object_id(path, "HEAD"),
            entry.submission["source_head"],
        )
        snapshot = fixture.store.read("XT-101")
        self.assertEqual(snapshot.state, "active")
        archive_ref = snapshot.event["details"]["archive_ref"]
        self.assertEqual(
            git_ops.remote_ref_sha(fixture.root, "origin", archive_ref),
            entry.candidate_sha,
        )
        self.assertIsNone(
            git_ops.remote_ref_sha(fixture.root, "origin", entry.queue_ref)
        )
        self.assertIsNone(git_ops.ref_sha(fixture.root, entry.queue_ref))

    def test_two_nonconflicting_submissions_form_cumulative_train(self) -> None:
        fixture = DeliveryFixture(self, multi_task=True)
        first = fixture.submit()
        second = fixture.submit_documentation()
        queue = queue_module.MergeQueue(fixture.contracts, fixture.store)
        train = queue.build_train(["XT-101", "XT-103"], "train-002", fixture.base)
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
        created_at: str = "2026-08-12T13:00:00Z",
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
            clock=lambda: created_at,
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
        acceptance_value = self._publisher(fixture, queue_store).publish(
            entry,
            fixture.evidence(entry),
            ["a" * 64, "e" * 64],
            fixture.evidence(entry)["artifacts"][0]["criterion_evidence"],
        )
        remote_head = git_ops.remote_ref_sha(
            fixture.root, "origin", "refs/heads/harness"
        )
        self.assertEqual(remote_head, entry.candidate_sha)
        self.assertEqual(queue_store.read("XT-101").state, "done")
        self.assertEqual(acceptance_value["candidate_sha"], entry.candidate_sha)
        self.assertEqual(
            git_ops.commit_parents(fixture.root, entry.candidate_sha),
            [entry.parent_sha],
        )
        if git_ops.remote_ref_sha(fixture.root, "origin", entry.queue_ref) is not None:
            self.fail("queue refs must be reclaimed after durable completion")
        self.assertIsNone(git_ops.ref_sha(fixture.root, entry.queue_ref))
        self.assertIsNotNone(
            git_ops.remote_ref_sha(fixture.root, "origin", entry.submission_ref)
        )

    def test_acceptance_owner_closes_evidence_without_product_commit(
        self,
    ) -> None:
        fixture = DeliveryFixture(self, with_remote=True)
        entry = fixture.train(fixture.submit()).entries[0]
        queue_store = state.StateStore(
            fixture.contracts,
            remote="origin",
            actor=QUEUE_ACTOR,
            clock=lambda: "2026-08-12T13:00:00Z",
        )
        evidence = fixture.evidence(entry)
        self._publisher(fixture, queue_store).publish(
            entry,
            evidence,
            evidence["artifacts"][0]["gate_attestations"],
            evidence["artifacts"][0]["criterion_evidence"],
        )
        protected_before = git_ops.remote_ref_sha(
            fixture.root, "origin", "refs/heads/harness"
        )
        acceptance_path = Path(fixture.repository.fixture.temporary.name + "-XT-102")
        fixture.workspaces.claim("XT-102", acceptance_path)
        value = closure.AcceptanceCloser(
            fixture.contracts,
            fixture.store,
            fixture.workspaces,
            "origin",
        ).close("XT-102")
        self.assertEqual(
            git_ops.remote_ref_sha(fixture.root, "origin", "refs/heads/harness"),
            protected_before,
        )
        self.assertEqual(fixture.store.read("XT-102").state, "done")
        self.assertFalse(acceptance_path.exists())
        self.assertIsNone(git_ops.ref_sha(fixture.root, "refs/heads/work/XT-102"))
        self.assertEqual(value["criteria"], ["CRIT-EXAMPLE-BEHAVIOR"])

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
                    ["a" * 64, "e" * 64],
                    fixture.evidence(entry)["artifacts"][0]["criterion_evidence"],
                )
        self.assertEqual(
            git_ops.remote_ref_sha(fixture.root, "origin", "refs/heads/harness"),
            entry.candidate_sha,
        )
        queue_store.transition = original_transition
        publisher.recover(entry)
        self.assertEqual(queue_store.read("XT-101").state, "done")
        self.assertIsNone(
            git_ops.remote_ref_sha(fixture.root, "origin", entry.queue_ref)
        )
        self.assertIsNone(git_ops.ref_sha(fixture.root, entry.queue_ref))

    def test_rejects_unbound_criterion_evidence(self) -> None:
        fixture = DeliveryFixture(self, with_remote=True)
        entry = fixture.train(fixture.submit()).entries[0]
        evidence = fixture.evidence(entry)
        evidence["artifacts"][0]["criterion_evidence"] = ["b" * 64]
        queue_store = state.StateStore(
            fixture.contracts,
            remote="origin",
            actor=QUEUE_ACTOR,
        )
        with self.assertRaisesRegex(attestation.AttestationError, "bound criterion"):
            self._publisher(fixture, queue_store).publish(
                entry,
                evidence,
                ["a" * 64, "e" * 64],
                ["b" * 64],
            )
        self.assertEqual(
            git_ops.remote_ref_sha(fixture.root, "origin", "refs/heads/harness"),
            fixture.base,
        )

    def test_criterion_evidence_binds_full_contract(self) -> None:
        fixture = DeliveryFixture(self)
        gate_attestations = ["a" * 64, "e" * 64]
        before = attestation.criterion_evidence_digest(
            fixture.contracts,
            "CRIT-EXAMPLE-BEHAVIOR",
            fixture.base,
            gate_attestations,
        )
        criterion = fixture.contracts.plans["DP-EXAMPLE"]["requirements"][0][
            "criteria"
        ][0]
        criterion["evidence"]["scenarios"].append("new_negative_case")
        after = attestation.criterion_evidence_digest(
            fixture.contracts,
            "CRIT-EXAMPLE-BEHAVIOR",
            fixture.base,
            gate_attestations,
        )
        self.assertNotEqual(before, after)

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
                ["a" * 64, "e" * 64],
                fixture.evidence(entry)["artifacts"][0]["criterion_evidence"],
            )
        self.assertEqual(queue_store.read("XT-101").state, "queued")
        acceptance_ref = self._publisher(fixture, queue_store).acceptance.ref(
            "XT-101", entry.candidate_sha
        )
        self.assertIsNotNone(
            git_ops.remote_ref_sha(fixture.root, "origin", acceptance_ref)
        )
        git_ops.push_ref_cas(
            fixture.root,
            "origin",
            fixture.base,
            "refs/heads/harness",
            competing,
        )
        retried = self._publisher(
            fixture,
            queue_store,
            created_at="2026-08-12T14:00:00Z",
        ).publish(
            entry,
            fixture.evidence(entry),
            ["a" * 64, "e" * 64],
            fixture.evidence(entry)["artifacts"][0]["criterion_evidence"],
        )
        self.assertEqual(retried["created_at"], "2026-08-12T13:00:00Z")
        self.assertEqual(queue_store.read("XT-101").state, "done")

    def test_blocks_train_successor_until_predecessor_state_is_done(self) -> None:
        fixture = DeliveryFixture(self, with_remote=True, multi_task=True)
        fixture.submit()
        fixture.submit_documentation()
        train = queue_module.MergeQueue(
            fixture.contracts, fixture.store, remote="origin"
        ).build_train(["XT-101", "XT-103"], "ordered-train", fixture.base)
        successor = train.entries[1]
        queue_store = state.StateStore(
            fixture.contracts,
            remote="origin",
            actor=QUEUE_ACTOR,
        )
        with self.assertRaisesRegex(queue_module.QueueError, "predecessor XT-101"):
            self._publisher(fixture, queue_store).publish(
                successor,
                fixture.evidence(successor),
                ["a" * 64, "e" * 64],
                fixture.evidence(successor)["artifacts"][0]["criterion_evidence"],
            )

    def test_rejects_candidate_that_self_modifies_queue_trust_root(self) -> None:
        fixture = DeliveryFixture(self, with_remote=True)
        entry = fixture.train(fixture.submit()).entries[0]
        tamper = fixture.repository.external / "trust-root-tamper"
        git_ops.run_git(
            fixture.root,
            "worktree",
            "add",
            "--detach",
            str(tamper),
            entry.parent_sha,
        )
        try:
            workflow = tamper / ".github" / "workflows" / "merge-queue.yml"
            workflow.write_text(
                "name: weakened\non: push\njobs: {}\n",
                encoding="utf-8",
            )
            git_ops.run_git(tamper, "add", ".github/workflows/merge-queue.yml")
            git_ops.run_git(
                tamper,
                "commit",
                "-m",
                "ci(harness): weaken queue",
                "-m",
                "Xnn-Task: XT-101\nXnn-Lifecycle: delivery",
            )
            candidate = git_ops.object_id(tamper, "HEAD")
        finally:
            git_ops.run_git(
                fixture.root,
                "worktree",
                "remove",
                "--force",
                str(tamper),
                check=False,
            )
        manifest = dict(entry.submission)
        manifest["payload_patch_sha256"] = queue_module._payload_sha256(
            fixture.root, entry.parent_sha, candidate
        )
        fixture.contracts.tasks["XT-101"]["owned_paths"] = [".github/workflows/**"]
        tampered_entry = replace(
            entry,
            candidate_sha=candidate,
            submission=manifest,
        )
        queue_store = state.StateStore(
            fixture.contracts,
            remote="origin",
            actor=QUEUE_ACTOR,
        )
        with self.assertRaisesRegex(queue_module.QueueError, "trust root"):
            self._publisher(fixture, queue_store).publish(
                tampered_entry,
                fixture.evidence(tampered_entry),
                ["a" * 64, "e" * 64],
                fixture.evidence(tampered_entry)["artifacts"][0]["criterion_evidence"],
            )
        self.assertEqual(
            git_ops.remote_ref_sha(fixture.root, "origin", "refs/heads/harness"),
            fixture.base,
        )


if __name__ == "__main__":
    unittest.main()
