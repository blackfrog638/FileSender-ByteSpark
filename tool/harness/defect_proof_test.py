#!/usr/bin/env python3

from __future__ import annotations

import copy
import json
import shlex
import subprocess
import tempfile
import unittest
from contextlib import ExitStack
from pathlib import Path
from unittest import mock

import defect_proof
import governance


class DefectProofTest(unittest.TestCase):
    def setUp(self) -> None:
        self.fingerprint = "EXPECTED REGRESSION FAILURE"
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "repository"
        self.root.mkdir()
        self.git("init", "-q")
        self.git("config", "user.name", "Defect Proof Test")
        self.git("config", "user.email", "defect-proof@example.test")

    def git(self, *args: str) -> str:
        result = subprocess.run(
            ["git", "-C", str(self.root), *args],
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout.strip()

    def write_script(self, exit_code: int, output: str) -> None:
        (self.root / "regression.sh").write_text(
            "#!/usr/bin/env bash\n"
            f"printf '%s\\n' {shlex.quote(output)}\n"
            f"exit {exit_code}\n",
            encoding="utf-8",
        )

    def write_record(
        self,
        base: str,
        reproduction: str,
        mode: str,
    ) -> None:
        record = {
            "schema_version": 3,
            "id": "XT-999",
            "task_type": "bugfix",
            "state": "in_progress",
            "base_sha": base,
            "defect": {
                "proof_mode": mode,
                "reproduction_commit": reproduction,
                "regression_gate": "verify",
                "failure_fingerprint": self.fingerprint,
            },
            "verification": {
                "gates": ["verify"],
                "commands": ["bash regression.sh"],
            },
        }
        path = self.root / ".agents" / "records" / "XT-999.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")

    def create_history(
        self,
        reproduction_exit: int,
        head_exit: int,
        *,
        base_exit: int = 0,
        mode: str = "deterministic",
        reproduction_output: str | None = None,
    ) -> tuple[str, str, str]:
        manifest = self.root / ".agents" / "manifest.yaml"
        manifest.parent.mkdir(parents=True)
        manifest.write_text(
            "commands:\n  verify: bash regression.sh\n",
            encoding="utf-8",
        )
        self.write_script(base_exit, "BASE RESULT")
        self.git("add", ".agents/manifest.yaml", "regression.sh")
        self.git("commit", "-m", "test: create proof base")
        base = self.git("rev-parse", "HEAD")

        self.write_script(
            reproduction_exit,
            reproduction_output or self.fingerprint,
        )
        self.git("add", "regression.sh")
        self.git("commit", "-m", "test: reproduce defect")
        reproduction = self.git("rev-parse", "HEAD")

        self.write_script(head_exit, "HEAD RESULT")
        self.write_record(base, reproduction, mode)
        self.git("add", "regression.sh", ".agents/records/XT-999.json")
        self.git("commit", "-m", "test: repair defect")
        head = self.git("rev-parse", "HEAD")
        return base, reproduction, head

    def worktree_count(self) -> int:
        return self.git("worktree", "list", "--porcelain").count("worktree ")

    def test_records_failure_at_reproduction_and_pass_at_head(self) -> None:
        base, reproduction, head = self.create_history(1, 0)

        proof = defect_proof.run_proof(self.root, "XT-999", head)

        self.assertIsNotNone(proof)
        assert proof is not None
        self.assertEqual(proof["gate"], "verify")
        self.assertEqual(proof["base_commit"], base)
        self.assertEqual(proof["reproduction_commit"], reproduction)
        self.assertEqual(proof["head_commit"], head)
        self.assertEqual(proof["base_exit_code"], 0)
        self.assertEqual(proof["reproduction_exit_code"], 1)
        self.assertEqual(proof["head_exit_code"], 0)
        self.assertEqual(
            proof["command_sha256"],
            defect_proof.command_digest("bash regression.sh"),
        )
        self.assertEqual(
            proof["failure_fingerprint_sha256"],
            defect_proof.command_digest(self.fingerprint),
        )
        self.assertEqual(
            proof["reproduction_output_sha256"],
            defect_proof.command_digest(self.fingerprint + "\n"),
        )
        record = defect_proof.load_record(self.root, "XT-999")
        self.assertEqual(record["verification"]["defect_proof"], proof)
        self.assertEqual(self.worktree_count(), 1)

    def test_rejects_passing_reproduction_and_removes_worktree(self) -> None:
        _, _, head = self.create_history(0, 0)

        with self.assertRaisesRegex(
            defect_proof.DefectProofError,
            "unexpectedly passed at reproduction",
        ):
            defect_proof.run_proof(self.root, "XT-999", head)

        self.assertEqual(self.worktree_count(), 1)

    def test_rejects_failing_base_and_removes_worktree(self) -> None:
        _, _, head = self.create_history(1, 0, base_exit=3)

        with self.assertRaisesRegex(
            defect_proof.DefectProofError,
            "failed at task base with exit 3",
        ):
            defect_proof.run_proof(self.root, "XT-999", head)

        self.assertEqual(self.worktree_count(), 1)

    def test_rejects_failure_without_fingerprint(self) -> None:
        _, _, head = self.create_history(
            1,
            0,
            reproduction_output="UNRELATED COMPILER FAILURE",
        )

        with self.assertRaisesRegex(
            defect_proof.DefectProofError,
            "did not contain the declared fingerprint",
        ):
            defect_proof.run_proof(self.root, "XT-999", head)

        self.assertEqual(self.worktree_count(), 1)

    def test_rejects_fingerprint_that_is_only_a_substring(self) -> None:
        _, _, head = self.create_history(
            1,
            0,
            reproduction_output=f"source text: {self.fingerprint}",
        )

        with self.assertRaisesRegex(
            defect_proof.DefectProofError,
            "fingerprint as a complete line",
        ):
            defect_proof.run_proof(self.root, "XT-999", head)

        self.assertEqual(self.worktree_count(), 1)

    def test_detached_gate_uses_derived_tool_root(self) -> None:
        manifest = self.root / ".agents" / "manifest.yaml"
        manifest.parent.mkdir(parents=True)
        manifest.write_text(
            "commands:\n  verify: bash regression.sh\n",
            encoding="utf-8",
        )
        self.write_script(0, "BASE RESULT")
        self.git("add", ".agents/manifest.yaml", "regression.sh")
        self.git("commit", "-m", "test: create proof base")
        base = self.git("rev-parse", "HEAD")

        expected = str((self.root / "out" / "tools" / "vcpkg").resolve())
        (self.root / "regression.sh").write_text(
            "#!/usr/bin/env bash\n"
            "printf '%s\\n' \"$XNN_TRANSFER_VCPKG_ROOT\"\n"
            "exit 1\n",
            encoding="utf-8",
        )
        self.git("add", "regression.sh")
        self.git("commit", "-m", "test: reproduce defect")
        reproduction = self.git("rev-parse", "HEAD")

        self.fingerprint = expected
        self.write_script(0, "HEAD RESULT")
        self.write_record(base, reproduction, "deterministic")
        self.git("add", "regression.sh", ".agents/records/XT-999.json")
        self.git("commit", "-m", "test: repair defect")
        head = self.git("rev-parse", "HEAD")

        with mock.patch.dict(
            defect_proof.os.environ,
            {"XNN_TRANSFER_VCPKG_ROOT": "/untrusted/caller/path"},
        ):
            proof = defect_proof.run_proof(self.root, "XT-999", head)

        assert proof is not None
        self.assertEqual(
            proof["reproduction_output_sha256"],
            defect_proof.command_digest(expected + "\n"),
        )
        self.assertEqual(self.worktree_count(), 1)

    def test_rejects_failing_head_and_removes_worktree(self) -> None:
        _, _, head = self.create_history(1, 2)

        with self.assertRaisesRegex(
            defect_proof.DefectProofError,
            "failed at reviewed head with exit 2",
        ):
            defect_proof.run_proof(self.root, "XT-999", head)

        self.assertEqual(self.worktree_count(), 1)

    def test_rejects_unsupported_proof_mode(self) -> None:
        _, _, head = self.create_history(1, 0, mode="stress")

        with self.assertRaisesRegex(
            defect_proof.DefectProofError,
            "is not supported by deterministic proof",
        ):
            defect_proof.run_proof(self.root, "XT-999", head)

    def test_rejects_unavailable_reproduction(self) -> None:
        base, _, head = self.create_history(1, 0)
        record = defect_proof.load_record(self.root, "XT-999")
        record["defect"]["reproduction_commit"] = "0" * 40
        defect_proof.write_record(self.root, "XT-999", record)
        self.git("add", ".agents/records/XT-999.json")
        self.git("commit", "--amend", "--no-edit")
        head = self.git("rev-parse", "HEAD")
        self.assertNotEqual(base, head)

        with self.assertRaisesRegex(
            defect_proof.DefectProofError,
            "reproduction commit is unavailable",
        ):
            defect_proof.run_proof(self.root, "XT-999", head)


