#!/usr/bin/env python3
"""Tests for Harness V2 Git state and worktree ownership."""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HARNESS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HARNESS))

import git_ops  # noqa: E402
import model  # noqa: E402
import state  # noqa: E402
import workspace  # noqa: E402
from test_model import ContractFixture  # noqa: E402


ACTOR = {
    "kind": "user",
    "id": "owner@example.com",
    "name": "Project Owner",
    "email": "owner@example.com",
}


class RepositoryFixture:
    def __init__(self, testcase: unittest.TestCase) -> None:
        self.contract_fixture = ContractFixture(testcase)
        self.root = self.contract_fixture.root
        subprocess.run(
            ["git", "init", "-q", "-b", "harness", str(self.root)], check=True
        )
        subprocess.run(
            ["git", "-C", str(self.root), "config", "user.name", "Project Owner"],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(self.root), "config", "user.email", "owner@example.com"],
            check=True,
        )
        (self.root / "docs").mkdir(exist_ok=True)
        (self.root / "native" / "tests").mkdir(parents=True)
        (self.root / "native" / "tests" / "feature_test.cpp").write_text(
            "// test\n", encoding="utf-8"
        )
        self.commit("chore: initialize fixture")
        self.contracts = model.load_contracts(self.root)
        self.store = state.StateStore(
            self.contracts,
            actor=ACTOR,
            clock=lambda: "2026-08-12T12:00:00Z",
        )

    def git(self, *arguments: str) -> str:
        result = subprocess.run(
            ["git", "-C", str(self.root), *arguments],
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout.strip()

    def commit(self, message: str) -> str:
        self.git("add", ".")
        self.git("commit", "-q", "-m", message)
        return self.git("rev-parse", "HEAD")

    def active(self) -> state.StateSnapshot:
        return self.store.transition(
            "XT-101",
            "ready",
            "active",
            "claimed",
            details={
                "base_sha": self.git("rev-parse", "harness"),
                "branch": "work/XT-101",
                "worktree": "/tmp/fixture-XT-101",
                "owner": ACTOR["id"],
            },
        )


class StateTest(unittest.TestCase):
    def test_appends_and_validates_complete_state_chain(self) -> None:
        fixture = RepositoryFixture(self)
        self.assertEqual(fixture.store.read("XT-101").state, "ready")
        active = fixture.active()
        queued = fixture.store.transition(
            "XT-101",
            "active",
            "queued",
            "reviewed_submission",
            details={"attempt": 1},
            submission_ref="refs/heads/submit/XT-101",
        )
        done = fixture.store.transition(
            "XT-101",
            "queued",
            "done",
            "published",
            details={
                "acceptance_attestation_sha256": "a" * 64,
                "published_sha": "b" * 40,
            },
            submission_ref="refs/heads/submit/XT-101",
        )
        self.assertEqual(active.sequence, 1)
        self.assertEqual(queued.sequence, 2)
        self.assertEqual(done.sequence, 3)
        history = fixture.store.history("XT-101")
        self.assertEqual(
            [event["to"] for event in history], ["active", "queued", "done"]
        )
        self.assertEqual(
            history[2]["previous_event_sha256"],
            model.canonical_sha256(history[1]),
        )

    def test_rejects_illegal_transition_and_missing_done_proof(self) -> None:
        fixture = RepositoryFixture(self)
        with self.assertRaisesRegex(state.StateError, "illegal requested"):
            fixture.store.transition("XT-101", "ready", "done", "skip", details={})
        fixture.active()
        fixture.store.transition(
            "XT-101",
            "active",
            "queued",
            "reviewed_submission",
            submission_ref="refs/heads/submit/XT-101",
        )
        with self.assertRaisesRegex(state.StateError, "missing publication proof"):
            fixture.store.transition(
                "XT-101",
                "queued",
                "done",
                "published",
                submission_ref="refs/heads/submit/XT-101",
            )

    def test_low_level_ref_update_is_compare_and_swap(self) -> None:
        fixture = RepositoryFixture(self)
        active = fixture.active()
        with self.assertRaisesRegex(git_ops.GitError, "stale ref"):
            git_ops.append_json_ref(
                fixture.root,
                active.ref,
                {"not": "an event"},
                "stale writer",
                None,
            )

    def test_local_ref_deletion_is_idempotent_and_compare_and_swap(self) -> None:
        fixture = RepositoryFixture(self)
        original = fixture.git("rev-parse", "HEAD")
        ref = "refs/heads/queue/test/001-XT-101"
        git_ops.update_ref_cas(fixture.root, ref, original, None)
        moved = git_ops.git_text(
            fixture.root,
            "commit-tree",
            git_ops.current_tree(fixture.root),
            "-p",
            original,
            input_bytes=b"move candidate\n",
        )
        git_ops.update_ref_cas(fixture.root, ref, moved, original)
        with self.assertRaisesRegex(git_ops.GitError, "compare-and-swap"):
            git_ops.delete_ref_cas(fixture.root, ref, original)
        self.assertEqual(git_ops.ref_sha(fixture.root, ref), moved)
        self.assertTrue(git_ops.delete_ref_cas(fixture.root, ref, moved))
        self.assertFalse(git_ops.delete_ref_cas(fixture.root, ref, moved))

    def test_remote_ref_deletion_is_idempotent_and_compare_and_swap(self) -> None:
        fixture = RepositoryFixture(self)
        remote = Path(fixture.contract_fixture.temporary.name + "-remote.git")
        self.addCleanup(lambda: shutil.rmtree(remote, ignore_errors=True))
        subprocess.run(["git", "init", "-q", "--bare", str(remote)], check=True)
        fixture.git("remote", "add", "origin", str(remote))
        original = fixture.git("rev-parse", "HEAD")
        ref = "refs/heads/queue/test/001-XT-101"
        git_ops.push_ref_cas(fixture.root, "origin", original, ref, None)
        moved = git_ops.git_text(
            fixture.root,
            "commit-tree",
            git_ops.current_tree(fixture.root),
            "-p",
            original,
            input_bytes=b"move remote candidate\n",
        )
        git_ops.push_ref_cas(fixture.root, "origin", moved, ref, original)
        with self.assertRaisesRegex(git_ops.GitError, "compare-and-swap"):
            git_ops.delete_remote_ref_cas(
                fixture.root,
                "origin",
                ref,
                original,
            )
        self.assertEqual(
            git_ops.remote_ref_sha(fixture.root, "origin", ref),
            moved,
        )
        self.assertTrue(
            git_ops.delete_remote_ref_cas(fixture.root, "origin", ref, moved)
        )
        self.assertFalse(
            git_ops.delete_remote_ref_cas(fixture.root, "origin", ref, moved)
        )

    def test_rejects_tampered_event_chain(self) -> None:
        fixture = RepositoryFixture(self)
        active = fixture.active()
        tampered = dict(active.event)
        tampered["sequence"] = 3
        commit = git_ops.commit_json(
            fixture.root,
            tampered,
            "tampered",
            parent=active.commit,
        )
        git_ops.update_ref_cas(fixture.root, active.ref, commit, active.commit)
        with self.assertRaisesRegex(state.StateError, "sequence is not contiguous"):
            fixture.store.read("XT-101")

    def test_remote_state_ref_is_pushed_with_cas(self) -> None:
        fixture = RepositoryFixture(self)
        remote_parent = Path(fixture.contract_fixture.temporary.name).parent
        remote = remote_parent / (
            Path(fixture.contract_fixture.temporary.name).name + "-remote.git"
        )
        self.addCleanup(lambda: shutil.rmtree(remote, ignore_errors=True))
        subprocess.run(["git", "init", "-q", "--bare", str(remote)], check=True)
        fixture.git("remote", "add", "origin", str(remote))
        store = state.StateStore(
            fixture.contracts,
            remote="origin",
            actor=ACTOR,
            clock=lambda: "2026-08-12T12:00:00Z",
        )
        snapshot = store.transition(
            "XT-101",
            "ready",
            "active",
            "claimed",
            details={
                "base_sha": fixture.git("rev-parse", "harness"),
                "branch": "work/XT-101",
                "worktree": "/tmp/fixture-XT-101",
                "owner": ACTOR["id"],
            },
        )
        remote_sha = subprocess.run(
            [
                "git",
                "--git-dir",
                str(remote),
                "rev-parse",
                "refs/heads/state/XT-101",
            ],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        self.assertEqual(remote_sha, snapshot.commit)


class WorkspaceTest(unittest.TestCase):
    def _worktree_path(self, fixture: RepositoryFixture, suffix: str) -> Path:
        path = Path(fixture.contract_fixture.temporary.name + suffix)
        self.addCleanup(lambda: shutil.rmtree(path, ignore_errors=True))
        return path

    def test_claim_creates_active_state_branch_and_worktree(self) -> None:
        fixture = RepositoryFixture(self)
        manager = workspace.WorkspaceManager(fixture.contracts, fixture.store)
        path = self._worktree_path(fixture, "-XT-101")
        claimed = manager.claim("XT-101", path)
        self.assertEqual(fixture.store.read("XT-101").state, "active")
        self.assertTrue(path.is_dir())
        self.assertEqual(
            subprocess.run(
                ["git", "-C", str(path), "branch", "--show-current"],
                check=True,
                capture_output=True,
                text=True,
            ).stdout.strip(),
            "work/XT-101",
        )
        self.assertEqual(claimed.base_sha, fixture.git("rev-parse", "harness"))

    def test_claim_rejects_unfinished_dependency(self) -> None:
        fixture = RepositoryFixture(self)
        manager = workspace.WorkspaceManager(fixture.contracts, fixture.store)
        path = self._worktree_path(fixture, "-XT-102")
        with self.assertRaisesRegex(workspace.WorkspaceError, "dependency XT-101"):
            manager.claim("XT-102", path)
        self.assertFalse(path.exists())
        self.assertEqual(fixture.store.read("XT-102").state, "ready")

    def test_failed_worktree_creation_rolls_state_back_to_ready(self) -> None:
        fixture = RepositoryFixture(self)
        manager = workspace.WorkspaceManager(fixture.contracts, fixture.store)
        path = self._worktree_path(fixture, "-XT-101")
        with mock.patch.object(
            workspace.git_ops,
            "add_worktree",
            side_effect=git_ops.GitError("injected worktree failure"),
        ):
            with self.assertRaisesRegex(workspace.WorkspaceError, "claim failed"):
                manager.claim("XT-101", path)
        self.assertEqual(fixture.store.read("XT-101").state, "ready")
        self.assertEqual(
            [event["to"] for event in fixture.store.history("XT-101")],
            ["active", "ready"],
        )

    def test_recovers_claim_crash_before_worktree_creation(self) -> None:
        fixture = RepositoryFixture(self)
        fixture.active()
        manager = workspace.WorkspaceManager(fixture.contracts, fixture.store)
        self.assertIsNone(manager.recover_claim("XT-101"))
        self.assertEqual(fixture.store.read("XT-101").state, "ready")

    def test_claim_recovery_preserves_branch_with_user_commits(self) -> None:
        fixture = RepositoryFixture(self)
        active = fixture.active()
        tree = git_ops.current_tree(fixture.root)
        commit = git_ops.git_text(
            fixture.root,
            "commit-tree",
            tree,
            "-p",
            active.event["details"]["base_sha"],
            input_bytes=b"user payload\n",
        )
        git_ops.update_ref_cas(
            fixture.root,
            "refs/heads/work/XT-101",
            commit,
            None,
        )
        manager = workspace.WorkspaceManager(fixture.contracts, fixture.store)
        with self.assertRaisesRegex(
            workspace.WorkspaceError, "branch with user commits"
        ):
            manager.recover_claim("XT-101")
        self.assertEqual(fixture.store.read("XT-101").state, "active")

    def test_stale_check_ignores_unrelated_and_rejects_owned_change(self) -> None:
        fixture = RepositoryFixture(self)
        manager = workspace.WorkspaceManager(fixture.contracts, fixture.store)
        path = self._worktree_path(fixture, "-XT-101")
        manager.claim("XT-101", path)

        (fixture.root / "README.md").write_text("unrelated\n", encoding="utf-8")
        fixture.commit("docs: unrelated")
        self.assertEqual(manager.stale_reasons("XT-101"), [])

        (fixture.root / "native" / "source.cpp").write_text(
            "// changed\n", encoding="utf-8"
        )
        fixture.commit("feat: overlap")
        reasons = manager.stale_reasons("XT-101")
        self.assertTrue(any("owned path" in reason for reason in reasons))


if __name__ == "__main__":
    unittest.main()
