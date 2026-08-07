#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("commit_message.py")
HOOK_PATH = MODULE_PATH.parents[2] / ".githooks" / "commit-msg"
SPEC = importlib.util.spec_from_file_location("commit_message", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("Cannot load commit_message.py")
commit_message = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(commit_message)


class MessageValidationTest(unittest.TestCase):
    def assert_valid(self, message: str) -> None:
        self.assertEqual(commit_message.validate_message(message), [])

    def assert_invalid(self, message: str, expected: str) -> None:
        errors = commit_message.validate_message(message)
        self.assertTrue(
            any(expected in error for error in errors),
            f"{expected!r} not found in {errors!r}",
        )

    def test_accepts_meaningful_message_with_trailers(self) -> None:
        self.assert_valid(
            "ci(harness): enforce meaningful commit messages\n"
            "\n"
            "Reject vague subjects in local hooks and CI.\n"
            "\n"
            "Xnn-Task: XT-047\n"
            "Xnn-Lifecycle: delivery\n"
        )

    def test_accepts_standard_types(self) -> None:
        for commit_type in sorted(commit_message.ALLOWED_TYPES):
            with self.subTest(commit_type=commit_type):
                self.assert_valid(
                    f"{commit_type}(native): validate frame boundaries\n"
                )

    def test_rejects_missing_scope(self) -> None:
        self.assert_invalid(
            "fix: validate frame boundaries\n",
            "type(scope): summary",
        )

    def test_rejects_vague_summary(self) -> None:
        for summary in (
            "update files",
            "fix issue",
            "address review comments",
            "work in progress",
        ):
            with self.subTest(summary=summary):
                self.assert_invalid(
                    f"chore(harness): {summary}\n",
                    "summary is vague",
                )

    def test_rejects_task_id_in_subject_or_body(self) -> None:
        self.assert_invalid(
            "chore(harness): deliver XT-047\n",
            "task IDs belong",
        )
        self.assert_invalid(
            "ci(harness): enforce meaningful commit messages\n"
            "\n"
            "Implements XT-047.\n",
            "only allowed in an Xnn-Task trailer",
        )

    def test_rejects_malformed_or_separated_trailers(self) -> None:
        self.assert_invalid(
            "ci(harness): enforce meaningful commit messages\n"
            "\n"
            "Xnn-Task XT-047\n",
            "only allowed in an Xnn-Task trailer",
        )
        self.assert_invalid(
            "ci(harness): enforce meaningful commit messages\n"
            "\n"
            "Xnn-Task: XT-047\n"
            "\n"
            "Xnn-Lifecycle: delivery\n",
            "must not contain blank lines",
        )

    def test_rejects_uppercase_summary_and_long_subject(self) -> None:
        self.assert_invalid(
            "fix(native): Validate frame boundaries\n",
            "lowercase imperative verb",
        )
        self.assert_invalid(
            "fix(native): " + "validate " + ("frame " * 12) + "\n",
            "must not exceed 72",
        )


class IdentityValidationTest(unittest.TestCase):
    expected = commit_message.CommitIdentity(
        name="chenzhuoran",
        email="chenzhuoran.638@bytedance.com",
    )
    wrong = commit_message.CommitIdentity(
        name="blackfrog638",
        email="blackfrog638@gmail.com",
    )

    def policy_source(self) -> str:
        return json.dumps(
            {
                "schema_version": 1,
                "name": self.expected.name,
                "email": self.expected.email,
            }
        )

    def git(self, root: Path, *args: str) -> str:
        result = subprocess.run(
            ["git", "-C", str(root), *args],
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout.strip()

    def test_parses_canonical_identity_policy(self) -> None:
        self.assertEqual(
            commit_message.parse_identity_policy(self.policy_source()),
            self.expected,
        )

    def test_repository_policy_matches_canonical_identity(self) -> None:
        self.assertEqual(
            commit_message.load_identity_policy(MODULE_PATH.parents[2]),
            self.expected,
        )

    def test_rejects_malformed_identity_policy(self) -> None:
        invalid_sources = (
            "{",
            "[]",
            '{"schema_version": 2, "name": "blackfrog638", '
            '"email": "blackfrog638@gmail.com"}',
            '{"schema_version": 1, "name": "blackfrog638\\n", '
            '"email": "blackfrog638@gmail.com"}',
            '{"schema_version": 1, "name": "blackfrog638", '
            '"email": "blackfrog638 @gmail.com"}',
        )
        for source in invalid_sources:
            with self.subTest(source=source):
                with self.assertRaises(commit_message.CommitMessageError):
                    commit_message.parse_identity_policy(source)

    def test_reports_author_and_committer_independently(self) -> None:
        self.assertEqual(
            commit_message.validate_identities(
                self.expected,
                self.wrong,
                self.expected,
            ),
            [
                "author identity must be chenzhuoran "
                "<chenzhuoran.638@bytedance.com>; got blackfrog638 "
                "<blackfrog638@gmail.com>"
            ],
        )
        self.assertEqual(
            commit_message.validate_identities(
                self.expected,
                self.expected,
                self.wrong,
            ),
            [
                "committer identity must be chenzhuoran "
                "<chenzhuoran.638@bytedance.com>; got blackfrog638 "
                "<blackfrog638@gmail.com>"
            ],
        )

    def test_configure_repairs_local_identity_for_hook_check(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.git(root, "init", "-q")
            policy = root / commit_message.IDENTITY_POLICY_PATH
            policy.parent.mkdir(parents=True)
            policy.write_text(self.policy_source(), encoding="utf-8")
            self.git(root, "config", "user.name", self.wrong.name)
            self.git(root, "config", "user.email", self.wrong.email)

            with self.assertRaises(commit_message.CommitMessageError):
                commit_message.check_current_identity(root)

            commit_message.configure_identity(root)
            commit_message.check_current_identity(root)
            self.assertEqual(
                self.git(root, "config", "--local", "user.name"),
                self.expected.name,
            )
            self.assertEqual(
                self.git(root, "config", "--local", "user.email"),
                self.expected.email,
            )

    def test_commit_hook_rejects_wrong_local_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.git(root, "init", "-q")
            policy = root / commit_message.IDENTITY_POLICY_PATH
            policy.parent.mkdir(parents=True)
            policy.write_text(self.policy_source(), encoding="utf-8")

            validator = root / "tool" / "harness" / "commit_message.py"
            validator.parent.mkdir(parents=True)
            shutil.copy2(MODULE_PATH, validator)
            hook = root / ".githooks" / "commit-msg"
            hook.parent.mkdir()
            shutil.copy2(HOOK_PATH, hook)
            self.git(root, "config", "core.hooksPath", ".githooks")
            self.git(root, "config", "user.name", self.wrong.name)
            self.git(root, "config", "user.email", self.wrong.email)

            payload = root / "payload.txt"
            payload.write_text("payload\n", encoding="utf-8")
            self.git(root, "add", "payload.txt")
            result = subprocess.run(
                [
                    "git",
                    "-C",
                    str(root),
                    "commit",
                    "-m",
                    "test(harness): reject incorrect commit identity",
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("author identity must be", result.stderr)
            self.assertIn("committer identity must be", result.stderr)

            commit_message.configure_identity(root)
            self.git(
                root,
                "commit",
                "-m",
                "test(harness): accept configured commit identity",
            )


class RangeValidationTest(unittest.TestCase):
    expected_name = "chenzhuoran"
    expected_email = "chenzhuoran.638@bytedance.com"
    wrong_name = "blackfrog638"
    wrong_email = "blackfrog638@gmail.com"

    def git(self, root: Path, *args: str) -> str:
        result = subprocess.run(
            ["git", "-C", str(root), *args],
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout.strip()

    def commit(self, root: Path, subject: str, body: str = "") -> str:
        arguments = ["commit", "--allow-empty", "-m", subject]
        if body:
            arguments.extend(["-m", body])
        self.git(root, *arguments)
        return self.git(root, "rev-parse", "HEAD")

    def configure_expected_identity(self, root: Path) -> None:
        self.git(root, "config", "user.name", self.expected_name)
        self.git(root, "config", "user.email", self.expected_email)

    def activate_identity_policy(self, root: Path) -> str:
        message_policy = root / commit_message.POLICY_PATH
        message_policy.parent.mkdir(parents=True, exist_ok=True)
        message_policy.write_text("policy\n", encoding="utf-8")
        identity_policy = root / commit_message.IDENTITY_POLICY_PATH
        identity_policy.parent.mkdir(parents=True, exist_ok=True)
        identity_policy.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "name": self.expected_name,
                    "email": self.expected_email,
                }
            )
            + "\n",
            encoding="utf-8",
        )
        self.git(
            root,
            "add",
            commit_message.POLICY_PATH,
            commit_message.IDENTITY_POLICY_PATH,
        )
        self.git(
            root,
            "commit",
            "-m",
            "ci(harness): enforce repository commit identity",
        )
        return self.git(root, "rev-parse", "HEAD")

    def test_range_exempts_history_before_policy_activation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.git(root, "init", "-q")
            self.git(root, "config", "user.name", "Commit Policy Test")
            self.git(root, "config", "user.email", "commit-policy@example.test")

            self.commit(root, "legacy: vague")
            base = self.git(root, "rev-parse", "HEAD")
            policy = root / commit_message.POLICY_PATH
            policy.parent.mkdir(parents=True)
            policy.write_text("policy\n", encoding="utf-8")
            self.git(root, "add", commit_message.POLICY_PATH)
            self.git(
                root,
                "commit",
                "-m",
                "docs(harness): define commit message policy",
            )
            self.commit(root, "harness: deliver XT-999")
            head = self.git(root, "rev-parse", "HEAD")

            checked, failures = commit_message.validate_range(root, base, head)
            self.assertEqual(checked, 2)
            self.assertTrue(any("harness: deliver" in item for item in failures))

    def test_range_rejects_no_verify_identity_bypass(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.git(root, "init", "-q")
            self.configure_expected_identity(root)
            self.commit(root, "legacy: initialize")
            base = self.git(root, "rev-parse", "HEAD")
            self.activate_identity_policy(root)
            self.git(
                root,
                "-c",
                f"user.name={self.wrong_name}",
                "-c",
                f"user.email={self.wrong_email}",
                "commit",
                "--allow-empty",
                "--no-verify",
                "-m",
                "fix(harness): preserve repository attribution",
            )
            head = self.git(root, "rev-parse", "HEAD")

            checked, failures = commit_message.validate_range(root, base, head)
            self.assertEqual(checked, 2)
            self.assertTrue(
                any("author identity must be" in item for item in failures)
            )
            self.assertTrue(
                any("committer identity must be" in item for item in failures)
            )

    def test_range_exempts_wrong_identity_before_activation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.git(root, "init", "-q")
            self.configure_expected_identity(root)
            self.commit(root, "legacy: initialize")
            base = self.git(root, "rev-parse", "HEAD")
            self.git(
                root,
                "-c",
                f"user.name={self.wrong_name}",
                "-c",
                f"user.email={self.wrong_email}",
                "commit",
                "--allow-empty",
                "-m",
                "legacy: misattributed history",
            )
            self.activate_identity_policy(root)
            head = self.git(root, "rev-parse", "HEAD")

            checked, failures = commit_message.validate_range(root, base, head)
            self.assertEqual(checked, 1)
            self.assertEqual(failures, [])


if __name__ == "__main__":
    unittest.main()
