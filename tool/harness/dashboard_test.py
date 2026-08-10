#!/usr/bin/env python3

"""Focused tests for the read-only project delivery dashboard.

The dashboard must never disagree with the existing reports, so these tests
assert that its aggregation matches:

- ``agent.sh list`` runtime-state resolution, including active states read from
  task branches rather than only durable records;
- ``delivery_plan`` requirement and scheduler state;
- roadmap milestone parsing and the manifest gate registry;
- HTML escaping of untrusted backlog text.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
HARNESS_DIR = Path(__file__).resolve().parent
if str(HARNESS_DIR) not in sys.path:
    sys.path.insert(0, str(HARNESS_DIR))

import dashboard
import delivery_plan


def write_json(path: Path, value: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def git(root: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(root), *args],
        check=True,
        capture_output=True,
        text=True,
    )


class DashboardTest(unittest.TestCase):
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
  governance_test: make governance-test
  dashboard_test: make dashboard-test

workstreams:
  integration:
    owns:
      - .agents/**
""",
            encoding="utf-8",
        )
        (self.root / "docs" / "roadmap.md").write_text(
            """# Roadmap

## P0 foundation

- [x] Lay the engineering baseline

## P1 vertical slice

<!-- roadmap-id: RM-P1-TRANSFER -->
- [ ] Transfer one accepted file
""",
            encoding="utf-8",
        )
        self.tasks = [
            self._task("XT-063", "Delivery plan governance", [], plan_bound=False),
            self._task("XT-064", "Implement <transfer>", ["XT-063"], role="implementation"),
            self._task("XT-065", "Accept transfer", ["XT-064"], role="acceptance"),
        ]
        self.states = {"XT-063": "done", "XT-064": "ready", "XT-065": "ready"}
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
        self._flush()
        self._init_git()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _task(
        self,
        task_id: str,
        title: str,
        dependencies: list[str],
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
            "owned_paths": [".agents/**"],
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

    def _record(self, task: dict[str, object], state: str) -> dict[str, object]:
        task_id = str(task["id"])
        record: dict[str, object] = {
            "schema_version": 3,
            "id": task_id,
            "task_type": "feature",
            "state": state,
            "owner": "test-owner" if state != "ready" else "unassigned",
            "verification": {"status": "pending"},
        }
        if task.get("delivery_plan"):
            record.update(
                {
                    "delivery_plan": task["delivery_plan"],
                    "requirement_ids": task["requirement_ids"],
                    "delivery_role": task["delivery_role"],
                }
            )
        return record

    def _flush(self) -> None:
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
            self._write_spec(task)
            write_json(
                self.root / ".agents" / "records" / f"{task_id}.json",
                self._record(task, self.states[task_id]),
            )

    def _write_spec(self, task: dict[str, object]) -> None:
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
        path = self.root / ".agents" / "tasks" / f"{task_id}-{task_id.lower()}.md"
        path.write_text(
            f"""---
id: {task_id}
title: {task["title"]}
state: {self.states[task_id]}
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

    def _init_git(self) -> None:
        git(self.root, "init", "--quiet", "-b", "harness")
        git(self.root, "config", "user.email", "test@example.com")
        git(self.root, "config", "user.name", "test")
        git(self.root, "add", ".")
        git(self.root, "commit", "--quiet", "-m", "baseline")

    def _branch_state(self, task_id: str, state: str) -> None:
        """Create a task branch whose record reports ``state``.

        This mirrors an in-flight task: the durable record on ``harness`` is
        unchanged while the live branch record advances.
        """

        branch = f"task/{task_id}"
        git(self.root, "checkout", "--quiet", "-b", branch)
        record = self._record(dict(self.tasks[1]), state)
        record["id"] = task_id
        write_json(
            self.root / ".agents" / "records" / f"{task_id}.json", record
        )
        git(self.root, "add", ".")
        git(self.root, "commit", "--quiet", "-m", f"{task_id} {state}")
        # Restore harness working tree to the durable record.
        git(self.root, "checkout", "--quiet", "harness")

    def _agent_list_states(self) -> dict[str, str]:
        """Reference implementation of agent.sh list runtime resolution."""

        states: dict[str, str] = {}
        for task in self.tasks:
            task_id = str(task["id"])
            durable = json.loads(
                (self.root / ".agents" / "records" / f"{task_id}.json").read_text(
                    encoding="utf-8"
                )
            )
            branch = f"task/{task_id}"
            exists = (
                subprocess.run(
                    ["git", "-C", str(self.root), "show-ref", "--verify",
                     "--quiet", f"refs/heads/{branch}"],
                    check=False,
                ).returncode
                == 0
            )
            record = None
            if exists:
                shown = subprocess.run(
                    ["git", "-C", str(self.root), "show",
                     f"{branch}:.agents/records/{task_id}.json"],
                    capture_output=True, check=False, text=True,
                )
                if shown.returncode == 0:
                    record = json.loads(shown.stdout)
            if durable.get("state") in {"integrated", "done"}:
                record = durable
            elif record is None:
                record = durable
            states[task_id] = record.get("state", "")
        return states

    def test_runtime_state_matches_agent_list_with_active_branch(self) -> None:
        # XT-064 is ready in the durable record but in_progress on its branch.
        self._branch_state("XT-064", "in_progress")
        model = dashboard.build_model(self.root)
        dashboard_states = {t.task_id: t.runtime_state for t in model.tasks}
        self.assertEqual(dashboard_states, self._agent_list_states())
        self.assertEqual(dashboard_states["XT-064"], "in_progress")
        divergent = {t.task_id for t in model.tasks if t.branch_divergent}
        self.assertEqual(divergent, {"XT-064"})

    def test_terminal_durable_state_wins_over_branch(self) -> None:
        # A done durable record must not be overridden by a stale branch record.
        self.states["XT-064"] = "done"
        self._flush()
        self._init_git_recommit()
        self._branch_state("XT-064", "review")
        model = dashboard.build_model(self.root)
        state = {t.task_id: t.runtime_state for t in model.tasks}["XT-064"]
        self.assertEqual(state, "done")

    def _init_git_recommit(self) -> None:
        git(self.root, "add", ".")
        git(self.root, "commit", "--quiet", "-m", "update states")

    def test_requirement_state_matches_delivery_plan(self) -> None:
        model = dashboard.build_model(self.root)
        reqs = {
            view.requirement_id: view.state
            for views in model.requirements.values()
            for view in views
        }
        # XT-063 done, XT-064 ready, XT-065 ready -> planned (no impl done).
        self.assertEqual(reqs["REQ-P1-TRANSFER"], "planned")

        # Advance the implementation and confirm both views agree.
        self.states["XT-064"] = "done"
        self._flush()
        self._init_git_recommit()
        model = dashboard.build_model(self.root)
        reqs = {
            view.requirement_id: view.state
            for views in model.requirements.values()
            for view in views
        }
        self.assertEqual(reqs["REQ-P1-TRANSFER"], "acceptance-ready")

    def test_draft_plan_reserving_unregistered_task_does_not_crash(self) -> None:
        # A draft plan legitimately reserves a future task id (XT-070) whose
        # record does not exist yet. delivery_plan.py status raises for that
        # plan, but the aggregate dashboard must still render every plan.
        draft = {
            "schema_version": 1,
            "id": "DP-FUTURE",
            "title": "Future governance",
            "status": "draft",
            "source": {"kind": "roadmap", "path": "docs/roadmap.md"},
            "requirements": [
                {
                    "id": "REQ-FUTURE",
                    "source_ref": "RM-P1-TRANSFER",
                    "statement": "Reserve future work.",
                    "acceptance_criteria": ["Reserved."],
                    "implementation_tasks": ["XT-070"],
                    "acceptance_task": "XT-071",
                }
            ],
            "approval": {"approved_by": "", "approved_at": "",
                         "content_sha256": ""},
            "superseded_by": "",
        }
        write_json(self.root / ".agents" / "plans" / "DP-FUTURE.json", draft)
        self._init_git_recommit()

        # Negative: the underlying per-plan report fails on the missing record.
        with self.assertRaises(delivery_plan.DeliveryPlanError):
            delivery_plan._record_state(self.root, "XT-070")

        # The aggregate dashboard degrades gracefully instead of raising.
        model = dashboard.build_model(self.root)
        reqs = {
            view.requirement_id: view.state
            for views in model.requirements.values()
            for view in views
        }
        # No implementation is done, so the reserved requirement is planned.
        self.assertEqual(reqs["REQ-FUTURE"], "planned")
        # The reserved id resolves to the reserved sentinel, not a crash.
        self.assertEqual(
            dashboard._plan_task_state(self.root, "XT-070"),
            dashboard.RESERVED_STATE,
        )
        # A registered task still resolves to its real record state.
        self.assertEqual(
            dashboard._plan_task_state(self.root, "XT-063"), "done"
        )

    def test_milestones_parse_from_roadmap(self) -> None:
        model = dashboard.build_model(self.root)
        titles = {group.title for group in model.milestones}
        self.assertIn("P0 foundation", titles)
        self.assertIn("P1 vertical slice", titles)
        p1 = next(g for g in model.milestones if g.title == "P1 vertical slice")
        self.assertEqual(p1.items[0].roadmap_id, "RM-P1-TRANSFER")
        self.assertFalse(p1.items[0].done)
        p0 = next(g for g in model.milestones if g.title == "P0 foundation")
        self.assertTrue(p0.items[0].done)

    def test_gates_parse_from_manifest(self) -> None:
        model = dashboard.build_model(self.root)
        self.assertEqual(model.gates.get("verify"), "make verify")
        self.assertEqual(model.gates.get("dashboard_test"), "make dashboard-test")
        # Workstream keys must not leak into the gate registry.
        self.assertNotIn("integration", model.gates)

    def test_untrusted_title_is_escaped_in_html(self) -> None:
        self.tasks[1]["title"] = "<script>alert(1)</script>"
        self._flush()
        self._init_git_recommit()
        model = dashboard.build_model(self.root)
        document = dashboard.render_html(model, "test")
        self.assertNotIn("<script>alert(1)</script>", document)
        self.assertIn("&lt;script&gt;alert(1)&lt;/script&gt;", document)

    def test_generate_writes_artifact(self) -> None:
        output = self.root / "build" / "dashboard" / "index.html"
        destination = dashboard.generate(self.root, output)
        self.assertTrue(destination.is_file())
        self.assertIn("XnnTransfer delivery dashboard",
                      destination.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
