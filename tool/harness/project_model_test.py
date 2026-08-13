#!/usr/bin/env python3
"""Focused adversarial tests for the Project Blueprint composition."""

from __future__ import annotations

import copy
import unittest
from pathlib import Path

import project_model


ROOT = Path(__file__).resolve().parents[2]


class ProjectModelTest(unittest.TestCase):
    def test_repository_model_and_generated_documents_are_current(self) -> None:
        project = project_model.load_repository_project(ROOT)
        project_model.check_generated_documents(project)
        self.assertTrue(project.outcomes)
        self.assertEqual(set(project.plan_changes), set(project_model._plans(ROOT)))

    def test_plan_revision_binds_exact_normative_json(self) -> None:
        plan = next(iter(project_model._plans(ROOT).values()))
        original = project_model.plan_revision_sha256(plan)
        changed = copy.deepcopy(plan)
        changed["title"] += " changed"
        self.assertNotEqual(
            original,
            project_model.plan_revision_sha256(changed),
        )
        changed["approval"] = {"approved_by": "someone"}
        self.assertNotEqual(
            original,
            project_model.plan_revision_sha256(changed),
        )

    def test_change_contract_rejects_approval_fields(self) -> None:
        project = project_model.load_repository_project(ROOT)
        change = copy.deepcopy(next(iter(project.changes.values())))
        change["status"] = "approved"
        with self.assertRaisesRegex(
            project_model.ProjectModelError,
            "extra=.*status",
        ):
            project_model._exact(
                change,
                project_model.CHANGE_FIELDS,
                "change",
            )

    def test_blueprint_graph_rejects_cycles(self) -> None:
        with self.assertRaisesRegex(
            project_model.ProjectModelError,
            "contains a cycle",
        ):
            project_model._topological(
                {"A": ["B"], "B": ["A"]},
                "fixture graph",
            )

    def test_plan_cannot_weaken_blueprint_criterion(self) -> None:
        project = project_model.load_repository_project(ROOT)
        plan = copy.deepcopy(next(iter(project_model._plans(ROOT).values())))
        criterion = plan["requirements"][0]["criteria"][0]
        criterion["negative_definitions"] = ["weaker"]
        index = project_model._criterion_index(project.outcomes)
        with self.assertRaisesRegex(
            project_model.ProjectModelError,
            "weakens or changes",
        ):
            project_model._validate_plan_semantics(plan, index)


if __name__ == "__main__":
    unittest.main()
