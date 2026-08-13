#!/usr/bin/env python3
"""Adversarial tests for derived state and temporary delivery refs."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path
from unittest import mock


HARNESS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HARNESS))

import ci_validation  # noqa: E402
import cleanup  # noqa: E402
import delivery  # noqa: E402
import git_ops  # noqa: E402
import model  # noqa: E402
import runtime  # noqa: E402
import tdd  # noqa: E402
import workspace  # noqa: E402
from support import Repository  # noqa: E402


class RuntimeDeliveryTest(unittest.TestCase):
    def _active_green(
        self,
        repository: Repository,
        *,
        remote: str | None = None,
    ) -> tuple[
        model.ContractSet,
        runtime.RuntimeView,
        workspace.WorkspaceManager,
        str,
    ]:
        contracts = repository.contracts()
        view = runtime.RuntimeView(contracts, remote)
        workspaces = workspace.WorkspaceManager(contracts, view)
        path = repository.external / "XT-101"
        workspaces.claim("XT-101", path)
        (path / "product" / "tests").mkdir(parents=True, exist_ok=True)
        (path / "product" / "tests" / "red.txt").write_text(
            "red\n",
            encoding="utf-8",
        )
        red_sha = repository.commit_in(path, "test: expose missing example")
        red = tdd.TddManager(contracts, workspaces).record_red("XT-101")
        self.assertEqual(red["red_sha"], red_sha)
        (path / "product" / "impl.txt").write_text(
            "green\n",
            encoding="utf-8",
        )
        repository.commit_in(path, "feat: implement example")
        return contracts, view, workspaces, red_sha

    def test_state_is_derived_from_history_worktree_and_queue(self) -> None:
        repository = Repository(self)
        contracts = repository.contracts()
        view = runtime.RuntimeView(contracts)
        self.assertEqual(view.read("XT-101").state, "ready")

        workspaces = workspace.WorkspaceManager(contracts, view)
        path = repository.external / "XT-101"
        workspaces.claim("XT-101", path)
        self.assertEqual(view.read("XT-101").state, "active")

        (path / "product" / "impl.txt").write_text("green\n", encoding="utf-8")
        repository.commit_in(path, "feat: implement example")
        candidate = repository.git("rev-parse", "work/XT-101")
        active = workspaces.active_workspace("XT-101")
        git_ops.update_ref_cas(
            repository.root,
            "refs/heads/queue/train/001-XT-101",
            candidate,
            None,
        )
        workspaces.release(active, candidate)
        self.assertEqual(view.read("XT-101").state, "queued")

        message = (
            "feat(example): implement example behavior\n\n"
            "Xnn-Task: XT-101\n"
            "Xnn-Lifecycle: delivery"
        )
        repository.git(
            "commit",
            "--amend",
            "-q",
            "-m",
            message,
        )
        delivery_sha = repository.git("rev-parse", "HEAD")
        self.assertNotEqual(delivery_sha, candidate)
        self.assertEqual(view.read("XT-101").state, "done")

    def test_tdd_red_is_replayed_from_git_chronology(self) -> None:
        repository = Repository(self)
        contracts, _, workspaces, red_sha = self._active_green(repository)
        result = tdd.TddManager(contracts, workspaces).review_green(
            "XT-101",
            red_sha,
        )
        self.assertEqual(result["red_sha"], red_sha)
        self.assertEqual(
            result["green_sha"],
            git_ops.object_id(repository.external / "XT-101", "HEAD"),
        )
        self.assertFalse(git_ops.list_refs(repository.root, "refs/heads/attest/"))

    def test_submit_creates_only_temporary_queue_and_releases_worktree(self) -> None:
        repository = Repository(self)
        contracts, view, workspaces, red_sha = self._active_green(repository)
        queue = delivery.QueueManager(contracts, view, workspaces, None)
        train = queue.build_train(
            ["XT-101"],
            "train-001",
            red_shas={"XT-101": red_sha},
        )
        entry = train.entries[0]
        self.assertEqual(view.read("XT-101").state, "queued")
        self.assertFalse((repository.external / "XT-101").exists())
        refs = git_ops.list_refs(repository.root, "refs/heads/")
        self.assertIn(entry.queue_ref, refs)
        self.assertNotIn("refs/heads/work/XT-101", refs)
        self.assertFalse(
            any(
                ref.startswith(
                    (
                        "refs/heads/approve/",
                        "refs/heads/state/",
                        "refs/heads/submit/",
                        "refs/heads/attest/",
                        "refs/heads/archive/",
                    )
                )
                for ref in refs
            )
        )

    def test_reopen_drops_queue_without_archive(self) -> None:
        repository = Repository(self)
        contracts, view, workspaces, red_sha = self._active_green(repository)
        queue = delivery.QueueManager(contracts, view, workspaces, None)
        entry = queue.build_train(
            ["XT-101"],
            "train-001",
            red_shas={"XT-101": red_sha},
        ).entries[0]
        path = queue.reopen(entry)
        self.assertTrue(path.is_dir())
        self.assertEqual(view.read("XT-101").state, "active")
        self.assertIsNone(git_ops.ref_sha(repository.root, entry.queue_ref))
        self.assertFalse(git_ops.list_refs(repository.root, "refs/heads/archive/"))

    def test_publish_consumes_live_ci_once_and_deletes_queue(self) -> None:
        repository = Repository(self, remote=True)
        contracts, view, workspaces, red_sha = self._active_green(
            repository,
            remote="origin",
        )
        queue = delivery.QueueManager(contracts, view, workspaces, "origin")
        entry = queue.build_train(
            ["XT-101"],
            "train-001",
            red_shas={"XT-101": red_sha},
        ).entries[0]
        gate_digests = ["a" * 64, "b" * 64]
        criterion_digest = ci_validation.criterion_evidence_digest(
            contracts,
            "CRIT-EXAMPLE",
            entry.candidate_sha,
            gate_digests,
        )
        workflow_blob = git_ops.object_id(
            repository.root,
            "{}:.github/workflows/merge-queue.yml".format(entry.candidate_sha),
        )
        workflow = {
            "repository": "example/repository",
            "workflow_path": ".github/workflows/merge-queue.yml",
            "workflow_blob": workflow_blob,
            "run_id": 42,
            "run_attempt": 1,
            "head_sha": entry.candidate_sha,
            "head_branch": entry.queue_ref[len("refs/heads/") :],
            "event": "push",
            "conclusion": "success",
            "jobs": [
                {"name": "Candidate plan", "conclusion": "success"},
                {"name": "Harness V2", "conclusion": "success"},
                {"name": "Product gates (linux)", "conclusion": "success"},
                {"name": "Candidate accepted", "conclusion": "success"},
            ],
            "artifacts": [
                {
                    "name": "candidate-evidence-linux",
                    "source_sha": entry.candidate_sha,
                    "sha256": "c" * 64,
                    "platform": "linux",
                    "gate_ids": ["governance", "feature_test"],
                    "gate_attestations": gate_digests,
                    "criterion_ids": ["CRIT-EXAMPLE"],
                    "criterion_evidence": [criterion_digest],
                }
            ],
        }
        publisher = delivery.Publisher(
            contracts,
            view,
            "origin",
            "example/repository",
            ".github/workflows/merge-queue.yml",
        )
        with mock.patch.object(
            delivery.github_evidence,
            "credential_token",
            return_value="token",
        ), mock.patch.object(
            delivery.github_evidence,
            "collect_workflow_evidence",
            return_value=workflow,
        ) as collect:
            result = publisher.publish(entry)
        self.assertEqual(result["run_id"], 42)
        collect.assert_called_once()
        self.assertEqual(
            git_ops.remote_ref_sha(
                repository.root,
                "origin",
                "refs/heads/harness",
            ),
            entry.candidate_sha,
        )
        self.assertIsNone(
            git_ops.remote_ref_sha(
                repository.root,
                "origin",
                entry.queue_ref,
            )
        )
        self.assertEqual(view.read("XT-101").state, "done")
        remote_refs = git_ops.list_remote_refs(
            repository.root,
            "origin",
            "refs/heads/",
        )
        self.assertEqual(set(remote_refs), {"refs/heads/harness"})

    def test_cleanup_never_uses_age_or_deletes_unpublished_work(self) -> None:
        repository = Repository(self)
        contracts, view, workspaces, _ = self._active_green(repository)
        active = workspaces.active_workspace("XT-101")
        repository.git(
            "worktree",
            "remove",
            "--force",
            str(active.path),
        )
        plan = cleanup.BranchCleanup(contracts, view, None).plan()
        self.assertEqual(plan, [])
        self.assertIsNotNone(git_ops.ref_sha(repository.root, "refs/heads/work/XT-101"))


if __name__ == "__main__":
    unittest.main()
