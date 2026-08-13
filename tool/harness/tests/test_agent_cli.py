#!/usr/bin/env python3
"""Smoke tests for the single Harness V2 CLI."""

from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path

from test_gates_tdd import GateRepository

import gates
import git_ops
import model
import state


AGENT = Path(__file__).resolve().parents[1] / "agent.py"


class AgentCliTest(unittest.TestCase):
    def _run(
        self, repository: GateRepository, *arguments: str
    ) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                sys.executable,
                "-B",
                str(AGENT),
                "--root",
                str(repository.root),
                "--local",
                *arguments,
            ],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_validate_list_and_matrix(self) -> None:
        repository = GateRepository(self)
        repository.commit("chore: initialize")
        validation = self._run(repository, "validate")
        self.assertEqual(validation.returncode, 0, validation.stderr)
        self.assertEqual(json.loads(validation.stdout)["status"], "valid")

        listing = self._run(repository, "list")
        self.assertEqual(listing.returncode, 0, listing.stderr)
        self.assertIn("XT-101\tready", listing.stdout)

        matrix = self._run(repository, "matrix", "XT-101")
        self.assertEqual(matrix.returncode, 0, matrix.stderr)
        self.assertEqual(
            json.loads(matrix.stdout),
            {"include": [{"runner": "ubuntu-latest", "label": "linux"}]},
        )

    def test_verify_emits_candidate_evidence(self) -> None:
        repository = GateRepository(self)
        repository.commit("chore: initialize")
        output = repository.external / "evidence.json"
        result = self._run(
            repository,
            "verify",
            "XT-101",
            "--phase",
            "review",
            "--output",
            str(output),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        evidence = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(evidence["source_sha"], repository.git("rev-parse", "HEAD"))
        self.assertTrue(evidence["gate_attestations"])
        self.assertTrue(evidence["criterion_evidence"])

    def test_verify_all_emits_bootstrap_evidence(self) -> None:
        repository = GateRepository(self)
        repository.commit("chore: initialize")
        output = repository.external / "bootstrap-evidence.json"
        result = self._run(
            repository,
            "verify-all",
            "--platform",
            "linux",
            "--output",
            str(output),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        evidence = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(evidence["kind"], "bootstrap_cutover")
        self.assertEqual(evidence["source_sha"], repository.git("rev-parse", "HEAD"))
        self.assertEqual(evidence["platform"], "linux")
        self.assertFalse(evidence["skipped"])
        self.assertEqual(
            evidence["plan_sha256"],
            gates.global_gate_plan(model.load_contracts(repository.root)).digest,
        )
        self.assertEqual(
            len(evidence["gate_ids"]),
            len(evidence["gate_attestations"]),
        )

    def test_cumulative_candidate_uses_all_task_risks_and_gates(self) -> None:
        repository = GateRepository(self)
        repository.fixture.implementation["risk"]["platform"] = "high"
        repository.fixture.gates["gates"]["feature_test"]["platforms"].extend(
            ["macos", "windows"]
        )
        repository.fixture.write()
        repository.commit("chore: initialize")
        repository.git("checkout", "-q", "-b", "queue/train/002-XT-102")
        repository.commit(
            "feat(harness): deliver first\n\n"
            "Xnn-Task: XT-101\n"
            "Xnn-Lifecycle: delivery",
            allow_empty=True,
        )
        repository.commit(
            "test(harness): deliver acceptance\n\n"
            "Xnn-Task: XT-102\n"
            "Xnn-Lifecycle: delivery",
            allow_empty=True,
        )

        matrix = self._run(repository, "matrix")
        self.assertEqual(matrix.returncode, 0, matrix.stderr)
        self.assertEqual(
            {item["label"] for item in json.loads(matrix.stdout)["include"]},
            {"linux", "macos", "windows"},
        )

        output = repository.external / "candidate-evidence.json"
        verification = self._run(
            repository,
            "verify",
            "--phase",
            "queue",
            "--output",
            str(output),
        )
        self.assertEqual(verification.returncode, 0, verification.stderr)
        evidence = json.loads(output.read_text(encoding="utf-8"))
        self.assertTrue(evidence["gate_attestations"])
        self.assertTrue(evidence["criterion_evidence"])

    def test_branch_gc_is_dry_run_by_default_and_preserves_evidence(self) -> None:
        repository = GateRepository(self)
        head = repository.commit("chore: initialize")
        contracts = model.load_contracts(repository.root)
        store = state.StateStore(
            contracts,
            actor={
                "kind": "user",
                "id": "owner@example.com",
                "name": "Project Owner",
                "email": "owner@example.com",
            },
            clock=lambda: "2026-08-12T12:00:00Z",
        )
        work_ref = "refs/heads/work/XT-101"
        git_ops.update_ref_cas(repository.root, work_ref, head, None)
        store.transition(
            "XT-101",
            "ready",
            "active",
            "claimed",
            details={
                "base_sha": head,
                "branch": "work/XT-101",
                "worktree": "/tmp/missing-XT-101",
                "owner": "owner@example.com",
            },
        )
        store.transition(
            "XT-101",
            "active",
            "queued",
            "reviewed_submission",
            details={"source_head": head},
            submission_ref="refs/heads/submit/XT-101/000001",
        )
        store.transition(
            "XT-101",
            "queued",
            "done",
            "published",
            details={
                "acceptance_attestation_sha256": "a" * 64,
                "published_sha": head,
            },
            submission_ref="refs/heads/submit/XT-101/000001",
        )

        dry_run = self._run(repository, "branch-gc")
        self.assertEqual(dry_run.returncode, 0, dry_run.stderr)
        dry_value = json.loads(dry_run.stdout)
        self.assertEqual(dry_value["mode"], "dry-run")
        self.assertEqual(
            [item["ref"] for item in dry_value["eligible"]],
            [work_ref],
        )
        self.assertEqual(git_ops.ref_sha(repository.root, work_ref), head)
        self.assertTrue(
            all(
                item["ref"].startswith(("refs/heads/work/", "refs/heads/queue/"))
                for item in dry_value["eligible"]
            )
        )

        execute = self._run(repository, "branch-gc", "--execute")
        self.assertEqual(execute.returncode, 0, execute.stderr)
        self.assertEqual(json.loads(execute.stdout)["mode"], "execute")
        self.assertIsNone(git_ops.ref_sha(repository.root, work_ref))
        self.assertIsNotNone(
            git_ops.ref_sha(repository.root, "refs/heads/state/XT-101")
        )


if __name__ == "__main__":
    unittest.main()
