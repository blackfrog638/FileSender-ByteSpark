#!/usr/bin/env python3
"""Shared minimal repository fixture for Harness V2 tests."""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any, Dict, Optional


HARNESS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HARNESS))

import model  # noqa: E402
import project_model  # noqa: E402


def _leaf(argv: list[str], inputs: list[str]) -> Dict[str, Any]:
    return {
        "command": {
            "argv": argv,
            "timeout_seconds": 30,
            "environment": {},
        },
        "aggregate": None,
        "inputs": inputs,
        "resource_group": "lightweight",
        "platforms": ["local", "linux"],
        "cache": "success_only",
    }


class Repository:
    def __init__(
        self,
        testcase: unittest.TestCase,
        *,
        remote: bool = False,
    ) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        testcase.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.external = self.root.parent / "{}-external".format(self.root.name)
        self.external.mkdir()
        testcase.addCleanup(lambda: shutil.rmtree(self.external, ignore_errors=True))
        self.remote_path: Optional[Path] = None
        self._write_contracts()
        self.git("init", "-q", "-b", "harness")
        self.git("config", "user.name", "Test User")
        self.git("config", "user.email", "test@example.com")
        self.commit("chore: initialize fixture")
        if remote:
            self.remote_path = self.root.parent / "{}-remote.git".format(self.root.name)
            subprocess.run(
                ["git", "init", "-q", "--bare", str(self.remote_path)],
                check=True,
            )
            testcase.addCleanup(
                lambda: shutil.rmtree(self.remote_path, ignore_errors=True)
            )
            self.git("remote", "add", "origin", str(self.remote_path))
            self.git("push", "-q", "-u", "origin", "harness")

    def _dump(self, path: Path, value: Dict[str, Any]) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps(value, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )

    def _write_contracts(self) -> None:
        agents = self.root / ".agents"
        (agents / "plans").mkdir(parents=True)
        (agents / "tasks").mkdir()
        (agents / "architecture").mkdir()
        (agents / "project" / "changes").mkdir(parents=True)
        (self.root / "product" / "tests").mkdir(parents=True)
        (self.root / "product" / "example.txt").write_text(
            "example\n",
            encoding="utf-8",
        )
        (self.root / "docs" / "adr").mkdir(parents=True)
        (self.root / "docs" / "adr" / "0001-example.md").write_text(
            "# Example\n",
            encoding="utf-8",
        )
        workflow = self.root / ".github" / "workflows"
        workflow.mkdir(parents=True)
        (workflow / "merge-queue.yml").write_text(
            "name: Harness V2 Merge Queue\n",
            encoding="utf-8",
        )
        self._dump(
            agents / "manifest.json",
            {
                "schema_version": 1,
                "harness_version": 2,
                "project": "fixture",
                "integration_branch": "harness",
                "queue_namespace": "refs/heads/queue/",
            },
        )
        self._dump(
            agents / "commit-identity.json",
            {
                "schema_version": 2,
                "name": "Delivery Bot",
                "email": "delivery@example.com",
                "immutable": True,
            },
        )
        fingerprint = "FAILED: expected feature behavior is unavailable"
        feature_command = (
            "from pathlib import Path; import sys; "
            "red=Path('product/tests/red.txt').exists(); "
            "green=Path('product/impl.txt').exists(); "
            "print({!r}) if red and not green else None; "
            "sys.exit(1 if red and not green else 0)"
        ).format(fingerprint)
        gates = {
            "schema_version": 1,
            "resource_groups": {"lightweight": {"max_parallel": 2}},
            "gates": {
                "governance": _leaf(
                    ["python3", "-c", "print('governance')"],
                    [".agents/**"],
                ),
                "feature_test": _leaf(
                    ["python3", "-c", feature_command],
                    ["product/**"],
                ),
                "verify": {
                    "command": None,
                    "aggregate": ["governance", "feature_test"],
                    "inputs": [],
                    "resource_group": None,
                    "platforms": ["local"],
                    "cache": "disabled",
                },
            },
        }
        self._dump(agents / "gates.json", gates)
        self._dump(
            agents / "risk-routing.json",
            {
                "schema_version": 1,
                "path_rules": [
                    {
                        "paths": ["product/**"],
                        "minimum_risk": {"functionality": "high"},
                        "required_gates": ["feature_test"],
                    }
                ],
                "phase_minimums": {
                    "review": ["governance"],
                    "queue": ["verify"],
                },
            },
        )
        self._dump(
            agents / "architecture" / "modules.json",
            {"schema_version": 1, "modules": []},
        )
        criterion = {
            "id": "CRIT-EXAMPLE",
            "statement": "The example behavior is available.",
            "negative_definitions": ["A skipped test does not qualify."],
        }
        plan = {
            "schema_version": 1,
            "id": "DP-EXAMPLE",
            "title": "Example delivery",
            "source": {
                "kind": "test",
                "path": "docs/roadmap.md",
            },
            "requirements": [
                {
                    "id": "REQ-EXAMPLE",
                    "statement": "Deliver the example behavior.",
                    "criteria": [
                        {
                            **criterion,
                            "evidence": {
                                "gates": ["feature_test"],
                                "scenarios": ["positive", "negative"],
                                "topology": "single_process",
                                "platforms": ["linux"],
                                "roles": ["developer"],
                                "allow_skipped": False,
                            },
                        }
                    ],
                    "implementation_tasks": ["XT-101"],
                }
            ],
        }
        self._dump(agents / "plans" / "DP-EXAMPLE.json", plan)
        self._dump(
            agents / "tasks" / "XT-101.json",
            {
                "schema_version": 1,
                "id": "XT-101",
                "title": "Implement the example behavior",
                "plan": "DP-EXAMPLE",
                "criteria": ["CRIT-EXAMPLE"],
                "depends_on": [],
                "owned_paths": ["product/**"],
                "type": "feature",
                "workstream": "example",
                "risk": {
                    "functionality": "high",
                    "security": "none",
                    "performance": "none",
                    "compatibility": "none",
                    "concurrency": "none",
                    "platform": "none",
                    "persistence": "none",
                },
                "tdd": {
                    "mode": "red_green",
                    "gate": "feature_test",
                    "proof_paths": ["product/tests/**"],
                    "oracle_paths": [],
                    "failure_fingerprints": [fingerprint],
                },
                "delivery": {
                    "commit_type": "feat",
                    "scope": "example",
                    "summary": "implement example behavior",
                    "architecture_change": {
                        "mode": "none",
                        "modules": [],
                        "supersedes": {
                            "paths": [],
                            "symbols": [],
                            "targets": [],
                        },
                        "temporary_leases": [],
                        "retires_leases": [],
                    },
                },
            },
        )
        outcome = {
            "id": "OUT-EXAMPLE",
            "title": "Example outcome",
            "milestone": "MS-EXAMPLE",
            "capability": "CAP-EXAMPLE",
            "statement": "The example behavior is delivered.",
            "depends_on": [],
            "invariants": ["INV-EXAMPLE"],
            "criteria": [criterion],
            "target_state": "qualified",
            "deferred_reason": None,
        }
        blueprint = {
            "schema_version": 1,
            "id": "BP-EXAMPLE",
            "title": "Example Blueprint",
            "mission": "Exercise the Harness V2 contracts.",
            "state_order": ["absent", "specified", "implemented", "qualified"],
            "goals": [
                {
                    "id": "GOAL-EXAMPLE",
                    "title": "Example goal",
                    "statement": "Deliver one example.",
                    "milestones": ["MS-EXAMPLE"],
                }
            ],
            "milestones": [
                {
                    "id": "MS-EXAMPLE",
                    "title": "Example milestone",
                    "sequence": 1,
                    "outcomes": ["OUT-EXAMPLE"],
                }
            ],
            "capabilities": [
                {
                    "id": "CAP-EXAMPLE",
                    "title": "Example capability",
                    "depends_on": [],
                    "implementation_units": ["UNIT-EXAMPLE"],
                }
            ],
            "outcomes": [outcome],
        }
        self._dump(agents / "project" / "blueprint.json", blueprint)
        self._dump(
            agents / "project" / "invariants.json",
            {
                "schema_version": 1,
                "invariants": [
                    {
                        "id": "INV-EXAMPLE",
                        "category": "correctness",
                        "statement": "The example remains deterministic.",
                        "decision_refs": ["docs/adr/0001-example.md"],
                        "applies_to": ["OUT-EXAMPLE"],
                    }
                ],
            },
        )
        self._dump(
            agents / "project" / "composition-baseline.json",
            {
                "schema_version": 1,
                "states": [
                    {
                        "outcome": "OUT-EXAMPLE",
                        "state": "specified",
                        "basis": "The test fixture declares the behavior.",
                    }
                ],
            },
        )
        self._dump(
            agents / "project" / "quality-budgets.json",
            {
                "schema_version": 1,
                "budgets": [
                    {
                        "id": "BUDGET-EXAMPLE",
                        "dimension": "latency",
                        "statement": "Performance is deferred in this fixture.",
                        "outcomes": ["OUT-EXAMPLE"],
                        "metric": "duration",
                        "operator": "<=",
                        "threshold": None,
                        "unit": "seconds",
                        "status": "deferred",
                        "gate": None,
                        "deferred_reason": "The fixture exercises control-plane behavior.",
                    }
                ],
            },
        )
        self._dump(
            agents / "project" / "assets.json",
            {
                "schema_version": 1,
                "production_roots": ["product"],
                "units": [
                    {
                        "id": "UNIT-EXAMPLE",
                        "title": "Example implementation",
                        "capabilities": ["CAP-EXAMPLE"],
                        "architecture_modules": [],
                        "production_paths": ["product/**"],
                        "test_paths": ["product/tests/**"],
                        "classification": "requalify",
                        "qualification": "unqualified",
                        "evidence": ["product/example.txt"],
                        "rationale": "The fixture implementation exists and requires qualification.",
                        "follow_up_outcomes": ["OUT-EXAMPLE"],
                    }
                ],
            },
        )
        plan_revision = hashlib.sha256(
            json.dumps(
                plan,
                ensure_ascii=False,
                allow_nan=False,
                separators=(",", ":"),
                sort_keys=True,
            ).encode("utf-8")
        ).hexdigest()
        outcome_revision = project_model.canonical_sha256(outcome)
        self._dump(
            agents / "project" / "changes" / "BC-EXAMPLE.json",
            {
                "schema_version": 1,
                "id": "BC-EXAMPLE",
                "title": "Deliver the example",
                "depends_on": [],
                "transitions": [
                    {
                        "plan": "DP-EXAMPLE",
                        "plan_revision": plan_revision,
                        "outcome": "OUT-EXAMPLE",
                        "outcome_revision": outcome_revision,
                        "from": "specified",
                        "to": "qualified",
                        "criteria": ["CRIT-EXAMPLE"],
                        "preserves": ["INV-EXAMPLE"],
                        "requires": [],
                    }
                ],
            },
        )
        project = project_model.load_project(
            self.root,
            {"DP-EXAMPLE": plan},
            set(gates["gates"]),
            {},
        )
        project_model.write_generated_documents(project)

    def git(self, *arguments: str) -> str:
        result = subprocess.run(
            ["git", "-C", str(self.root), *arguments],
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout.strip()

    def commit(self, message: str, *, allow_empty: bool = False) -> str:
        self.git("add", ".")
        arguments = ["commit", "-q"]
        if allow_empty:
            arguments.append("--allow-empty")
        arguments.extend(["-m", message])
        self.git(*arguments)
        return self.git("rev-parse", "HEAD")

    def commit_in(self, path: Path, message: str) -> str:
        subprocess.run(["git", "-C", str(path), "add", "."], check=True)
        subprocess.run(
            ["git", "-C", str(path), "commit", "-q", "-m", message],
            check=True,
        )
        return subprocess.run(
            ["git", "-C", str(path), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()

    def contracts(self) -> model.ContractSet:
        return model.load_contracts(self.root)
