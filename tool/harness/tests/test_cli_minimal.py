#!/usr/bin/env python3
"""Smoke tests for the minimal Harness V2 CLI."""

from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path

from support import Repository


AGENT = Path(__file__).resolve().parents[1] / "agent.py"


class MinimalCliTest(unittest.TestCase):
    def _run(
        self,
        repository: Repository,
        *arguments: str,
    ) -> subprocess.CompletedProcess[str]:
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

    def test_validate_list_matrix_and_dry_run_cleanup(self) -> None:
        repository = Repository(self)
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

        cleanup = self._run(repository, "branch-gc")
        self.assertEqual(cleanup.returncode, 0, cleanup.stderr)
        self.assertEqual(json.loads(cleanup.stdout)["mode"], "dry-run")

    def test_removed_commands_are_not_exposed(self) -> None:
        repository = Repository(self)
        result = self._run(repository, "approve-plan")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid choice", result.stderr)
        self.assertNotIn("acceptance-close", result.stderr)
        self.assertNotIn("collect-evidence", result.stderr)


if __name__ == "__main__":
    unittest.main()
