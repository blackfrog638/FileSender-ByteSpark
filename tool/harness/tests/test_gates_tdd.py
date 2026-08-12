#!/usr/bin/env python3
"""Tests for Harness V2 Gate execution and TDD attestations."""

from __future__ import annotations

import shutil
import os
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

HARNESS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HARNESS))

import executor  # noqa: E402
import gates  # noqa: E402
import model  # noqa: E402
import state  # noqa: E402
import tdd  # noqa: E402
import workspace  # noqa: E402
from test_model import ContractFixture  # noqa: E402
from test_state_workspace import ACTOR  # noqa: E402


class GateRepository:
    def __init__(self, testcase: unittest.TestCase) -> None:
        self.fixture = ContractFixture(testcase)
        self.root = self.fixture.root
        self.external = Path(self.fixture.temporary.name + "-external")
        self.external.mkdir()
        testcase.addCleanup(lambda: shutil.rmtree(self.external, ignore_errors=True))
        subprocess.run(
            ["git", "init", "-q", "-b", "harness", str(self.root)], check=True
        )
        self.git("config", "user.name", "Project Owner")
        self.git("config", "user.email", "owner@example.com")
        (self.root / "docs").mkdir()
        (self.root / "docs" / "roadmap.md").write_text("# Roadmap\n", encoding="utf-8")
        (self.root / "native" / "tests").mkdir(parents=True)
        (self.root / "native" / "tests" / "feature_test.cpp").write_text(
            "// test\n", encoding="utf-8"
        )

    def git(self, *arguments: str) -> str:
        return subprocess.run(
            ["git", "-C", str(self.root), *arguments],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()

    def commit(self, message: str, allow_empty: bool = False) -> str:
        self.git("add", ".")
        arguments = ["commit", "-q", "-m", message]
        if allow_empty:
            arguments.insert(1, "--allow-empty")
        self.git(*arguments)
        return self.git("rev-parse", "HEAD")

    def configure_command(
        self, gate_id: str, argv: list, group: str = "lightweight"
    ) -> None:
        gate = self.fixture.gates["gates"][gate_id]
        gate["command"]["argv"] = argv
        gate["resource_group"] = group
        self.fixture.write()

    def load(self) -> model.ContractSet:
        return model.load_contracts(self.root)


class GatePlanTest(unittest.TestCase):
    def test_expands_unique_leaf_plan_with_reasons(self) -> None:
        repository = GateRepository(self)
        repository.commit("chore: initialize")
        contracts = repository.load()
        review = gates.plan_gates(
            contracts,
            "XT-101",
            "review",
            ["native/src/feature.cpp"],
        )
        self.assertEqual(set(review.leaves), {"feature_test", "governance"})
        self.assertEqual(len(review.leaves), len(set(review.leaves)))
        self.assertIn("criterion:CRIT-EXAMPLE-BEHAVIOR", review.reasons["feature_test"])
        self.assertIn("path-rule:0", review.reasons["feature_test"])
        self.assertIn("phase:review", review.reasons["governance"])

        queue = gates.plan_gates(contracts, "XT-101", "queue")
        self.assertEqual(set(queue.leaves), {"feature_test", "governance"})
        self.assertIn("aggregate:verify", queue.reasons["governance"])

    def test_plan_digest_is_stable(self) -> None:
        repository = GateRepository(self)
        repository.commit("chore: initialize")
        contracts = repository.load()
        left = gates.plan_gates(contracts, "XT-101", "review")
        right = gates.plan_gates(contracts, "XT-101", "review")
        self.assertEqual(left.digest, right.digest)

    def test_candidate_prefix_unions_task_gate_reasons(self) -> None:
        repository = GateRepository(self)
        repository.commit("chore: initialize")
        plan = gates.plan_task_set(
            repository.load(),
            ["XT-101", "XT-102"],
            "queue",
            ["native/src/feature.cpp"],
        )
        self.assertEqual(set(plan.leaves), {"feature_test", "governance"})
        self.assertIn("task:XT-101", plan.reasons["feature_test"])
        self.assertIn("task:XT-102", plan.reasons["feature_test"])
        self.assertEqual(len(plan.leaves), len(set(plan.leaves)))

    def test_platform_plan_filters_unsupported_leaves(self) -> None:
        repository = GateRepository(self)
        repository.fixture.gates["gates"]["governance"]["platforms"] = [
            "local",
            "linux",
        ]
        repository.fixture.gates["gates"]["feature_test"]["platforms"] = [
            "local",
            "linux",
            "macos",
        ]
        repository.fixture.write()
        repository.commit("chore: initialize")
        plan = gates.plan_gates(repository.load(), "XT-101", "queue")
        macos = gates.plan_for_platform(repository.load(), plan, "macos")
        self.assertEqual(macos.leaves, ("feature_test",))


class GateExecutorTest(unittest.TestCase):
    def test_reported_skip_is_not_success_or_cacheable(self) -> None:
        repository = GateRepository(self)
        repository.configure_command(
            "feature_test",
            ["python3", "-c", "print('OK (skipped=2)')"],
        )
        repository.commit("chore: initialize")
        contracts = repository.load()
        plan = gates.single_gate_plan(contracts, "XT-101", "review", "feature_test")
        first = executor.GateExecutor(contracts).execute(plan)
        second = executor.GateExecutor(contracts).execute(plan)
        self.assertEqual(first.results[0].outcome, "skipped")
        self.assertTrue(first.results[0].attestation["skipped"])
        self.assertFalse(first.results[0].cached)
        self.assertFalse(second.results[0].cached)

    def test_inherited_environment_changes_cache_key(self) -> None:
        repository = GateRepository(self)
        repository.configure_command(
            "feature_test",
            [
                "python3",
                "-c",
                "import os; print(os.environ['XNN_DYNAMIC_TEST'])",
            ],
        )
        repository.commit("chore: initialize")
        contracts = repository.load()
        plan = gates.single_gate_plan(contracts, "XT-101", "review", "feature_test")
        with mock.patch.dict(os.environ, {"XNN_DYNAMIC_TEST": "first"}):
            first = executor.GateExecutor(contracts).execute(plan)
        with mock.patch.dict(os.environ, {"XNN_DYNAMIC_TEST": "second"}):
            second = executor.GateExecutor(contracts).execute(plan)
        self.assertFalse(first.results[0].cached)
        self.assertFalse(second.results[0].cached)
        self.assertNotEqual(
            first.results[0].attestation["environment_sha256"],
            second.results[0].attestation["environment_sha256"],
        )

    def test_bounds_output_and_classifies_limit(self) -> None:
        repository = GateRepository(self)
        repository.configure_command(
            "feature_test",
            ["python3", "-c", "print('x' * 4096)"],
        )
        repository.commit("chore: initialize")
        plan = gates.single_gate_plan(
            repository.load(), "XT-101", "review", "feature_test"
        )
        with mock.patch.object(executor, "MAX_OUTPUT_BYTES", 1024):
            result = executor.GateExecutor(
                repository.load(), cache_enabled=False
            ).execute(plan)
        self.assertEqual(result.results[0].outcome, "output_limit")
        self.assertGreater(result.results[0].attestation["output_bytes"], 1024)

    def test_failure_summary_includes_bounded_gate_diagnostic(self) -> None:
        repository = GateRepository(self)
        repository.configure_command(
            "feature_test",
            [
                "python3",
                "-c",
                "import sys; print('HOSTED-FAILURE-DETAIL'); sys.exit(1)",
            ],
        )
        repository.commit("chore: initialize")
        plan = gates.single_gate_plan(
            repository.load(), "XT-101", "review", "feature_test"
        )
        result = executor.GateExecutor(repository.load(), cache_enabled=False).execute(
            plan
        )
        with self.assertRaisesRegex(
            executor.GateExecutionError,
            "HOSTED-FAILURE-DETAIL",
        ):
            result.require_success()

    def test_classifies_signal_exit_as_crash(self) -> None:
        if sys.platform == "win32":
            return
        repository = GateRepository(self)
        repository.configure_command(
            "feature_test",
            [
                "python3",
                "-c",
                "import os, signal; os.kill(os.getpid(), signal.SIGKILL)",
            ],
        )
        repository.commit("chore: initialize")
        result = executor.GateExecutor(repository.load(), cache_enabled=False).execute(
            gates.single_gate_plan(
                repository.load(), "XT-101", "review", "feature_test"
            )
        )
        self.assertEqual(result.results[0].outcome, "crash")

    def test_success_cache_reuses_same_tree_across_commit_sha(self) -> None:
        repository = GateRepository(self)
        counter = repository.external / "counter.txt"
        code = (
            "from pathlib import Path; "
            "p=Path({!r}); "
            "n=int(p.read_text()) if p.exists() else 0; "
            "p.write_text(str(n+1)); "
            "print('executed')"
        ).format(str(counter))
        repository.configure_command("feature_test", ["python3", "-c", code])
        repository.commit("chore: initialize")
        contracts = repository.load()
        plan = gates.single_gate_plan(contracts, "XT-101", "review", "feature_test")
        first = executor.GateExecutor(contracts).execute(plan)
        self.assertTrue(first.passed)
        self.assertFalse(first.results[0].cached)
        self.assertEqual(counter.read_text(), "1")

        repository.commit("chore: metadata-only commit", allow_empty=True)
        second = executor.GateExecutor(contracts).execute(plan)
        self.assertTrue(second.results[0].cached)
        self.assertEqual(counter.read_text(), "1")
        self.assertNotEqual(
            first.results[0].attestation["source_sha"],
            second.results[0].attestation["source_sha"],
        )
        self.assertEqual(
            second.results[0].attestation["reused_from_source_sha"],
            first.results[0].attestation["source_sha"],
        )

        (repository.root / "README.md").write_text("changed tree\n", encoding="utf-8")
        repository.commit("docs: change tree")
        third = executor.GateExecutor(contracts).execute(plan)
        self.assertFalse(third.results[0].cached)
        self.assertEqual(counter.read_text(), "2")

    def test_failure_is_not_cached(self) -> None:
        repository = GateRepository(self)
        counter = repository.external / "failure-counter.txt"
        code = (
            "from pathlib import Path; import sys; "
            "p=Path({!r}); "
            "n=int(p.read_text()) if p.exists() else 0; "
            "p.write_text(str(n+1)); "
            "print('FAILED: deterministic failure'); sys.exit(1)"
        ).format(str(counter))
        repository.configure_command("feature_test", ["python3", "-c", code])
        repository.commit("chore: initialize")
        contracts = repository.load()
        plan = gates.single_gate_plan(contracts, "XT-101", "review", "feature_test")
        first = executor.GateExecutor(contracts).execute(plan)
        second = executor.GateExecutor(contracts).execute(plan)
        self.assertEqual(first.results[0].outcome, "failure")
        self.assertEqual(second.results[0].outcome, "failure")
        self.assertFalse(first.results[0].cached)
        self.assertFalse(second.results[0].cached)
        self.assertEqual(counter.read_text(), "2")

    def test_dirty_worktree_is_rejected(self) -> None:
        repository = GateRepository(self)
        repository.commit("chore: initialize")
        contracts = repository.load()
        (repository.root / "dirty.txt").write_text("dirty\n", encoding="utf-8")
        plan = gates.single_gate_plan(contracts, "XT-101", "review", "feature_test")
        with self.assertRaisesRegex(executor.GateExecutionError, "clean worktree"):
            executor.GateExecutor(contracts).execute(plan)

    def test_independent_resource_groups_overlap(self) -> None:
        repository = GateRepository(self)
        governance_times = repository.external / "governance-times.txt"
        feature_times = repository.external / "feature-times.txt"

        def timed_command(path: Path, label: str) -> list[str]:
            code = (
                "from pathlib import Path; import time; "
                "p=Path({!r}); "
                "p.write_text(str(time.monotonic_ns())+'\\n'); "
                "time.sleep(0.35); "
                "p.write_text(p.read_text()+str(time.monotonic_ns())+'\\n'); "
                "print({!r})"
            ).format(str(path), label)
            return ["python3", "-c", code]

        repository.configure_command(
            "governance",
            timed_command(governance_times, "governance"),
        )
        repository.configure_command(
            "feature_test",
            timed_command(feature_times, "feature"),
            "native_build",
        )
        repository.commit("chore: initialize")
        contracts = repository.load()
        plan = gates.plan_gates(contracts, "XT-101", "review")
        result = executor.GateExecutor(contracts, cache_enabled=False).execute(plan)
        self.assertTrue(result.passed)
        governance_interval = tuple(
            int(value) for value in governance_times.read_text().splitlines()
        )
        feature_interval = tuple(
            int(value) for value in feature_times.read_text().splitlines()
        )
        self.assertLess(
            max(governance_interval[0], feature_interval[0]),
            min(governance_interval[1], feature_interval[1]),
        )


class TddTest(unittest.TestCase):
    def _fixture(self) -> tuple:
        repository = GateRepository(self)
        fingerprint = "FAILED: expected feature behavior is unavailable"
        code = (
            "from pathlib import Path; import sys; "
            "red=Path('native/tests/red.txt').exists(); "
            "green=Path('native/src/impl.txt').exists(); "
            "print({!r}) if red and not green else None; "
            "sys.exit(1 if red and not green else 0)"
        ).format(fingerprint)
        repository.configure_command("feature_test", ["python3", "-c", code])
        repository.commit("chore: initialize")
        contracts = repository.load()
        store = state.StateStore(
            contracts,
            actor=ACTOR,
            clock=lambda: "2026-08-12T12:00:00Z",
        )
        manager = workspace.WorkspaceManager(contracts, store)
        worktree = Path(repository.fixture.temporary.name + "-XT-101")
        self.addCleanup(lambda: shutil.rmtree(worktree, ignore_errors=True))
        manager.claim("XT-101", worktree)
        return repository, contracts, store, manager, worktree

    def _commit(self, worktree: Path, message: str) -> str:
        subprocess.run(["git", "-C", str(worktree), "add", "."], check=True)
        subprocess.run(
            ["git", "-C", str(worktree), "commit", "-q", "-m", message],
            check=True,
        )
        return subprocess.run(
            ["git", "-C", str(worktree), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()

    def test_records_red_once_and_reviews_green_without_red_replay(self) -> None:
        repository, contracts, store, manager, worktree = self._fixture()
        (worktree / "native" / "tests" / "red.txt").write_text(
            "red\n", encoding="utf-8"
        )
        red_sha = self._commit(worktree, "test: expose missing feature")
        tdd_manager = tdd.TddManager(contracts, store, manager)
        red = tdd_manager.record_red("XT-101")
        self.assertEqual(red["red_sha"], red_sha)
        self.assertEqual(red["base_gate"]["outcome"], "success")
        self.assertEqual(red["red_gate"]["outcome"], "failure")

        # Idempotent recording reads the immutable attestation.
        self.assertEqual(tdd_manager.record_red("XT-101"), red)

        (worktree / "native" / "src").mkdir()
        (worktree / "native" / "src" / "impl.txt").write_text(
            "green\n", encoding="utf-8"
        )
        green_sha = self._commit(worktree, "feat: implement behavior")
        proof = tdd_manager.review_green("XT-101", red_sha)
        self.assertEqual(proof["green_sha"], green_sha)
        self.assertEqual(proof["green_gate"]["outcome"], "success")

    def test_rejects_production_before_red_even_if_test_follows(self) -> None:
        _, contracts, store, manager, worktree = self._fixture()
        (worktree / "native" / "src").mkdir()
        (worktree / "native" / "src" / "premature.txt").write_text(
            "implementation\n", encoding="utf-8"
        )
        self._commit(worktree, "feat: implement before test")
        (worktree / "native" / "tests" / "red.txt").write_text(
            "red\n", encoding="utf-8"
        )
        self._commit(worktree, "test: add late test")
        with self.assertRaisesRegex(tdd.TddError, "before Red"):
            tdd.TddManager(contracts, store, manager).record_red("XT-101")

    def test_rejects_changed_frozen_test_after_red(self) -> None:
        _, contracts, store, manager, worktree = self._fixture()
        red_path = worktree / "native" / "tests" / "red.txt"
        red_path.write_text("red\n", encoding="utf-8")
        red_sha = self._commit(worktree, "test: expose missing feature")
        tdd_manager = tdd.TddManager(contracts, store, manager)
        tdd_manager.record_red("XT-101")
        red_path.write_text("weakened\n", encoding="utf-8")
        (worktree / "native" / "src").mkdir()
        (worktree / "native" / "src" / "impl.txt").write_text(
            "green\n", encoding="utf-8"
        )
        self._commit(worktree, "feat: change test and implementation")
        with self.assertRaisesRegex(tdd.TddError, "frozen"):
            tdd_manager.review_green("XT-101", red_sha)

    def test_rejects_infrastructure_failure_as_red(self) -> None:
        repository = GateRepository(self)
        repository.configure_command(
            "feature_test", ["xnn-command-that-does-not-exist"]
        )
        repository.commit("chore: initialize")
        contracts = repository.load()
        store = state.StateStore(contracts, actor=ACTOR)
        manager = workspace.WorkspaceManager(contracts, store)
        worktree = Path(repository.fixture.temporary.name + "-XT-101")
        self.addCleanup(lambda: shutil.rmtree(worktree, ignore_errors=True))
        manager.claim("XT-101", worktree)
        (worktree / "native" / "tests" / "red.txt").write_text(
            "red\n", encoding="utf-8"
        )
        self._commit(worktree, "test: add red")
        with self.assertRaisesRegex(tdd.TddError, "does not pass at base"):
            tdd.TddManager(contracts, store, manager).record_red("XT-101")

    def test_rejects_failure_with_skipped_test_as_red(self) -> None:
        repository = GateRepository(self)
        fingerprint = "FAILED: expected feature behavior is unavailable"
        code = (
            "from pathlib import Path; import sys; "
            "red=Path('native/tests/red.txt').exists(); "
            "print({!r}) if red else None; "
            "print('1 skipped') if red else None; "
            "sys.exit(1 if red else 0)"
        ).format(fingerprint)
        repository.configure_command("feature_test", ["python3", "-c", code])
        repository.commit("chore: initialize")
        contracts = repository.load()
        store = state.StateStore(contracts, actor=ACTOR)
        manager = workspace.WorkspaceManager(contracts, store)
        worktree = Path(repository.fixture.temporary.name + "-XT-101")
        self.addCleanup(lambda: shutil.rmtree(worktree, ignore_errors=True))
        manager.claim("XT-101", worktree)
        (worktree / "native" / "tests" / "red.txt").write_text(
            "red\n", encoding="utf-8"
        )
        self._commit(worktree, "test: add partially skipped red")
        with self.assertRaisesRegex(tdd.TddError, "attributed assertion failure"):
            tdd.TddManager(contracts, store, manager).record_red("XT-101")


if __name__ == "__main__":
    unittest.main()
