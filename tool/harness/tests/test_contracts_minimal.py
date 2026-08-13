#!/usr/bin/env python3
"""Tests for the compatibility-free Harness V2 contracts."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path


HARNESS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HARNESS))

import model  # noqa: E402
from support import Repository  # noqa: E402


class MinimalContractTest(unittest.TestCase):
    def _rewrite(self, path: Path, value: dict) -> None:
        path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")

    def test_loads_without_approval_or_acceptance_entities(self) -> None:
        repository = Repository(self)
        contracts = repository.contracts()
        self.assertEqual(set(contracts.tasks), {"XT-101"})
        self.assertNotIn("project_owner", contracts.manifest)
        self.assertNotIn("ref_namespaces", contracts.manifest)
        self.assertNotIn("approval", contracts.plans["DP-EXAMPLE"])
        self.assertNotIn(
            "acceptance_owner",
            contracts.plans["DP-EXAMPLE"]["requirements"][0],
        )

    def test_rejects_durable_ref_namespaces(self) -> None:
        repository = Repository(self)
        path = repository.root / ".agents" / "manifest.json"
        value = json.loads(path.read_text(encoding="utf-8"))
        value["ref_namespaces"] = {"state": "refs/heads/state/"}
        self._rewrite(path, value)
        with self.assertRaisesRegex(model.ContractError, "unknown=ref_namespaces"):
            repository.contracts()

    def test_rejects_plan_approval_and_acceptance_owner(self) -> None:
        repository = Repository(self)
        path = repository.root / ".agents" / "plans" / "DP-EXAMPLE.json"
        value = json.loads(path.read_text(encoding="utf-8"))
        value["approval"] = {"approved_by": "owner"}
        value["requirements"][0]["acceptance_owner"] = "XT-102"
        self._rewrite(path, value)
        with self.assertRaisesRegex(model.ContractError, "unknown=approval"):
            repository.contracts()

    def test_rejects_acceptance_task_type(self) -> None:
        repository = Repository(self)
        path = repository.root / ".agents" / "tasks" / "XT-101.json"
        value = json.loads(path.read_text(encoding="utf-8"))
        value["type"] = "acceptance"
        self._rewrite(path, value)
        with self.assertRaisesRegex(model.ContractError, "type is invalid"):
            repository.contracts()

    def test_canonical_digest_is_order_independent(self) -> None:
        self.assertEqual(
            model.canonical_sha256({"a": 1, "b": 2}),
            model.canonical_sha256({"b": 2, "a": 1}),
        )


if __name__ == "__main__":
    unittest.main()
