#!/usr/bin/env python3

"""Focused tests for Delivery Plan governance."""

from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
HARNESS_DIR = Path(__file__).resolve().parent
if str(HARNESS_DIR) not in sys.path:
    sys.path.insert(0, str(HARNESS_DIR))

import delivery_plan


def write_json(path: Path, value: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


class DeliveryPlanTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        (self.root / ".agents" / "tasks").mkdir(parents=True)
        (self.root / ".agents" / "records").mkdir(parents=True)
        (self.root / ".agents" / "plans").mkdir(parents=True)
        (self.root / "docs").mkdir()
        (self.root / ".agents" / "manifest.yaml").write_text(
            """schema_version: 1
delivery_plans:
  directory: .agents/plans
  required_from_task: XT-064
commands:
  verify: make verify
  native_test: make native-test
""",
            encoding="utf-8",
        )
        (self.root / "docs" / "roadmap.md").write_text(
            """# Roadmap

<!-- roadmap-id: RM-P1-TRANSFER -->
- [ ] Transfer one file
""",
            encoding="utf-8",
        )
        self.tasks = [
            self.task(
                "XT-063",
                "Delivery plan governance",
                [],
                ["tool/harness/**"],
                plan_bound=False,
            ),
            self.task(
                "XT-064",
                "Implement transfer",
                ["XT-063"],
                ["native/src/transfer/**"],
                role="implementation",
            ),
            self.task(
                "XT-065",
                "Accept transfer",
                ["XT-064"],
                ["docs/roadmap.md"],
                role="acceptance",
            ),
        ]
        self.plan = {
            "schema_version": 1,
            "id": "DP-P1-TRANSFER",
            "title": "P1 transfer",
            "status": "approved",
            "source": {"kind": "roadmap", "path": "docs/roadmap.md"},
            "requirements": [
                {
                    "id": "REQ-P1-TRANSFER",
                    "source_ref": "RM-P1-TRANSFER",
                    "statement": "Transfer one accepted file.",
                    "acceptance_criteria": [
                        "The receiver commits exactly the offered bytes."
                    ],
                    "implementation_tasks": ["XT-064"],
                    "acceptance_task": "XT-065",
                }
            ],
            "approval": {
                "approved_by": "integration-owner",
                "approved_at": "2026-08-09T00:00:00+00:00",
                "content_sha256": "",
            },
            "superseded_by": "",
        }
        self.plan["approval"]["content_sha256"] = delivery_plan.approval_digest(
            self.plan
        )
        self.flush()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def task(
        self,
        task_id: str,
        title: str,
        dependencies: list[str],
        owned_paths: list[str],
        *,
        plan_bound: bool = True,
        role: str = "implementation",
    ) -> dict[str, object]:
        task: dict[str, object] = {
            "id": task_id,
            "title": title,
            "readiness": "ready",
            "workstream": "integration",
            "depends_on": dependencies,
            "owned_paths": owned_paths,
        }
        if plan_bound:
            task.update(
                {
                    "delivery_plan": "DP-P1-TRANSFER",
                    "requirement_ids": ["REQ-P1-TRANSFER"],
                    "delivery_role": role,
                }
            )
        return task

    def flush(self) -> None:
        write_json(
            self.root / ".agents" / "backlog.yaml",
            {"schema_version": 1, "tasks": self.tasks},
        )
        write_json(
            self.root / ".agents" / "plans" / "DP-P1-TRANSFER.json",
            self.plan,
        )
        for task in self.tasks:
            task_id = str(task["id"])
            self.write_spec(task)
            record: dict[str, object] = {
                "schema_version": 3,
                "id": task_id,
                "state": "done" if task_id == "XT-063" else "ready",
                "owner": "test-owner" if task_id == "XT-063" else "unassigned",
                "verification": {"status": "passed" if task_id == "XT-063" else "pending"},
            }
            if task.get("delivery_plan"):
                record.update(
                    {
                        "delivery_plan": task["delivery_plan"],
                        "requirement_ids": task["requirement_ids"],
                        "delivery_role": task["delivery_role"],
                    }
                )
            write_json(
                self.root / ".agents" / "records" / f"{task_id}.json",
                record,
            )

    def write_spec(self, task: dict[str, object], *, title: str | None = None) -> None:
        task_id = str(task["id"])

        def yaml_list(values: object) -> str:
            assert isinstance(values, list)
            if not values:
                return " []"
            return "\n" + "\n".join(f"  - {value}" for value in values)

        binding = ""
        if task.get("delivery_plan"):
            binding = (
                f"delivery_plan: {task['delivery_plan']}\n"
                f"requirement_ids:{yaml_list(task['requirement_ids'])}\n"
                f"delivery_role: {task['delivery_role']}\n"
            )
        path = (
            self.root
            / ".agents"
            / "tasks"
            / f"{task_id}-{task_id.lower()}.md"
        )
        path.write_text(
            f"""---
id: {task_id}
title: {title or task["title"]}
state: ready
workstream: {task["workstream"]}
owner: unassigned
depends_on:{yaml_list(task["depends_on"])}
owned_paths:{yaml_list(task["owned_paths"])}
{binding}contract_changes: []
---

## Outcome

Exercise one complete planning contract.
""",
            encoding="utf-8",
        )

    def assert_error(self, text: str) -> None:
        errors = delivery_plan.validate_repository(self.root)
        self.assertTrue(
            any(text in error for error in errors),
            f"missing {text!r} in {errors!r}",
        )

    def upgrade_plan_to_v2(self) -> None:
        requirement = self.plan["requirements"][0]
        assert isinstance(requirement, dict)
        requirement.pop("acceptance_criteria")
        requirement.pop("implementation_tasks")
        requirement["criteria"] = [
            {
                "id": "CRIT-P1-TRANSFER-EXACT-BYTES",
                "statement": "The receiver commits exactly the offered bytes.",
                "negative_definitions": [
                    "A transfer that commits before integrity verification fails."
                ],
                "implementation_tasks": ["XT-064"],
                "evidence": [
                    {
                        "id": "EVD-P1-TRANSFER-NATIVE",
                        "producer_task": "XT-064",
                        "gate": "native_test",
                        "level": "integration",
                        "required_scenarios": ["transfer.explicit_accept"],
                        "required_assertions": ["destination.exact_bytes"],
                        "required_platforms": [],
                        "required_roles": ["sender", "receiver"],
                        "topology": "two_process",
                        "allow_skipped": False,
                    }
                ],
            }
        ]
        self.plan["schema_version"] = 2
        self.plan["approval"]["content_sha256"] = delivery_plan.approval_digest(
            self.plan
        )
        self.flush()

    def test_valid_plan_and_claim_dependency_gate(self) -> None:
        self.assertEqual(delivery_plan.validate_repository(self.root), [])
        self.assertEqual(delivery_plan.validate_claim(self.root, "XT-064"), [])
        claim_errors = delivery_plan.validate_claim(self.root, "XT-065")
        self.assertTrue(any("XT-064 is ready, not done" in error for error in claim_errors))

    def test_draft_plan_blocks_ready_task_and_claim(self) -> None:
        self.plan["status"] = "draft"
        self.plan["approval"] = {
            "approved_by": "",
            "approved_at": "",
            "content_sha256": "",
        }
        self.flush()
        self.assert_error("must not be ready while DP-P1-TRANSFER is draft")
        self.tasks[1]["readiness"] = "blocked"
        self.tasks[2]["readiness"] = "blocked"
        self.flush()
        self.assertEqual(delivery_plan.validate_repository(self.root), [])
        claim_errors = delivery_plan.validate_claim(self.root, "XT-064")
        self.assertTrue(any("requires an approved" in error for error in claim_errors))

    def test_required_task_cannot_omit_plan_binding(self) -> None:
        for field in ("delivery_plan", "requirement_ids", "delivery_role"):
            self.tasks[1].pop(field)
        self.flush()
        self.assert_error("XT-064 requires Delivery Plan metadata")

    def test_unknown_and_one_sided_mappings_are_rejected(self) -> None:
        self.plan["requirements"][0]["implementation_tasks"].append("XT-999")
        self.flush()
        self.assert_error("references unknown task XT-999")
        self.plan["requirements"][0]["implementation_tasks"] = ["XT-064"]
        self.tasks[1]["requirement_ids"] = ["REQ-OTHER"]
        self.flush()
        self.assert_error("requirement_ids do not match")

    def test_dependency_cycle_is_rejected(self) -> None:
        self.tasks[1]["depends_on"] = ["XT-065"]
        self.flush()
        self.assert_error("Task dependency cycle")

    def test_acceptance_must_close_implementation_dependencies(self) -> None:
        self.tasks[2]["depends_on"] = ["XT-063"]
        self.flush()
        self.assert_error("XT-065 does not depend on XT-064")

    def test_plan_bound_spec_metadata_must_match_backlog(self) -> None:
        self.write_spec(self.tasks[1], title="Divergent title")
        self.assert_error("XT-064 task spec title does not match backlog")

    def test_approved_roadmap_requirement_needs_source_marker(self) -> None:
        (self.root / "docs" / "roadmap.md").write_text(
            "# Roadmap\n\n- [ ] Transfer one file\n",
            encoding="utf-8",
        )
        self.assert_error("missing roadmap marker")

    def test_duplicate_requirement_ids_are_rejected(self) -> None:
        duplicate = copy.deepcopy(self.plan["requirements"][0])
        self.plan["requirements"].append(duplicate)
        self.flush()
        self.assert_error("duplicate requirement REQ-P1-TRANSFER")

    def test_approval_digest_rejects_semantic_edit(self) -> None:
        self.plan["requirements"][0]["statement"] = "Changed after approval."
        self.flush()
        self.assert_error("content_sha256 does not bind plan content")

    def test_unknown_fields_and_source_escape_are_rejected(self) -> None:
        self.plan["unexpected"] = True
        self.plan["source"]["path"] = "../outside.md"
        self.flush()
        errors = delivery_plan.validate_repository(self.root)
        self.assertTrue(any("unknown fields: unexpected" in error for error in errors))
        self.assertTrue(any("escapes the repository" in error for error in errors))

    def test_plan_filename_must_match_plan_id(self) -> None:
        source = (
            self.root / ".agents" / "plans" / "DP-P1-TRANSFER.json"
        )
        source.rename(self.root / ".agents" / "plans" / "wrong.json")
        self.assert_error("must be stored as DP-P1-TRANSFER.json")

    def test_registration_must_match_reserved_mapping(self) -> None:
        self.assertEqual(
            delivery_plan.validate_registration(
                self.root,
                "XT-064",
                "DP-P1-TRANSFER",
                ["REQ-P1-TRANSFER"],
                "implementation",
            ),
            [],
        )
        errors = delivery_plan.validate_registration(
            self.root,
            "XT-064",
            "DP-P1-TRANSFER",
            ["REQ-WRONG"],
            "acceptance",
        )
        self.assertTrue(any("requirements must be" in error for error in errors))
        self.assertTrue(any("role must be implementation" in error for error in errors))

    def test_approve_validates_then_updates_plan_and_readiness(self) -> None:
        self.plan["status"] = "draft"
        self.plan["approval"] = {
            "approved_by": "",
            "approved_at": "",
            "content_sha256": "",
        }
        self.tasks[1]["readiness"] = "blocked"
        self.tasks[2]["readiness"] = "blocked"
        self.flush()
        delivery_plan.approve_plan(
            self.root,
            "DP-P1-TRANSFER",
            "integration-owner",
        )
        approved = json.loads(
            (
                self.root
                / ".agents"
                / "plans"
                / "DP-P1-TRANSFER.json"
            ).read_text(encoding="utf-8")
        )
        self.assertEqual(approved["status"], "approved")
        self.assertEqual(
            approved["approval"]["approved_by"],
            "integration-owner",
        )
        backlog = json.loads(
            (self.root / ".agents" / "backlog.yaml").read_text(encoding="utf-8")
        )
        readiness = {
            task["id"]: task["readiness"] for task in backlog["tasks"]
        }
        self.assertEqual(readiness["XT-064"], "ready")
        self.assertEqual(readiness["XT-065"], "ready")
        self.assertEqual(delivery_plan.validate_repository(self.root), [])

    def test_approve_rejects_incomplete_closure_without_mutation(self) -> None:
        self.plan["status"] = "draft"
        self.plan["approval"] = {
            "approved_by": "",
            "approved_at": "",
            "content_sha256": "",
        }
        self.tasks[1]["readiness"] = "blocked"
        self.tasks[2]["readiness"] = "blocked"
        self.tasks[2]["depends_on"] = ["XT-063"]
        self.flush()
        before_plan = (
            self.root / ".agents" / "plans" / "DP-P1-TRANSFER.json"
        ).read_bytes()
        before_backlog = (
            self.root / ".agents" / "backlog.yaml"
        ).read_bytes()
        with self.assertRaises(delivery_plan.DeliveryPlanError):
            delivery_plan.approve_plan(
                self.root,
                "DP-P1-TRANSFER",
                "integration-owner",
            )
        self.assertEqual(
            (
                self.root
                / ".agents"
                / "plans"
                / "DP-P1-TRANSFER.json"
            ).read_bytes(),
            before_plan,
        )
        self.assertEqual(
            (self.root / ".agents" / "backlog.yaml").read_bytes(),
            before_backlog,
        )

    def test_status_view_derives_requirement_and_scheduler_state(self) -> None:
        view = delivery_plan.render_status_view(
            self.root,
            "DP-P1-TRANSFER",
        )
        self.assertIn("- Requirements accepted: `0/1`", view)
        self.assertIn(
            "| REQ-P1-TRANSFER | planned | XT-065 | XT-064 |",
            view,
        )
        self.assertIn("| XT-064 | claimable | implementation |", view)
        self.assertIn("| XT-065 | dependency-blocked | acceptance |", view)

        implementation_record = (
            self.root / ".agents" / "records" / "XT-064.json"
        )
        record = json.loads(implementation_record.read_text(encoding="utf-8"))
        record["state"] = "done"
        write_json(implementation_record, record)

        updated_view = delivery_plan.render_status_view(
            self.root,
            "DP-P1-TRANSFER",
        )
        self.assertIn(
            "| REQ-P1-TRANSFER | acceptance-ready | XT-065 | XT-064 |",
            updated_view,
        )
        self.assertIn("| XT-065 | claimable | acceptance |", updated_view)

    def test_init_creates_draft_plan(self) -> None:
        destination = delivery_plan.initialize_plan(
            self.root,
            "DP-SECOND",
            "Second plan",
            "governance",
            ".agents/manifest.yaml",
        )
        document = json.loads(destination.read_text(encoding="utf-8"))
        self.assertEqual(document["status"], "draft")
        self.assertEqual(document["requirements"], [])

    def test_schema_v2_criteria_drive_mapping_and_status(self) -> None:
        self.upgrade_plan_to_v2()

        self.assertEqual(delivery_plan.validate_repository(self.root), [])
        self.assertEqual(
            delivery_plan.validate_registration(
                self.root,
                "XT-064",
                "DP-P1-TRANSFER",
                ["REQ-P1-TRANSFER"],
                "implementation",
            ),
            [],
        )
        view = delivery_plan.render_status_view(self.root, "DP-P1-TRANSFER")
        self.assertIn(
            "| REQ-P1-TRANSFER | planned | XT-065 | XT-064 |",
            view,
        )

    def test_schema_v2_rejects_duplicate_criterion_ids(self) -> None:
        self.upgrade_plan_to_v2()
        criterion = self.plan["requirements"][0]["criteria"][0]
        self.plan["requirements"][0]["criteria"].append(copy.deepcopy(criterion))
        self.plan["approval"]["content_sha256"] = delivery_plan.approval_digest(
            self.plan
        )
        self.flush()

        self.assert_error("duplicate criterion CRIT-P1-TRANSFER-EXACT-BYTES")

    def test_schema_v2_requires_negative_definitions(self) -> None:
        self.upgrade_plan_to_v2()
        self.plan["requirements"][0]["criteria"][0][
            "negative_definitions"
        ] = []
        self.plan["approval"]["content_sha256"] = delivery_plan.approval_digest(
            self.plan
        )
        self.flush()

        self.assert_error("negative_definitions must not be empty")

    def test_schema_v2_rejects_skipped_or_untrusted_evidence(self) -> None:
        self.upgrade_plan_to_v2()
        evidence = self.plan["requirements"][0]["criteria"][0]["evidence"][0]
        evidence["allow_skipped"] = True
        evidence["gate"] = "task_authored_shell"
        self.plan["approval"]["content_sha256"] = delivery_plan.approval_digest(
            self.plan
        )
        self.flush()

        errors = delivery_plan.validate_repository(self.root)
        self.assertTrue(any("allow_skipped must be false" in error for error in errors))
        self.assertTrue(any("unregistered gate" in error for error in errors))

    def test_schema_v2_evidence_producer_must_implement_criterion(self) -> None:
        self.upgrade_plan_to_v2()
        evidence = self.plan["requirements"][0]["criteria"][0]["evidence"][0]
        evidence["producer_task"] = "XT-065"
        self.plan["approval"]["content_sha256"] = delivery_plan.approval_digest(
            self.plan
        )
        self.flush()

        self.assert_error("producer_task must appear in implementation_tasks")

    def test_schema_v2_negative_definition_is_approval_semantics(self) -> None:
        self.upgrade_plan_to_v2()
        self.plan["requirements"][0]["criteria"][0][
            "negative_definitions"
        ][0] = "Changed after approval."
        self.flush()

        self.assert_error("content_sha256 does not bind plan content")

    def test_schema_v2_approval_updates_criterion_tasks(self) -> None:
        self.upgrade_plan_to_v2()
        self.plan["status"] = "draft"
        self.plan["approval"] = {
            "approved_by": "",
            "approved_at": "",
            "content_sha256": "",
        }
        self.tasks[1]["readiness"] = "blocked"
        self.tasks[2]["readiness"] = "blocked"
        self.flush()

        delivery_plan.approve_plan(
            self.root,
            "DP-P1-TRANSFER",
            "integration-owner",
        )

        backlog = json.loads(
            (self.root / ".agents" / "backlog.yaml").read_text(
                encoding="utf-8"
            )
        )
        readiness = {
            task["id"]: task["readiness"] for task in backlog["tasks"]
        }
        self.assertEqual(readiness["XT-064"], "ready")
        self.assertEqual(readiness["XT-065"], "ready")
        self.assertEqual(delivery_plan.validate_repository(self.root), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
