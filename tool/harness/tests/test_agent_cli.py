#!/usr/bin/env python3
"""Smoke tests for the single Harness V2 CLI."""

from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path

from test_gates_tdd import GateRepository


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
        self.assertEqual(
            evidence["source_sha"], repository.git("rev-parse", "HEAD")
        )
        self.assertTrue(evidence["gate_attestations"])
        self.assertTrue(evidence["criterion_evidence"])


if __name__ == "__main__":
    unittest.main()
