#!/usr/bin/env python3
"""Tests for the derived Harness V2 dashboard."""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

HARNESS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HARNESS))

import dashboard  # noqa: E402
from test_model import ContractFixture  # noqa: E402


class DashboardTest(unittest.TestCase):
    def _fixture(self) -> ContractFixture:
        fixture = ContractFixture(self)
        subprocess.run(
            ["git", "init", "-q", "-b", "harness", str(fixture.root)],
            check=True,
        )
        subprocess.run(
            [
                "git",
                "-C",
                str(fixture.root),
                "config",
                "user.name",
                "Project Owner",
            ],
            check=True,
        )
        subprocess.run(
            [
                "git",
                "-C",
                str(fixture.root),
                "config",
                "user.email",
                "owner@example.com",
            ],
            check=True,
        )
        return fixture

    def test_renders_derived_task_state(self) -> None:
        fixture = self._fixture()
        output = fixture.root / "dashboard.html"
        dashboard.render(fixture.root, output)
        document = output.read_text(encoding="utf-8")
        self.assertIn("XT-101", document)
        self.assertIn("ready", document)
        self.assertIn("Plans: 1", document)

    def test_empty_active_catalogue_is_visible(self) -> None:
        fixture = self._fixture()
        for path in (fixture.root / ".agents" / "plans").glob("*.json"):
            path.unlink()
        for path in (fixture.root / ".agents" / "tasks").glob("*.json"):
            path.unlink()
        output = fixture.root / "dashboard.html"
        dashboard.render(fixture.root, output)
        document = output.read_text(encoding="utf-8")
        self.assertIn("Active TaskSpecs: 0", document)
        self.assertIn("<tbody></tbody>", document)


if __name__ == "__main__":
    unittest.main()
