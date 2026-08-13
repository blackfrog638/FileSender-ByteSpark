#!/usr/bin/env python3
"""Tests for the derived Harness V2 dashboard."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

HARNESS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HARNESS))

import dashboard  # noqa: E402
import model  # noqa: E402
from support import Repository  # noqa: E402


class DashboardTest(unittest.TestCase):
    def test_renders_derived_task_state(self) -> None:
        repository = Repository(self)
        output = repository.root / "dashboard.html"
        dashboard.render(repository.root, output)
        document = output.read_text(encoding="utf-8")
        self.assertIn("XT-101", document)
        self.assertIn("ready", document)
        self.assertIn("Plans: 1", document)

    def test_rejects_catalogue_that_orphans_blueprint_transition(self) -> None:
        repository = Repository(self)
        for path in (repository.root / ".agents" / "plans").glob("*.json"):
            path.unlink()
        for path in (repository.root / ".agents" / "tasks").glob("*.json"):
            path.unlink()
        output = repository.root / "dashboard.html"
        with self.assertRaisesRegex(model.ContractError, "references unknown Plan"):
            dashboard.render(repository.root, output)


if __name__ == "__main__":
    unittest.main()