class DefectProofValidationTest(unittest.TestCase):
    def record(self) -> dict[str, object]:
        base = "0" * 40
        reproduction = "1" * 40
        head = "2" * 40
        fingerprint = "FAILED: fatal transcript remains usable"
        return {
            "task_type": "bugfix",
            "base_sha": base,
            "head_sha": head,
            "defect": {
                "severity": "P1",
                "source": "test",
                "symptom": "A fatal parser accepts a later frame.",
                "expected_contract": "Fatal errors terminate parsing.",
                "actual_behavior": "A later frame is accepted.",
                "trigger": "Feed a fatal frame and then a valid frame.",
                "affected_since": "initial implementation",
                "proof_mode": "deterministic",
                "reproduction_commit": reproduction,
                "regression_gate": "verify",
                "contract_disposition": "restore",
                "failure_fingerprint": fingerprint,
            },
            "impacts": {
                "adr": {
                    "required": False,
                    "status": "not_required",
                    "references": [],
                }
            },
            "verification": {
                "defect_proof": {
                    "mode": "deterministic",
                    "gate": "verify",
                    "command_sha256": defect_proof.command_digest("true"),
                    "failure_fingerprint_sha256": defect_proof.command_digest(
                        fingerprint
                    ),
                    "base_commit": base,
                    "reproduction_commit": reproduction,
                    "head_commit": head,
                    "base_exit_code": 0,
                    "reproduction_exit_code": 1,
                    "head_exit_code": 0,
                    "reproduction_output_sha256": "3" * 64,
                    "checked_at": "2000-01-01T00:00:00+00:00",
                }
            },
        }

    def errors(
        self,
        record: dict[str, object],
        state: str = "review",
    ) -> list[str]:
        errors: list[str] = []
        governance.validate_schema_v3(
            errors,
            "XT-999",
            record,
            state,
            {"verify": "true"},
            ["verify"],
            verify_git=False,
        )
        return errors

    def test_accepts_bound_deterministic_proof(self) -> None:
        self.assertEqual(self.errors(self.record()), [])

    def test_requires_proof_at_review(self) -> None:
        record = self.record()
        record["verification"] = {}

        self.assertTrue(
            any(
                "verification.defect_proof must be an object" in error
                for error in self.errors(record)
            )
        )

    def test_rejects_changed_command_hash(self) -> None:
        record = self.record()
        record["verification"]["defect_proof"]["command_sha256"] = "0" * 64

        self.assertTrue(
            any(
                "does not match the trusted gate" in error
                for error in self.errors(record)
            )
        )

    def test_rejects_forged_fingerprint_and_output_hash(self) -> None:
        record = self.record()
        proof = record["verification"]["defect_proof"]
        proof["failure_fingerprint_sha256"] = "4" * 64
        proof["reproduction_output_sha256"] = "not-a-digest"

        errors = "\n".join(self.errors(record))
        self.assertIn("does not match defect.failure_fingerprint", errors)
        self.assertIn(
            "reproduction_output_sha256 must be a SHA-256 digest",
            errors,
        )

    def test_rejects_forged_base_evidence(self) -> None:
        record = self.record()
        proof = record["verification"]["defect_proof"]
        proof["base_commit"] = "5" * 40
        proof["base_exit_code"] = 1

        errors = "\n".join(self.errors(record))
        self.assertIn("base_commit does not match base_sha", errors)
        self.assertIn("base_exit_code must be zero", errors)

    def test_rejects_reproduction_at_task_base(self) -> None:
        record = self.record()
        reproduction = record["defect"]["reproduction_commit"]
        record["base_sha"] = reproduction
        record["verification"]["defect_proof"]["base_commit"] = reproduction

        self.assertTrue(
            any(
                "reproduction must differ from task base" in error
                for error in self.errors(record)
            )
        )

    def test_rejects_forged_exit_codes_and_head(self) -> None:
        record = self.record()
        proof = record["verification"]["defect_proof"]
        proof["reproduction_exit_code"] = 0
        proof["head_exit_code"] = 1
        proof["head_commit"] = "3" * 40

        errors = "\n".join(self.errors(record))
        self.assertIn("non-infrastructure failure", errors)
        self.assertIn("head_exit_code must be zero", errors)
        self.assertIn("does not match head_sha", errors)

    def test_rejects_unsupported_mode_at_review(self) -> None:
        record = self.record()
        record["defect"]["proof_mode"] = "sanitizer"
        record["verification"]["defect_proof"]["mode"] = "sanitizer"

        self.assertTrue(
            any("has no review executor" in error for error in self.errors(record))
        )

    def test_rejects_naive_proof_time(self) -> None:
        record = self.record()
        record["verification"]["defect_proof"][
            "checked_at"
        ] = "2000-01-01T00:00:00"

        self.assertTrue(
            any("must include a timezone" in error for error in self.errors(record))
        )

    def test_accepts_legacy_proof_only_after_acceptance(self) -> None:
        record = self.record()
        del record["defect"]["failure_fingerprint"]
        proof = record["verification"]["defect_proof"]
        for field in (
            "failure_fingerprint_sha256",
            "base_commit",
            "base_exit_code",
            "reproduction_output_sha256",
        ):
            del proof[field]

        self.assertEqual(self.errors(record, "done"), [])
        self.assertTrue(
            any(
                "defect is missing fields: failure_fingerprint" in error
                for error in self.errors(record, "review")
            )
        )


