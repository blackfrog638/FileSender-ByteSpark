#!/usr/bin/env python3

from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path

import task_conflicts


class PatternOverlapTest(unittest.TestCase):
    def test_exact_paths_must_match(self) -> None:
        self.assertTrue(task_conflicts.patterns_overlap("a/b.cpp", "a/b.cpp"))
        self.assertFalse(task_conflicts.patterns_overlap("a/b.cpp", "a/c.cpp"))

    def test_exact_path_intersects_glob(self) -> None:
        self.assertTrue(
            task_conflicts.patterns_overlap(
                "native/src/protocol/parser.cpp",
                "native/src/protocol/**",
            )
        )

    def test_recursive_prefixes_intersect(self) -> None:
        self.assertTrue(
            task_conflicts.patterns_overlap(
                "native/src/**/CMakeLists.txt",
                "native/src/security/**",
            )
        )

    def test_disjoint_prefixes_do_not_intersect(self) -> None:
        self.assertFalse(
            task_conflicts.patterns_overlap(
                "apps/desktop/lib/**",
                "native/src/**",
            )
        )

    def test_uncertain_same_prefix_fails_conservatively(self) -> None:
        self.assertTrue(task_conflicts.patterns_overlap("foo/*a", "foo/*b"))


class RepositoryTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "repository"
        self.root.mkdir()
        self.git("init", "-q")
        self.git("config", "user.name", "Task Conflict Test")
        self.git("config", "user.email", "task-conflict@example.test")
        tasks = [
            {
                "id": "XT-100",
                "owned_paths": ["native/src/foo/**"],
            },
            {
                "id": "XT-101",
                "owned_paths": ["native/src/foo/parser.cpp"],
            },
            {
                "id": "XT-102",
                "owned_paths": ["native/src/bar/**"],
            },
        ]
        agents = self.root / ".agents"
        records = agents / "records"
        records.mkdir(parents=True)
        (agents / "backlog.yaml").write_text(
            json.dumps({"schema_version": 1, "tasks": tasks}, indent=2) + "\n",
            encoding="utf-8",
        )
        for task in tasks:
            self.write_record(task["id"], "ready")
        owned = self.root / "native" / "src" / "foo" / "old.cpp"
        owned.parent.mkdir(parents=True)
        owned.write_text("old\n", encoding="utf-8")
        self.git("add", ".")
        self.git("commit", "-m", "test: create conflict fixture")
        self.base = self.git("rev-parse", "HEAD")

    def git(self, *args: str) -> str:
        result = subprocess.run(
            ["git", "-C", str(self.root), *args],
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout.strip()

    def write_record(self, task_id: str, state: str) -> None:
        path = self.root / ".agents" / "records" / f"{task_id}.json"
        path.write_text(
            json.dumps(
                {
                    "id": task_id,
                    "state": state,
                    "owner": "test-agent" if state != "ready" else "unassigned",
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )

    def set_durable_state(self, task_id: str, state: str) -> None:
        self.write_record(task_id, state)
        self.git("add", f".agents/records/{task_id}.json")
        self.git("commit", "-m", f"test: mark {task_id} {state}")

    def set_branch_state(self, task_id: str, state: str) -> None:
        worktree = Path(self.temporary.name) / f"worktree-{task_id}"
        self.git("worktree", "add", "-q", "-b", f"task/{task_id}", str(worktree))
        path = worktree / ".agents" / "records" / f"{task_id}.json"
        record = json.loads(path.read_text(encoding="utf-8"))
        record["state"] = state
        record["owner"] = "branch-agent"
        path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        subprocess.run(
            ["git", "-C", str(worktree), "add", str(path)],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(worktree), "commit", "-m", "test: activate task"],
            check=True,
            capture_output=True,
            text=True,
        )
        self.git("worktree", "remove", str(worktree))

    def commit_file(self, relative: str, content: str) -> str:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        self.git("add", relative)
        self.git("commit", "-m", f"test: change {relative}")
        return self.git("rev-parse", "HEAD")

    def test_active_branch_ownership_blocks_claim(self) -> None:
        self.set_branch_state("XT-101", "in_progress")

        conflicts = task_conflicts.claim_conflicts(self.root, "XT-100")

        self.assertEqual(
            conflicts,
            [
                (
                    "XT-101",
                    "native/src/foo/**",
                    "native/src/foo/parser.cpp",
                )
            ],
        )

    def test_ready_and_done_tasks_do_not_block_claim(self) -> None:
        self.set_branch_state("XT-101", "review")
        self.set_durable_state("XT-101", "done")

        self.assertEqual(
            task_conflicts.claim_conflicts(self.root, "XT-100"),
            [],
        )

    def test_unrelated_upstream_change_allows_stale_base(self) -> None:
        self.commit_file("native/src/bar/value.cpp", "bar\n")

        self.assertEqual(
            task_conflicts.check_stale_base(
                self.root,
                "XT-100",
                self.base,
                "HEAD",
            ),
            [],
        )

    def test_owned_upstream_change_rejects_stale_base(self) -> None:
        self.commit_file("native/src/foo/parser.cpp", "changed\n")

        with self.assertRaisesRegex(
            task_conflicts.TaskConflictError,
            "native/src/foo/parser.cpp",
        ):
            task_conflicts.check_stale_base(
                self.root,
                "XT-100",
                self.base,
                "HEAD",
            )

    def test_global_governance_change_rejects_stale_base(self) -> None:
        self.commit_file("tool/harness/policy.py", "changed\n")

        with self.assertRaisesRegex(
            task_conflicts.TaskConflictError,
            "tool/harness/policy.py",
        ):
            task_conflicts.check_stale_base(
                self.root,
                "XT-102",
                self.base,
                "HEAD",
            )

    def test_rename_reports_owned_deletion_side(self) -> None:
        destination = self.root / "native" / "src" / "bar" / "moved.cpp"
        destination.parent.mkdir(parents=True, exist_ok=True)
        (self.root / "native" / "src" / "foo" / "old.cpp").rename(destination)
        self.git("add", "-A")
        self.git("commit", "-m", "test: move owned file")

        with self.assertRaisesRegex(
            task_conflicts.TaskConflictError,
            "native/src/foo/old.cpp",
        ):
            task_conflicts.check_stale_base(
                self.root,
                "XT-100",
                self.base,
                "HEAD",
            )

    def test_diverged_base_is_rejected(self) -> None:
        self.commit_file("native/src/bar/main.cpp", "main\n")
        side = Path(self.temporary.name) / "side"
        self.git("worktree", "add", "-q", "-b", "side", str(side), self.base)
        side_file = side / "side.txt"
        side_file.write_text("side\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(side), "add", "side.txt"], check=True)
        subprocess.run(
            ["git", "-C", str(side), "commit", "-m", "test: diverge"],
            check=True,
            capture_output=True,
            text=True,
        )
        side_sha = subprocess.run(
            ["git", "-C", str(side), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        self.git("worktree", "remove", str(side))

        with self.assertRaisesRegex(
            task_conflicts.TaskConflictError,
            "has diverged",
        ):
            task_conflicts.check_stale_base(
                self.root,
                "XT-100",
                side_sha,
                "HEAD",
            )


if __name__ == "__main__":
    unittest.main()
