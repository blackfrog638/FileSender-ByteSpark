#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("commit_message.py")
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


class RangeValidationTest(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
