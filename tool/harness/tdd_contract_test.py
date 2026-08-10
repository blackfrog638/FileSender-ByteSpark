#!/usr/bin/env python3

"""Focused tests for future-task TDD contracts."""

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
import governance
import tdd_contract


def write_json(path: Path, value: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


class TddContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.plan = {
            "schema_version": 2,
            "id": "DP-FUTURE-FEATURE",
            "title": "Future feature",
            "status": "approved",
            "source": {"kind": "governance", "path": "AGENTS.md"},
            "requirements": [
                {
                    "id": "REQ-FUTURE-FEATURE",
                    "source_ref": "GOVERNANCE-FUTURE-FEATURE",
                    "statement": "Implement one future behavior.",
                    "criteria": [
                        {
                            "id": "CRIT-FUTURE-FEATURE-BEHAVIOR",
                            "statement": "The behavior is observable.",
                            "negative_definitions": [
                                "A mocked result does not satisfy the criterion."
                            ],
                            "implementation_tasks": ["XT-083"],
                            "evidence": [
                                {
                                    "id": "EVD-FUTURE-FEATURE-NATIVE",
                                    "producer_task": "XT-083",
                                    "gate": "native_test",
                                    "level": "integration",
                                    "required_scenarios": ["feature.happy_path"],
                                    "required_assertions": ["result.observable"],
                                    "required_platforms": [],
                                    "required_roles": [],
                                    "topology": "in_process",
                                    "allow_skipped": False,
                                }
                            ],
                        }
                    ],
                    "acceptance_task": "XT-084",
                }
            ],
            "approval": {
                "approved_by": "integration-owner",
                "approved_at": "2026-08-11T00:00:00+00:00",
                "content_sha256": "",
            },
            "superseded_by": "",
        }
        self.plan["approval"]["content_sha256"] = delivery_plan.approval_digest(
            self.plan
        )
        write_json(
            self.root
            / ".agents"
            / "plans"
            / "DP-FUTURE-FEATURE.json",
            self.plan,
        )
        self.task = {
            "id": "XT-083",
            "title": "Future feature",
            "workstream": "native_core",
            "depends_on": [],
            "owned_paths": [
                "native/src/core/**",
                "native/tests/core/**",
                ".agents/records/XT-083.json",
            ],
            "risk_profile_required": True,
            "commit_policy_required": True,
            "architecture_contract_required": True,
            "delivery_plan": "DP-FUTURE-FEATURE",
            "requirement_ids": ["REQ-FUTURE-FEATURE"],
            "delivery_role": "implementation",
        }
        self.record = {
            "schema_version": 4,
            "id": "XT-083",
            "task_type": "feature",
            "state": "ready",
            "owner": "unassigned",
            "base_sha": "",
            "head_sha": "",
            "handoff": ".agents/handoffs/XT-083.md",
            "delivery_plan": "DP-FUTURE-FEATURE",
            "requirement_ids": ["REQ-FUTURE-FEATURE"],
            "delivery_role": "implementation",
            "commit": {
                "type": "feat",
                "scope": "native",
                "summary": "implement future behavior",
            },
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
            "risks": {
                "functionality": {
                    "level": "medium",
                    "rationale": "The future behavior could regress.",
                    "gates": ["native_test", "verify"],
                },
                "security": {
                    "level": "none",
                    "rationale": "The fixture has no security boundary.",
                    "gates": [],
                },
                "performance": {
                    "level": "none",
                    "rationale": "The fixture has no performance behavior.",
                    "gates": [],
                },
                "compatibility": {
                    "level": "none",
                    "rationale": "The fixture changes no public contract.",
                    "gates": [],
                },
                "concurrency": {
                    "level": "none",
                    "rationale": "The fixture has no concurrent behavior.",
                    "gates": [],
                },
                "platform": {
                    "level": "none",
                    "rationale": "The fixture is platform independent.",
                    "gates": [],
                },
                "persistence": {
                    "level": "none",
                    "rationale": "The fixture persists no state.",
                    "gates": [],
                },
            },
            "impacts": {
                "adr": {
                    "required": False,
                    "status": "not_required",
                    "references": [],
                    "rationale": "The fixture makes no durable decision.",
                },
                "architecture": {
                    "status": "not_required",
                    "references": [],
                    "rationale": "The fixture changes no architecture.",
                },
                "roadmap": {
                    "status": "not_required",
                    "references": [],
                    "rationale": "The fixture changes no roadmap.",
                },
            },
            "integration": {
                "strategy": "",
                "mappings": [],
                "verified_sha": "",
            },
            "verification": {
                "status": "pending",
                "gates": ["native_test", "verify"],
                "commands": ["make native-test", "make verify"],
                "reference": "",
            },
            "acceptance": {
                "accepted_by": "",
                "accepted_at": "",
                "note": "",
            },
            "test_contract": {
                "schema_version": 1,
                "plan_content_sha256": self.plan["approval"][
                    "content_sha256"
                ],
                "criterion_ids": ["CRIT-FUTURE-FEATURE-BEHAVIOR"],
                "proof_mode": "red_green",
                "executor": "deterministic",
                "gate": "native_test",
                "proof_surface": ["native/tests/core/**"],
                "failure_fingerprints": [
                    "FAILED: future feature behavior is not implemented"
                ],
                "allow_skipped": False,
            },
        }
        self.gates = {
            "native_test": "make native-test",
            "verify": "make verify",
        }

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def errors(self) -> list[str]:
        return tdd_contract.validate_test_contract(
            self.root,
            self.task,
            self.record,
            self.gates,
        )

    def assert_error(self, text: str) -> None:
        errors = self.errors()
        self.assertTrue(
            any(text in error for error in errors),
            f"missing {text!r} in {errors!r}",
        )

    def test_valid_feature_contract(self) -> None:
        self.assertEqual(self.errors(), [])

    def test_schema_v4_record_uses_test_contract_validator(self) -> None:
        original_root = governance.ROOT
        governance.ROOT = self.root
        try:
            errors = governance.validate_record(
                self.task,
                self.record,
                {"XT-083": self.record},
                {"XT-083": self.task},
                set(),
                self.gates,
                verify_git=False,
            )
        finally:
            governance.ROOT = original_root
        self.assertEqual(errors, [])

    def test_proof_modes_follow_task_type_and_delivery_role(self) -> None:
        cases = {
            ("feature", "implementation"): "red_green",
            ("bugfix", "implementation"): "regression",
            ("refactor", "implementation"): "equivalence",
            ("test", "implementation"): "mutation",
            ("governance", "implementation"): "red_green",
            ("investigation", "implementation"): "bounded_evidence",
            ("governance", "acceptance"): "evidence_closure",
        }
        for inputs, expected in cases.items():
            with self.subTest(inputs=inputs):
                self.assertEqual(
                    tdd_contract.expected_proof_mode(*inputs),
                    expected,
                )

    def test_rejects_wrong_proof_mode(self) -> None:
        self.record["test_contract"]["proof_mode"] = "equivalence"
        self.assert_error("proof_mode must be red_green")

    def test_rejects_unknown_or_unmapped_criterion(self) -> None:
        self.record["test_contract"]["criterion_ids"] = ["CRIT-UNKNOWN"]
        self.assert_error("criterion_ids do not match task mappings")

    def test_rejects_plan_digest_mismatch(self) -> None:
        self.record["test_contract"]["plan_content_sha256"] = "0" * 64
        self.assert_error("plan_content_sha256 does not match approved plan")

    def test_rejects_untrusted_or_unexecuted_gate(self) -> None:
        self.record["test_contract"]["gate"] = "task_authored_shell"
        self.assert_error("gate is not registered")
        self.record["test_contract"]["gate"] = "native_test"
        self.record["verification"]["gates"] = ["verify"]
        self.assert_error("gate must appear in verification.gates")

    def test_rejects_proof_surface_outside_ownership(self) -> None:
        self.record["test_contract"]["proof_surface"] = ["native/src/other/**"]
        self.assert_error("proof_surface is outside task ownership")

    def test_rejects_non_normalized_or_file_descendant_proof_surface(
        self,
    ) -> None:
        cases = (
            (
                ["native/tests/core/exact_test.py"],
                "native/tests/core/exact_test.py/**",
            ),
            (
                ["native/tests/core/**"],
                "native/tests/core/../secret/**",
            ),
        )
        for owned_paths, proof_path in cases:
            with self.subTest(proof_path=proof_path):
                self.task["owned_paths"] = owned_paths
                self.record["test_contract"]["proof_surface"] = [proof_path]
                self.assert_error("proof_surface is outside task ownership")

    def test_rejects_skips_and_missing_red_fingerprint(self) -> None:
        self.record["test_contract"]["allow_skipped"] = True
        self.record["test_contract"]["failure_fingerprints"] = []
        errors = self.errors()
        self.assertTrue(any("allow_skipped must be false" in error for error in errors))
        self.assertTrue(
            any("failure_fingerprints must not be empty" in error for error in errors)
        )

    def test_rejects_unknown_contract_fields(self) -> None:
        contract = copy.deepcopy(self.record["test_contract"])
        contract["shell"] = "rm -rf /"
        self.record["test_contract"] = contract
        self.assert_error("unknown fields: shell")


if __name__ == "__main__":
    unittest.main(verbosity=2)
