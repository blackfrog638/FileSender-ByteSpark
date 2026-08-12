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
import model


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


if __name__ == "__main__":
    unittest.main()