class MarkReviewProofTest(unittest.TestCase):
    def record(self) -> dict[str, object]:
        return {
            "state": "in_progress",
            "verification": {
                "status": "pending",
                "reference": "",
            },
        }

    def test_low_level_mark_review_executes_proof_runner(self) -> None:
        original = self.record()
        proved = copy.deepcopy(original)
        proved["verification"]["defect_proof"] = {"generated": True}
        path = Path("/tmp/XT-999.json")

        with ExitStack() as stack:
            stack.enter_context(
                mock.patch.object(
                    governance,
                    "load_record",
                    side_effect=[copy.deepcopy(original), proved],
                )
            )
            stack.enter_context(
                mock.patch.object(governance, "record_path", return_value=path)
            )
            write_json = stack.enter_context(
                mock.patch.object(governance, "write_json")
            )
            stack.enter_context(
                mock.patch.object(governance, "validate_repository")
            )
            run_proof = stack.enter_context(
                mock.patch.object(
                    governance.defect_proof_runner,
                    "run_proof",
                )
            )
            governance.mark_review("XT-999", "1" * 40, "test:proof")

        run_proof.assert_called_once_with(
            governance.ROOT,
            "XT-999",
            "1" * 40,
        )
        written = write_json.call_args.args[1]
        self.assertEqual(written["state"], "review")
        self.assertEqual(
            written["verification"]["defect_proof"],
            {"generated": True},
        )

    def test_proof_failure_restores_original_record(self) -> None:
        original = self.record()
        path = Path("/tmp/XT-999.json")

        with ExitStack() as stack:
            stack.enter_context(
                mock.patch.object(
                    governance,
                    "load_record",
                    return_value=copy.deepcopy(original),
                )
            )
            stack.enter_context(
                mock.patch.object(governance, "record_path", return_value=path)
            )
            write_json = stack.enter_context(
                mock.patch.object(governance, "write_json")
            )
            stack.enter_context(
                mock.patch.object(
                    governance.defect_proof_runner,
                    "run_proof",
                    side_effect=defect_proof.DefectProofError("proof failed"),
                )
            )
            stack.enter_context(
                self.assertRaisesRegex(governance.GovernanceError, "proof failed")
            )
            governance.mark_review("XT-999", "1" * 40, "test:proof")

        write_json.assert_called_once_with(path, original)

    def test_validation_failure_restores_original_record(self) -> None:
        original = self.record()
        proved = copy.deepcopy(original)
        proved["verification"]["defect_proof"] = {"generated": True}
        path = Path("/tmp/XT-999.json")

        with ExitStack() as stack:
            stack.enter_context(
                mock.patch.object(
                    governance,
                    "load_record",
                    side_effect=[copy.deepcopy(original), proved],
                )
            )
            stack.enter_context(
                mock.patch.object(governance, "record_path", return_value=path)
            )
            write_json = stack.enter_context(
                mock.patch.object(governance, "write_json")
            )
            stack.enter_context(
                mock.patch.object(
                    governance,
                    "validate_repository",
                    side_effect=governance.GovernanceError("invalid review"),
                )
            )
            stack.enter_context(
                mock.patch.object(governance.defect_proof_runner, "run_proof")
            )
            stack.enter_context(
                self.assertRaisesRegex(
                    governance.GovernanceError,
                    "invalid review",
                )
            )
            governance.mark_review("XT-999", "1" * 40, "test:proof")

        self.assertEqual(write_json.call_count, 2)
        self.assertEqual(write_json.call_args_list[-1].args, (path, original))


if __name__ == "__main__":
    unittest.main()
