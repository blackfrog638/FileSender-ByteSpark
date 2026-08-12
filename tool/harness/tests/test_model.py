#!/usr/bin/env python3
"""Tests for Harness V2 static contracts."""

from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any, Dict, Tuple

HARNESS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HARNESS))

import model  # noqa: E402


def leaf(argv: list, inputs: list, group: str = "lightweight") -> Dict[str, Any]:
    return {
        "command": {
            "argv": argv,
            "timeout_seconds": 30,
            "environment": {},
        },
        "aggregate": None,
        "inputs": inputs,
        "resource_group": group,
        "platforms": ["local", "linux"],
        "cache": "success_only",
    }


def aggregate(dependencies: list) -> Dict[str, Any]:
    return {
        "command": None,
        "aggregate": dependencies,
        "inputs": [],
        "resource_group": None,
        "platforms": ["local"],
        "cache": "disabled",
    }


def task(
    task_id: str,
    task_type: str,
    dependencies: list,
    owned_paths: list,
) -> Dict[str, Any]:
    if task_type == "feature":
        tdd = {
            "mode": "red_green",
            "gate": "feature_test",
            "proof_paths": ["native/tests/**"],
            "oracle_paths": [],
            "failure_fingerprints": [
                "FAILED: expected feature behavior is unavailable"
            ],
        }
    else:
        tdd = {
            "mode": "evidence_closure",
            "gate": None,
            "proof_paths": [],
            "oracle_paths": [],
            "failure_fingerprints": [],
        }
    return {
        "schema_version": 1,
        "id": task_id,
        "title": "Task {}".format(task_id),
        "plan": "DP-EXAMPLE",
        "criteria": ["CRIT-EXAMPLE-BEHAVIOR"],
        "depends_on": dependencies,
        "owned_paths": owned_paths,
        "type": task_type,
        "workstream": "integration",
        "risk": {
            "functionality": "high",
            "security": "high" if task_type == "feature" else "none",
            "performance": "none",
            "compatibility": "none",
            "concurrency": "none",
            "platform": "low",
            "persistence": "none",
        },
        "tdd": tdd,
        "delivery": {
            "commit_type": "feat" if task_type == "feature" else "test",
            "scope": "harness",
            "summary": "deliver {}".format(task_id.lower()),
            "architecture_change": {"mode": "none", "modules": []},
        },
    }


def valid_documents() -> Tuple[Dict[str, Any], ...]:
    manifest = {
        "schema_version": 1,
        "harness_version": 2,
        "project": "test-project",
        "integration_branch": "harness",
        "project_owner": {
            "id": "project-owner",
            "name": "Project Owner",
            "email": "owner@example.com",
        },
        "ref_namespaces": {
            "state": "refs/heads/state/",
            "submit": "refs/heads/submit/",
            "queue": "refs/heads/queue/",
            "attest": "refs/heads/attest/",
            "archive": "refs/heads/archive/",
        },
    }
    gates = {
        "schema_version": 1,
        "resource_groups": {
            "lightweight": {"max_parallel": 4},
            "native_build": {"max_parallel": 1},
        },
        "gates": {
            "governance": leaf(
                ["python3", "-c", "print('governance')"], [".agents/**"]
            ),
            "feature_test": leaf(
                ["python3", "-c", "print('feature')"],
                ["native/**"],
                "native_build",
            ),
            "verify": aggregate(["governance", "feature_test"]),
        },
    }
    routing = {
        "schema_version": 1,
        "path_rules": [
            {
                "paths": ["native/**"],
                "minimum_risk": {"security": "high"},
                "required_gates": ["feature_test"],
            }
        ],
        "phase_minimums": {
            "review": ["governance"],
            "queue": ["verify"],
        },
    }
    plan = {
        "schema_version": 1,
        "id": "DP-EXAMPLE",
        "title": "Example delivery",
        "status": "approved",
        "source": {"kind": "roadmap", "path": "docs/roadmap.md"},
        "requirements": [
            {
                "id": "REQ-EXAMPLE",
                "statement": "Deliver one observable behavior.",
                "criteria": [
                    {
                        "id": "CRIT-EXAMPLE-BEHAVIOR",
                        "statement": "The example behavior is observable.",
                        "negative_definitions": [
                            "A skipped test does not qualify."
                        ],
                        "evidence": {
                            "gates": ["feature_test"],
                            "scenarios": ["positive", "negative"],
                            "topology": "single_process",
                            "platforms": ["linux"],
                            "roles": [],
                            "allow_skipped": False,
                        },
                    }
                ],
                "implementation_tasks": ["XT-101"],
                "acceptance_owner": "XT-102",
            }
        ],
        "approval": {
            "approved_by": "project-owner",
            "approved_at": "2026-08-12T00:00:00Z",
            "content_sha256": "",
        },
    }
    plan["approval"]["content_sha256"] = model.plan_content_sha256(plan)
    implementation = task("XT-101", "feature", [], ["native/**"])
    acceptance = task(
        "XT-102",
        "acceptance",
        ["XT-101"],
        [".agents/tasks/XT-102.json"],
    )
    return manifest, gates, routing, plan, implementation, acceptance


