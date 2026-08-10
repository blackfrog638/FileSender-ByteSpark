#!/usr/bin/env python3

from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from contextlib import ExitStack
from pathlib import Path
from unittest import mock

import governance
import tdd_proof


class TddProofTest(unittest.TestCase):
    task_id = "XT-083"
    fingerprint = "FAILED: governed behavior is not implemented"

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "repository"
        self.root.mkdir()
        self.git("init", "-q")
        self.git("config", "user.name", "TDD Proof Test")
        self.git("config", "user.email", "tdd-proof@example.test")
        self.write_fixture()

    def git(self, *args: str, check: bool = True) -> str:
        result = subprocess.run(
            ["git", "-C", str(self.root), *args],
            check=check,
            capture_output=True,
            text=True,
        )
        return result.stdout.strip()

    def write(self, relative: str, content: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def commit(self, subject: str) -> str:
        self.git("add", "-A")
        self.git("commit", "-m", subject)
        return self.git("rev-parse", "HEAD")

    def record_path(self) -> Path:
        return self.root / ".agents" / "records" / f"{self.task_id}.json"

    def record(self) -> dict[str, object]:
        return json.loads(self.record_path().read_text(encoding="utf-8"))

    def write_record(self, record: dict[str, object]) -> None:
        path = self.record_path()
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps(record, indent=2) + "\n",
            encoding="utf-8",
        )

    def write_fixture(self) -> None:
        self.write(
            ".agents/manifest.yaml",
            "commands:\n"
            "  focused: bash focused.sh\n"
            "  verify: true\n",
        )
        self.write(
            ".agents/backlog.yaml",
            json.dumps(
                {
                    "schema_version": 1,
                    "tasks": [
                        {
                            "id": self.task_id,
                            "title": "Governed behavior",
                            "owned_paths": [
                                f".agents/records/{self.task_id}.json",
                                f".agents/tasks/{self.task_id}-fixture.md",
                                "tests/**",
                                "src/**",
                            ],
                            "delivery_plan": "DP-FIXTURE",
                            "requirement_ids": ["REQ-FIXTURE"],
                            "delivery_role": "implementation",
                        }
                    ],
                },
                indent=2,
            )
            + "\n",
        )
        self.write(
            ".agents/plans/DP-FIXTURE.json",
            json.dumps(
                {
                    "schema_version": 2,
                    "id": "DP-FIXTURE",
                    "status": "approved",
                    "approval": {"content_sha256": "a" * 64},
                },
                indent=2,
            )
            + "\n",
        )
        self.write(
            f".agents/tasks/{self.task_id}-fixture.md",
            "---\nid: XT-083\n---\n",
        )
        self.write(".github/workflows/ci.yml", "name: fixture\n")
        self.write("tool/harness/tdd_contract.py", "# fixture contract\n")
        harness = Path(tdd_proof.__file__).resolve().parent
        self.write(
            "tool/harness/tdd_proof.py",
            "#!/usr/bin/env python3\n"
            "import sys\n"
            f"sys.path.insert(0, {str(harness)!r})\n"
            "from tdd_proof import main\n"
            "raise SystemExit(main())\n",
        )
        self.write(
            "tool/harness/governance.py",
            "#!/usr/bin/env python3\n"
            "import json\n"
            "import sys\n"
            "from pathlib import Path\n"
            "task_id = sys.argv[2]\n"
            "field = sys.argv[3]\n"
            "root = Path(__file__).resolve().parents[2]\n"
            "record = json.loads((root / '.agents' / 'records' / "
            "f'{task_id}.json').read_text())\n"
            "print(record[field])\n",
        )
        self.write(
            "tool/harness/agent.sh",
            (harness / "agent.sh").read_text(encoding="utf-8"),
        )
        self.write(
            "focused.sh",
            "#!/usr/bin/env bash\n"
            "set -u\n"
            "if [[ ! -f tests/red.flag ]]; then exit 0; fi\n"
            "case \"$(cat tests/red.flag)\" in\n"
            "  red)\n"
            "    if [[ -f src/implemented.flag ]]; then exit 0; fi\n"
            f"    printf '%s\\n' '{self.fingerprint}'\n"
            "    exit 1\n"
            "    ;;\n"
            "  unrelated)\n"
            "    printf '%s\\n' 'FAILED: unrelated compiler failure'\n"
            "    exit 1\n"
            "    ;;\n"
            "  infrastructure)\n"
            f"    printf '%s\\n' '{self.fingerprint}'\n"
            "    exit 127\n"
            "    ;;\n"
            "  crash)\n"
            f"    printf '%s\\n' '{self.fingerprint}'\n"
            "    exit 139\n"
            "    ;;\n"
            "  skipped)\n"
            "    printf '%s\\n' 'SKIPPED: governed behavior'\n"
            f"    printf '%s\\n' '{self.fingerprint}'\n"
            "    exit 1\n"
            "    ;;\n"
            "  timeout)\n"
            "    sleep 1\n"
            f"    printf '%s\\n' '{self.fingerprint}'\n"
            "    exit 1\n"
            "    ;;\n"
            "  equivalence)\n"
            "    exit 0\n"
            "    ;;\n"
            "esac\n",
        )
        for executable in (
            "focused.sh",
            "tool/harness/agent.sh",
            "tool/harness/governance.py",
            "tool/harness/tdd_proof.py",
        ):
            (self.root / executable).chmod(0o755)
        record = {
            "schema_version": 4,
            "id": self.task_id,
            "task_type": "feature",
            "state": "ready",
            "base_sha": "",
            "delivery_plan": "DP-FIXTURE",
            "requirement_ids": ["REQ-FIXTURE"],
            "delivery_role": "implementation",
            "commit": {
                "type": "feat",
                "scope": "fixture",
                "summary": "implement governed behavior",
            },
            "verification": {
                "gates": ["focused", "verify"],
                "commands": ["bash focused.sh", "true"],
            },
            "test_contract": {
                "schema_version": 1,
                "plan_content_sha256": "a" * 64,
                "criterion_ids": ["CRIT-FIXTURE"],
                "proof_mode": "red_green",
                "executor": "deterministic",
                "gate": "focused",
                "proof_surface": ["tests/**"],
                "failure_fingerprints": [self.fingerprint],
                "allow_skipped": False,
            },
        }
        self.write_record(record)
        self.base = self.commit("test: create TDD proof base")
        record["state"] = "in_progress"
        record["base_sha"] = self.base
        self.write_record(record)
        self.commit("test: claim TDD proof fixture")
        self.write("tests/red.flag", "red")
        self.red = self.commit("test: add failing behavior oracle")

    def worktree_count(self) -> int:
        return self.git("worktree", "list", "--porcelain").count("worktree ")

    def checkpoint(self) -> dict[str, object]:
        return tdd_proof.record_red_checkpoint(self.root, self.task_id)

    def commit_checkpoint(self) -> str:
        self.git("add", "-A")
        self.git(
            "commit",
            "-m",
            "chore(fixture): record deterministic Red checkpoint",
            "-m",
            f"Xnn-Task: {self.task_id}\nXnn-Lifecycle: checkpoint",
        )
        return self.git("rev-parse", "HEAD")

    def complete_head(self) -> str:
        self.commit_checkpoint()
        self.write("src/implemented.flag", "implemented\n")
        return self.commit("test: implement governed behavior")

    def assert_checkpoint_error(self, expected: str) -> None:
        with self.assertRaisesRegex(tdd_proof.TddProofError, expected):
            self.checkpoint()
        self.assertEqual(self.worktree_count(), 1)

    def test_records_red_checkpoint_and_review_proof(self) -> None:
        checkpoint = self.checkpoint()
        self.assertEqual(checkpoint["base_commit"], self.base)
        self.assertEqual(checkpoint["red_commit"], self.red)
        self.assertEqual(checkpoint["base_exit_code"], 0)
        self.assertEqual(checkpoint["red_exit_code"], 1)
        self.assertEqual(checkpoint["gate"], "focused")
        self.assertEqual(self.worktree_count(), 1)

        head = self.complete_head()
        proof = tdd_proof.run_review_proof(self.root, self.task_id, head)

        self.assertIsNotNone(proof)
        assert proof is not None
        self.assertEqual(proof["base_commit"], self.base)
        self.assertEqual(proof["red_commit"], self.red)
        self.assertEqual(proof["head_commit"], head)
        self.assertEqual(
            proof["checkpoint_commit"],
            self.git("rev-parse", f"{head}^"),
        )
        self.assertEqual(proof["base_exit_code"], 0)
        self.assertEqual(proof["red_exit_code"], 1)
        self.assertEqual(proof["head_exit_code"], 0)
        self.assertEqual(
            self.record()["verification"]["tdd_proof"],
            proof,
        )
        self.assertEqual(self.worktree_count(), 1)

    def test_agent_command_commits_only_checkpoint_record(self) -> None:
        self.git("branch", "-M", f"task/{self.task_id}")

        result = subprocess.run(
            [
                str(self.root / "tool" / "harness" / "agent.sh"),
                "checkpoint",
                self.task_id,
                "red",
            ],
            cwd=self.root,
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("recorded deterministic Red checkpoint", result.stdout)
        changed = self.git(
            "diff-tree",
            "--no-commit-id",
            "--name-only",
            "-r",
            "HEAD",
        ).splitlines()
        self.assertEqual(
            changed,
            [f".agents/records/{self.task_id}.json"],
        )
        message = self.git("show", "-s", "--format=%B", "HEAD")
        self.assertIn("Xnn-Lifecycle: checkpoint", message)
        self.assertEqual(
            self.record()["verification"]["tdd_checkpoint"]["red_commit"],
            self.red,
        )

    def test_rejects_task_authored_checkpoint_commit(self) -> None:
        self.checkpoint()
        self.commit("test: hand author checkpoint evidence")
        self.write("src/implemented.flag", "implemented\n")
        head = self.commit("test: implement after authored checkpoint")

        with self.assertRaisesRegex(
            tdd_proof.TddProofError,
            "checkpoint lifecycle commit",
        ):
            tdd_proof.run_review_proof(self.root, self.task_id, head)

    def test_rejects_production_commit_even_when_reverted_before_red(self) -> None:
        self.git("reset", "--hard", "HEAD^")
        self.write("src/early.flag", "implemented too early\n")
        self.commit("test: add early implementation")
        (self.root / "src" / "early.flag").unlink()
        self.commit("test: revert early implementation")
        self.write("tests/red.flag", "red")
        self.red = self.commit("test: restore failing oracle")

        self.assert_checkpoint_error("production or undeclared path")

    def test_rejects_test_named_file_in_production_tree(self) -> None:
        record = self.record()
        record["test_contract"]["proof_surface"].append(
            "src/early_test.py"
        )
        self.write_record(record)
        self.write("src/early_test.py", "implemented too early\n")
        self.git("add", str(self.record_path()), "src/early_test.py")
        self.git("commit", "--amend", "--no-edit")
        self.red = self.git("rev-parse", "HEAD")

        self.assert_checkpoint_error("production or undeclared path")

    def test_rejects_changed_frozen_oracle_at_review(self) -> None:
        self.checkpoint()
        self.commit_checkpoint()
        self.write("tests/red.flag", "red\n")
        self.write("src/implemented.flag", "implemented\n")
        head = self.commit("test: change oracle and implementation")

        with self.assertRaisesRegex(
            tdd_proof.TddProofError,
            "frozen proof surface changed",
        ):
            tdd_proof.run_review_proof(self.root, self.task_id, head)
        self.assertEqual(self.worktree_count(), 1)

    def test_rejects_unrelated_and_infrastructure_failures(self) -> None:
        for content, expected in (
            ("unrelated", "declared failure fingerprints"),
            ("infrastructure", "infrastructure exit"),
            ("crash", "crash exit"),
            ("skipped", "skipped result"),
        ):
            with self.subTest(content=content):
                self.write("tests/red.flag", content)
                self.git("add", "tests/red.flag")
                self.git("commit", "--amend", "--no-edit")
                self.red = self.git("rev-parse", "HEAD")
                self.assert_checkpoint_error(expected)

    def test_rejects_timeout_and_cleans_detached_worktrees(self) -> None:
        self.write("tests/red.flag", "timeout")
        self.git("add", "tests/red.flag")
        self.git("commit", "--amend", "--no-edit")
        self.red = self.git("rev-parse", "HEAD")

        with mock.patch.object(tdd_proof, "GATE_TIMEOUT_SECONDS", 0.01):
            self.assert_checkpoint_error("timed out")

    def test_rejects_unregistered_or_unexecuted_gate_and_dirty_tree(self) -> None:
        record = self.record()
        record["test_contract"]["gate"] = "task_shell"
        self.write_record(record)
        self.git("add", str(self.record_path()))
        self.git("commit", "--amend", "--no-edit")
        self.assert_checkpoint_error("not registered")

        record["test_contract"]["gate"] = "focused"
        record["verification"]["gates"] = ["verify"]
        self.write_record(record)
        self.git("add", str(self.record_path()))
        self.git("commit", "--amend", "--no-edit")
        self.assert_checkpoint_error("verification.gates")

        record["verification"]["gates"] = ["focused", "verify"]
        self.write_record(record)
        self.write("tests/uncommitted.txt", "dirty\n")
        self.assert_checkpoint_error("worktree must be clean")

    def test_characterization_equivalence_passes_at_all_revisions(self) -> None:
        record = self.record()
        contract = record["test_contract"]
        contract["proof_mode"] = "equivalence"
        contract["failure_fingerprints"] = []
        self.write_record(record)
        self.write("tests/red.flag", "equivalence")
        self.git("add", str(self.record_path()), "tests/red.flag")
        self.git("commit", "--amend", "--no-edit")
        self.red = self.git("rev-parse", "HEAD")

        checkpoint = self.checkpoint()
        self.assertEqual(checkpoint["red_exit_code"], 0)
        head = self.complete_head()
        proof = tdd_proof.run_review_proof(self.root, self.task_id, head)
        assert proof is not None
        self.assertEqual(proof["head_exit_code"], 0)

    def test_regression_checkpoint_binds_defect_reproduction(self) -> None:
        record = self.record()
        record["task_type"] = "bugfix"
        record["test_contract"]["proof_mode"] = "regression"
        record["defect"] = {
            "regression_gate": "focused",
            "failure_fingerprint": self.fingerprint,
            "reproduction_commit": "",
        }
        self.write_record(record)
        self.git("add", str(self.record_path()))
        self.git("commit", "--amend", "--no-edit")
        self.red = self.git("rev-parse", "HEAD")

        self.checkpoint()

        self.assertEqual(
            self.record()["defect"]["reproduction_commit"],
            self.red,
        )

    def test_governance_and_mutation_modes_record_failing_red(self) -> None:
        for task_type, mode in (
            ("governance", "red_green"),
            ("test", "mutation"),
        ):
            with self.subTest(mode=mode):
                record = self.record()
                record["task_type"] = task_type
                record["test_contract"]["proof_mode"] = mode
                self.write_record(record)
                self.git("add", str(self.record_path()))
                self.git("commit", "--amend", "--no-edit")
                self.red = self.git("rev-parse", "HEAD")

                checkpoint = self.checkpoint()

                self.assertEqual(checkpoint["mode"], mode)
                self.git("reset", "--hard", "HEAD")

    def test_investigation_and_acceptance_cannot_record_product_red(self) -> None:
        for mode in ("bounded_evidence", "evidence_closure"):
            with self.subTest(mode=mode):
                record = self.record()
                record["test_contract"]["proof_mode"] = mode
                record["test_contract"]["failure_fingerprints"] = []
                self.write_record(record)
                self.git("add", str(self.record_path()))
                self.git("commit", "--amend", "--no-edit")
                self.red = self.git("rev-parse", "HEAD")
                self.assert_checkpoint_error("cannot record a Red checkpoint")

    def test_registration_change_requires_exact_surface_declaration(self) -> None:
        self.write("tests/CMakeLists.txt", "add_test(NAME fixture)\n")
        self.git("add", "tests/CMakeLists.txt")
        self.git("commit", "--amend", "--no-edit")
        self.red = self.git("rev-parse", "HEAD")

        self.assert_checkpoint_error("test registration path must be exact")

    def test_exact_registration_surface_is_accepted(self) -> None:
        record = self.record()
        record["test_contract"]["proof_surface"].append(
            "tests/CMakeLists.txt"
        )
        self.write_record(record)
        self.write("tests/CMakeLists.txt", "add_test(NAME fixture)\n")
        self.git("add", str(self.record_path()), "tests/CMakeLists.txt")
        self.git("commit", "--amend", "--no-edit")
        self.red = self.git("rev-parse", "HEAD")

        checkpoint = self.checkpoint()

        self.assertEqual(checkpoint["red_commit"], self.red)

    def test_governance_context_change_invalidates_checkpoint(self) -> None:
        self.checkpoint()
        self.commit_checkpoint()
        self.write(".github/workflows/ci.yml", "name: changed\n")
        self.write("src/implemented.flag", "implemented\n")
        head = self.commit("test: change workflow after Red")

        with self.assertRaisesRegex(
            tdd_proof.TddProofError,
            "frozen governance context changed",
        ):
            tdd_proof.run_review_proof(self.root, self.task_id, head)

    def test_unrelated_backlog_change_preserves_checkpoint(self) -> None:
        self.checkpoint()
        self.commit_checkpoint()
        backlog_path = self.root / ".agents" / "backlog.yaml"
        backlog = json.loads(backlog_path.read_text(encoding="utf-8"))
        backlog["tasks"].append(
            {
                "id": "XT-900",
                "title": "Unrelated task",
                "owned_paths": ["unrelated/**"],
            }
        )
        backlog_path.write_text(
            json.dumps(backlog, indent=2) + "\n",
            encoding="utf-8",
        )
        self.write("src/implemented.flag", "implemented\n")
        head = self.commit("test: add unrelated task after Red")

        proof = tdd_proof.run_review_proof(self.root, self.task_id, head)

        self.assertIsNotNone(proof)

    def test_non_ancestor_checkpoint_and_authored_fields_fail_closed(self) -> None:
        self.checkpoint()
        record = self.record()
        checkpoint = record["verification"]["tdd_checkpoint"]
        checkpoint["red_commit"] = self.base
        checkpoint["task_authored_result"] = "passed"
        self.write_record(record)
        head = self.commit("test: tamper with checkpoint evidence")

        errors = tdd_proof.validate_tdd_evidence(
            self.root,
            json.loads(
                (self.root / ".agents" / "backlog.yaml").read_text(
                    encoding="utf-8"
                )
            )["tasks"][0],
            record,
            {"focused": "bash focused.sh", "verify": "true"},
            verify_git=True,
        )
        self.assertTrue(
            any("unknown fields" in error for error in errors),
            errors,
        )
        checkpoint.pop("task_authored_result")
        self.write_record(record)
        self.git("add", str(self.record_path()))
        self.git("commit", "--amend", "--no-edit")
        head = self.git("rev-parse", "HEAD")
        with self.assertRaisesRegex(
            tdd_proof.TddProofError,
            "outside the task base",
        ):
            tdd_proof.run_review_proof(self.root, self.task_id, head)

    def test_validation_rejects_tampered_checkpoint_and_requires_review(self) -> None:
        self.checkpoint()
        task = json.loads(
            (self.root / ".agents" / "backlog.yaml").read_text(
                encoding="utf-8"
            )
        )["tasks"][0]
        record = self.record()
        self.assertEqual(
            tdd_proof.validate_tdd_evidence(
                self.root,
                task,
                record,
                {"focused": "bash focused.sh", "verify": "true"},
                verify_git=True,
            ),
            [],
        )

        record["verification"]["tdd_checkpoint"]["command_sha256"] = "0" * 64
        errors = tdd_proof.validate_tdd_evidence(
            self.root,
            task,
            record,
            {"focused": "bash focused.sh", "verify": "true"},
            verify_git=True,
        )
        self.assertTrue(
            any("command_sha256 does not match" in error for error in errors)
        )

        record = self.record()
        record["state"] = "review"
        errors = tdd_proof.validate_tdd_evidence(
            self.root,
            task,
            record,
            {"focused": "bash focused.sh", "verify": "true"},
            verify_git=True,
        )
        self.assertTrue(
            any("verification.tdd_proof must be an object" in error for error in errors)
        )

    def test_mark_review_invokes_schema_v4_tdd_runner(self) -> None:
        record = {
            "schema_version": 4,
            "state": "in_progress",
            "verification": {
                "status": "pending",
                "reference": "",
            },
        }
        output = Path(self.temporary.name) / "review-record.json"
        with ExitStack() as stack:
            stack.enter_context(
                mock.patch.object(
                    governance,
                    "load_record",
                    return_value=record,
                )
            )
            stack.enter_context(
                mock.patch.object(
                    governance,
                    "record_path",
                    return_value=output,
                )
            )
            stack.enter_context(
                mock.patch.object(
                    governance.defect_proof_runner,
                    "run_proof",
                    return_value=None,
                )
            )
            tdd_runner = stack.enter_context(
                mock.patch.object(
                    governance.tdd_proof_runner,
                    "run_review_proof",
                    return_value={"head_commit": "1" * 40},
                )
            )
            stack.enter_context(
                mock.patch.object(governance, "validate_repository")
            )
            governance.mark_review(self.task_id, "1" * 40, "test:proof")

        tdd_runner.assert_called_once_with(
            governance.ROOT,
            self.task_id,
            "1" * 40,
        )
        reviewed = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(reviewed["state"], "review")
        self.assertEqual(reviewed["head_sha"], "1" * 40)


if __name__ == "__main__":
    unittest.main(verbosity=2)
