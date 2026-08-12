#!/usr/bin/env python3
"""Tests for immutable project-owner Plan approvals."""

from __future__ import annotations

import shutil
import subprocess
import sys
import unittest
from pathlib import Path

HARNESS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HARNESS))

import approval  # noqa: E402
import git_ops  # noqa: E402
from test_model import ContractFixture  # noqa: E402


class ApprovalTest(unittest.TestCase):
    def _repository(self) -> tuple[ContractFixture, Path]:
        fixture = ContractFixture(self)
        subprocess.run(
            ["git", "init", "-q", "-b", "harness", str(fixture.root)],
            check=True,
        )
        git_ops.run_git(fixture.root, "config", "user.name", "Project Owner")
        git_ops.run_git(fixture.root, "config", "user.email", "owner@example.com")
        git_ops.run_git(fixture.root, "add", ".")
        git_ops.run_git(fixture.root, "commit", "-q", "-m", "chore: initialize")
        remote = Path(fixture.temporary.name + "-approval-remote.git")
        self.addCleanup(lambda: shutil.rmtree(remote, ignore_errors=True))
        subprocess.run(
            ["git", "init", "-q", "--bare", str(remote)],
            check=True,
        )
        git_ops.run_git(fixture.root, "remote", "add", "origin", str(remote))
        git_ops.run_git(fixture.root, "push", "-q", "origin", "harness")
        return fixture, remote

    def test_writes_and_refetches_authoritative_approval(self) -> None:
        fixture, _ = self._repository()
        store = approval.ApprovalStore(fixture.root, fixture.manifest, "origin")
        commit = store.write(fixture.plan, "2026-08-12T12:00:00Z")
        ref = store.ref(
            fixture.plan["id"],
            fixture.plan["approval"]["content_sha256"],
        )
        self.assertEqual(
            git_ops.remote_ref_sha(fixture.root, "origin", ref),
            commit,
        )
        git_ops.run_git(fixture.root, "update-ref", "-d", ref, commit)
        value = store.require(fixture.plan)
        self.assertEqual(
            value["content_sha256"],
            fixture.plan["approval"]["content_sha256"],
        )

    def test_rejects_missing_remote_approval(self) -> None:
        fixture, _ = self._repository()
        store = approval.ApprovalStore(fixture.root, fixture.manifest, "origin")
        with self.assertRaisesRegex(approval.ApprovalError, "remote immutable ref"):
            store.require(fixture.plan)

    def test_rejects_non_owner_identity(self) -> None:
        fixture, _ = self._repository()
        git_ops.run_git(fixture.root, "config", "user.email", "agent@example.com")
        store = approval.ApprovalStore(fixture.root, fixture.manifest, "origin")
        with self.assertRaisesRegex(approval.ApprovalError, "configured project owner"):
            store.write(fixture.plan, "2026-08-12T12:00:00Z")


if __name__ == "__main__":
    unittest.main()
