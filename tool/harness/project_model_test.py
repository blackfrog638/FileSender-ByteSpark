#!/usr/bin/env python3
"""Adversarial tests for the global project semantic model."""

from __future__ import annotations

import copy
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any, Dict

import project_model


OWNER = {
    "id": "project-owner",
    "name": "Project Owner",
    "email": "owner@example.com",
}


def dump(path: Path, value: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def approved_plan() -> Dict[str, Any]:
    plan = {
        "schema_version": 1,
        "id": "DP-EXAMPLE",
        "title": "Qualify the example outcome",
        "status": "approved",
        "source": {
            "kind": "implementation_plan",
            "path": "docs/example.md",
        },
        "requirements": [
            {
                "id": "REQ-EXAMPLE",
                "statement": "The example outcome is qualified.",
                "criteria": [
                    {
                        "id": "CRIT-EXAMPLE",
                        "statement": "The example behavior is observable.",
                        "negative_definitions": ["A skipped check does not qualify."],
                        "evidence": {
                            "gates": ["project_model"],
                            "scenarios": ["positive", "negative"],
                            "topology": "single_process",
                            "platforms": ["linux"],
                            "roles": ["project-owner"],
                            "allow_skipped": False,
                        },
                    }
                ],
                "implementation_tasks": ["XT-101"],
                "acceptance_owner": "XT-102",
            }
        ],
        "approval": {
            "approved_by": OWNER["id"],
            "approved_at": "2026-08-13T00:00:00Z",
            "content_sha256": "",
        },
    }
    plan["approval"]["content_sha256"] = project_model.plan_content_sha256(plan)
    return plan


class ProjectFixture:
    def __init__(self, testcase: unittest.TestCase) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        testcase.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.plan = approved_plan()
        self.blueprint = {
            "schema_version": 1,
            "id": "PROJECT-EXAMPLE",
            "title": "Example project",
            "mission": "Exercise strict project composition.",
            "state_order": [
                "absent",
                "specified",
                "implemented",
                "qualified",
            ],
            "goals": [
                {
                    "id": "GOAL-EXAMPLE",
                    "title": "Example goal",
                    "statement": "Deliver one qualified outcome.",
                    "milestones": ["MS-EXAMPLE"],
                }
            ],
            "milestones": [
                {
                    "id": "MS-EXAMPLE",
                    "title": "Example milestone",
                    "sequence": 10,
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
            "outcomes": [
                {
                    "id": "OUT-EXAMPLE",
                    "title": "Example outcome",
                    "milestone": "MS-EXAMPLE",
                    "capability": "CAP-EXAMPLE",
                    "statement": "The example outcome is qualified.",
                    "depends_on": [],
                    "invariants": ["INV-EXAMPLE"],
                    "criteria": [
                        {
                            "id": "CRIT-EXAMPLE",
                            "statement": "The example behavior is observable.",
                            "negative_definitions": [
                                "A skipped check does not qualify."
                            ],
                        }
                    ],
                    "target_state": "qualified",
                    "deferred_reason": None,
                }
            ],
        }
        self.invariants = {
            "schema_version": 1,
            "invariants": [
                {
                    "id": "INV-EXAMPLE",
                    "category": "correctness",
                    "statement": "The example behavior remains observable.",
                    "decision_refs": ["docs/adr/0001-example.md"],
                    "applies_to": ["OUT-EXAMPLE"],
                }
            ],
        }
        self.baseline = {
            "schema_version": 1,
            "states": [
                {
                    "outcome": "OUT-EXAMPLE",
                    "state": "specified",
                    "basis": "The behavior is specified but not currently qualified.",
                }
            ],
        }
        self.quality = {
            "schema_version": 1,
            "budgets": [
                {
                    "id": "QB-EXAMPLE",
                    "dimension": "correctness",
                    "statement": "The model check has no violations.",
                    "outcomes": ["OUT-EXAMPLE"],
                    "metric": "project_model_violations",
                    "operator": "equals",
                    "threshold": 0,
                    "unit": "violations",
                    "status": "active",
                    "gate": "project_model",
                    "deferred_reason": None,
                }
            ],
        }
        self.assets = {
            "schema_version": 1,
            "production_roots": ["product"],
            "units": [
                {
                    "id": "UNIT-EXAMPLE",
                    "title": "Example implementation unit",
                    "capabilities": ["CAP-EXAMPLE"],
                    "architecture_modules": ["core"],
                    "production_paths": ["product/**"],
                    "test_paths": ["tests/**"],
                    "classification": "requalify",
                    "qualification": "historical",
                    "evidence": ["docs/adr/0001-example.md"],
                    "rationale": (
                        "The canonical implementation remains in place while "
                        "current qualification evidence is collected."
                    ),
                    "follow_up_outcomes": ["OUT-EXAMPLE"],
                }
            ],
        }
        outcome = self.blueprint["outcomes"][0]
        self.change = {
            "schema_version": 1,
            "id": "BC-EXAMPLE",
            "title": "Qualify the example",
            "status": "draft",
            "depends_on": [],
            "transitions": [
                {
                    "plan": "DP-EXAMPLE",
                    "plan_revision": project_model.plan_revision_sha256(self.plan),
                    "outcome": "OUT-EXAMPLE",
                    "outcome_revision": project_model.canonical_sha256(outcome),
                    "from": "specified",
                    "to": "qualified",
                    "criteria": ["CRIT-EXAMPLE"],
                    "preserves": ["INV-EXAMPLE"],
                    "requires": [],
                }
            ],
            "approval": None,
        }
        self.write()

    @property
    def change_path(self) -> Path:
        return self.root / ".agents/project/changes/BC-EXAMPLE.json"

    def write(self) -> None:
        (self.root / "product").mkdir(parents=True, exist_ok=True)
        (self.root / "product/example.txt").write_text("example\n", encoding="utf-8")
        (self.root / "docs/adr").mkdir(parents=True, exist_ok=True)
        (self.root / "docs/adr/0001-example.md").write_text(
            "# Example decision\n", encoding="utf-8"
        )
        dump(self.root / ".agents/project/blueprint.json", self.blueprint)
        dump(self.root / ".agents/project/invariants.json", self.invariants)
        dump(
            self.root / ".agents/project/composition-baseline.json",
            self.baseline,
        )
        dump(self.root / ".agents/project/quality-budgets.json", self.quality)
        dump(self.root / ".agents/project/assets.json", self.assets)
        dump(self.change_path, self.change)
        dump(self.root / ".agents/plans/DP-EXAMPLE.json", self.plan)
        dump(
            self.root / ".agents/manifest.json",
            {
                "project_owner": OWNER,
            },
        )
        dump(
            self.root / ".agents/gates.json",
            {
                "gates": {
                    "project_model": {},
                }
            },
        )
        dump(
            self.root / ".agents/architecture/modules.json",
            {
                "schema_version": 1,
                "modules": [{"id": "core"}],
            },
        )

    def load(self, require_approved: bool = False) -> project_model.ProjectModel:
        return project_model.load_project(
            self.root,
            {"DP-EXAMPLE": self.plan},
            {"project_model"},
            OWNER["id"],
            {"core": {"id": "core"}},
            require_approved=require_approved,
        )

    def initialize_git(self, name: str, email: str) -> None:
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        subprocess.run(
            ["git", "-C", str(self.root), "config", "user.name", name],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(self.root), "config", "user.email", email],
            check=True,
        )


class ProjectModelTest(unittest.TestCase):
    def test_valid_model_and_generated_document_drift(self) -> None:
        fixture = ProjectFixture(self)
        project = fixture.load()
        self.assertEqual(project.projected_states["OUT-EXAMPLE"], "qualified")
        project_model.write_generated_documents(project)
        project_model.check_generated_documents(project)
        roadmap = fixture.root / "docs/roadmap.md"
        roadmap.write_text("stale\n", encoding="utf-8")
        with self.assertRaisesRegex(
            project_model.ProjectModelError, "generated document is stale"
        ):
            project_model.check_generated_documents(project)

    def test_rejects_stale_plan_and_outcome_revisions(self) -> None:
        fixture = ProjectFixture(self)
        fixture.change["transitions"][0]["plan_revision"] = "0" * 64
        fixture.write()
        with self.assertRaisesRegex(
            project_model.ProjectModelError, "stale Plan revision"
        ):
            fixture.load()

        fixture = ProjectFixture(self)
        fixture.change["transitions"][0]["outcome_revision"] = "0" * 64
        fixture.write()
        with self.assertRaisesRegex(
            project_model.ProjectModelError, "stale outcome revision"
        ):
            fixture.load()

    def test_rejects_outcome_cycle(self) -> None:
        fixture = ProjectFixture(self)
        second = {
            "id": "OUT-SECOND",
            "title": "Second outcome",
            "milestone": "MS-EXAMPLE",
            "capability": "CAP-EXAMPLE",
            "statement": "A second outcome exists.",
            "depends_on": [
                {
                    "outcome": "OUT-EXAMPLE",
                    "minimum_state": "specified",
                }
            ],
            "invariants": ["INV-EXAMPLE"],
            "criteria": [
                {
                    "id": "CRIT-SECOND",
                    "statement": "The second behavior is specified.",
                    "negative_definitions": [
                        "An absent specification does not qualify."
                    ],
                }
            ],
            "target_state": "specified",
            "deferred_reason": None,
        }
        fixture.blueprint["outcomes"].append(second)
        fixture.blueprint["milestones"][0]["outcomes"].append("OUT-SECOND")
        fixture.invariants["invariants"][0]["applies_to"].append("OUT-SECOND")
        fixture.blueprint["outcomes"][0]["depends_on"] = [
            {
                "outcome": "OUT-SECOND",
                "minimum_state": "specified",
            }
        ]
        fixture.baseline["states"].append(
            {
                "outcome": "OUT-SECOND",
                "state": "specified",
                "basis": "The second behavior is already specified.",
            }
        )
        fixture.write()
        with self.assertRaisesRegex(project_model.ProjectModelError, "cycle"):
            fixture.load()

    def test_rejects_duplicate_writer_and_invariant_gap(self) -> None:
        fixture = ProjectFixture(self)
        fixture.change["transitions"].append(
            copy.deepcopy(fixture.change["transitions"][0])
        )
        fixture.write()
        with self.assertRaisesRegex(
            project_model.ProjectModelError, "multiple writers"
        ):
            fixture.load()

        fixture = ProjectFixture(self)
        fixture.change["transitions"][0]["preserves"] = []
        fixture.write()
        with self.assertRaisesRegex(
            project_model.ProjectModelError, "preserve every Blueprint invariant"
        ):
            fixture.load()

    def test_rejects_plan_semantic_weakening(self) -> None:
        fixture = ProjectFixture(self)
        criterion = fixture.plan["requirements"][0]["criteria"][0]
        criterion["statement"] = "A weaker behavior is observable."
        fixture.plan["approval"]["content_sha256"] = project_model.plan_content_sha256(
            fixture.plan
        )
        fixture.change["transitions"][0][
            "plan_revision"
        ] = project_model.plan_revision_sha256(fixture.plan)
        fixture.write()
        with self.assertRaisesRegex(
            project_model.ProjectModelError, "weakens or changes"
        ):
            fixture.load()

    def test_rejects_unowned_production_asset(self) -> None:
        fixture = ProjectFixture(self)
        fixture.assets["units"][0]["production_paths"] = ["product/example.txt"]
        fixture.write()
        (fixture.root / "product/unowned.txt").write_text("unowned\n", encoding="utf-8")
        with self.assertRaisesRegex(
            project_model.ProjectModelError, "must have one unit owner"
        ):
            fixture.load()

    def test_rejects_non_owner_approval_identity(self) -> None:
        fixture = ProjectFixture(self)
        fixture.initialize_git("Other User", "other@example.com")
        with self.assertRaisesRegex(
            project_model.ProjectModelError, "configured project owner"
        ):
            project_model.approve_change(
                fixture.root,
                fixture.change_path,
                OWNER,
                "2026-08-13T01:00:00Z",
            )

    def test_project_approval_requires_approved_changes(self) -> None:
        fixture = ProjectFixture(self)
        fixture.initialize_git(OWNER["name"], OWNER["email"])
        with self.assertRaisesRegex(
            project_model.ProjectModelError,
            "requires approved Blueprint changes",
        ):
            project_model.approve_project(
                fixture.root,
                OWNER,
                "2026-08-13T01:00:00Z",
            )

    def test_owner_approval_round_trip(self) -> None:
        fixture = ProjectFixture(self)
        fixture.initialize_git(OWNER["name"], OWNER["email"])
        project_model.approve_change(
            fixture.root,
            fixture.change_path,
            OWNER,
            "2026-08-13T01:00:00Z",
        )
        project_model.approve_project(
            fixture.root,
            OWNER,
            "2026-08-13T01:01:00Z",
        )
        project = project_model.load_repository_project(
            fixture.root,
            require_approved=True,
        )
        self.assertIsNotNone(project.approval_digest)


if __name__ == "__main__":
    unittest.main()