class ContractFixture:
    def __init__(self, testcase: unittest.TestCase) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        testcase.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        agents = self.root / ".agents"
        (agents / "plans").mkdir(parents=True)
        (agents / "tasks").mkdir()
        (
            self.manifest,
            self.gates,
            self.routing,
            self.plan,
            self.implementation,
            self.acceptance,
        ) = copy.deepcopy(valid_documents())
        self.write()

    def _dump(self, path: Path, value: Dict[str, Any]) -> None:
        path.write_text(
            json.dumps(value, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )

    def write(self) -> None:
        agents = self.root / ".agents"
        self._dump(agents / "manifest.json", self.manifest)
        self._dump(agents / "gates.json", self.gates)
        self._dump(agents / "risk-routing.json", self.routing)
        self._dump(agents / "plans" / "DP-EXAMPLE.json", self.plan)
        self._dump(agents / "tasks" / "XT-101.json", self.implementation)
        self._dump(agents / "tasks" / "XT-102.json", self.acceptance)


class ModelTest(unittest.TestCase):
    def test_loads_valid_contract_set(self) -> None:
        fixture = ContractFixture(self)
        contracts = model.load_contracts(fixture.root)
        self.assertEqual(set(contracts.tasks), {"XT-101", "XT-102"})
        self.assertEqual(set(contracts.gates), {"governance", "feature_test", "verify"})

    def test_canonical_digest_is_order_independent(self) -> None:
        left = {"a": 1, "b": {"c": ["é", False]}}
        right = {"b": {"c": ["é", False]}, "a": 1}
        self.assertEqual(model.canonical_sha256(left), model.canonical_sha256(right))

    def test_rejects_duplicate_json_keys(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "duplicate.json"
            path.write_text('{"a": 1, "a": 2}\n', encoding="utf-8")
            with self.assertRaisesRegex(model.ContractError, "duplicate JSON key"):
                model.load_json(path)

    def test_rejects_unknown_task_field_and_task_authored_command(self) -> None:
        for field, value in (("state", "done"), ("command", "rm -rf /")):
            with self.subTest(field=field):
                fixture = ContractFixture(self)
                fixture.implementation[field] = value
                fixture.write()
                with self.assertRaisesRegex(model.ContractError, "invalid fields"):
                    model.load_contracts(fixture.root)

    def test_rejects_gate_cycle_and_duplicate_command(self) -> None:
        fixture = ContractFixture(self)
        fixture.gates["gates"]["verify"]["aggregate"] = ["verify"]
        fixture.write()
        with self.assertRaisesRegex(model.ContractError, "cycle"):
            model.load_contracts(fixture.root)

        fixture = ContractFixture(self)
        fixture.gates["gates"]["duplicate"] = copy.deepcopy(
            fixture.gates["gates"]["governance"]
        )
        fixture.write()
        with self.assertRaisesRegex(model.ContractError, "duplicate one command"):
            model.load_contracts(fixture.root)

    def test_rejects_lower_risk_and_unknown_evidence_gate(self) -> None:
        fixture = ContractFixture(self)
        fixture.implementation["risk"]["security"] = "medium"
        fixture.write()
        with self.assertRaisesRegex(model.ContractError, "lowers routed security"):
            model.load_contracts(fixture.root)

        fixture = ContractFixture(self)
        fixture.plan["requirements"][0]["criteria"][0]["evidence"]["gates"] = [
            "invented"
        ]
        fixture.plan["approval"]["content_sha256"] = model.plan_content_sha256(
            fixture.plan
        )
        fixture.write()
        with self.assertRaisesRegex(model.ContractError, "unknown gate"):
            model.load_contracts(fixture.root)

    def test_rejects_stale_approval_digest_and_free_text_approver(self) -> None:
        fixture = ContractFixture(self)
        fixture.plan["title"] = "Changed after approval"
        fixture.write()
        with self.assertRaisesRegex(model.ContractError, "digest does not match"):
            model.load_contracts(fixture.root)

        fixture = ContractFixture(self)
        fixture.plan["approval"]["approved_by"] = "integration-owner"
        fixture.plan["approval"]["content_sha256"] = model.plan_content_sha256(
            fixture.plan
        )
        fixture.write()
        with self.assertRaisesRegex(model.ContractError, "configured project owner"):
            model.load_contracts(fixture.root)

    def test_rejects_unclosed_acceptance_dependencies(self) -> None:
        fixture = ContractFixture(self)
        fixture.acceptance["depends_on"] = []
        fixture.write()
        with self.assertRaisesRegex(model.ContractError, "does not depend"):
            model.load_contracts(fixture.root)

    def test_rejects_foreign_or_unmapped_criterion(self) -> None:
        fixture = ContractFixture(self)
        fixture.implementation["criteria"] = ["CRIT-FOREIGN"]
        fixture.write()
        with self.assertRaisesRegex(model.ContractError, "unknown or foreign"):
            model.load_contracts(fixture.root)

        fixture = ContractFixture(self)
        fixture.implementation["criteria"] = []
        fixture.write()
        with self.assertRaisesRegex(model.ContractError, "must not be empty"):
            model.load_contracts(fixture.root)

    def test_rejects_unordered_overlapping_task_paths(self) -> None:
        fixture = ContractFixture(self)
        fixture.acceptance["owned_paths"] = ["native/tests/**"]
        fixture.acceptance["risk"]["security"] = "high"
        fixture.write()
        # XT-102 depends on XT-101, so the overlap is ordered and legal.
        model.load_contracts(fixture.root)
        fixture.acceptance["depends_on"] = []
        fixture.write()
        with self.assertRaises(model.ContractError):
            model.load_contracts(fixture.root)

    def test_approval_uses_configured_git_identity(self) -> None:
        fixture = ContractFixture(self)
        fixture.plan["status"] = "draft"
        fixture.plan["approval"] = None
        fixture.write()
        subprocess.run(["git", "init", "-q", str(fixture.root)], check=True)
        subprocess.run(
            ["git", "-C", str(fixture.root), "config", "user.email", "owner@example.com"],
            check=True,
        )
        digest = model.approve_plan(
            fixture.root,
            fixture.root / ".agents" / "plans" / "DP-EXAMPLE.json",
            "2026-08-12T12:00:00Z",
        )
        approved = model.load_json(
            fixture.root / ".agents" / "plans" / "DP-EXAMPLE.json"
        )
        self.assertEqual(approved["status"], "approved")
        self.assertEqual(approved["approval"]["content_sha256"], digest)

    def test_approval_rejects_non_owner_identity(self) -> None:
        fixture = ContractFixture(self)
        fixture.plan["status"] = "draft"
        fixture.plan["approval"] = None
        fixture.write()
        subprocess.run(["git", "init", "-q", str(fixture.root)], check=True)
        subprocess.run(
            ["git", "-C", str(fixture.root), "config", "user.email", "agent@example.com"],
            check=True,
        )
        with self.assertRaisesRegex(model.ContractError, "not the configured"):
            model.approve_plan(
                fixture.root,
                fixture.root / ".agents" / "plans" / "DP-EXAMPLE.json",
                "2026-08-12T12:00:00Z",
            )


if __name__ == "__main__":
    unittest.main()
