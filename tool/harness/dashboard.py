#!/usr/bin/env python3
"""Generate a read-only project delivery dashboard.

The dashboard is purely derived. It reads the tracked sources of truth on every
run and never persists, caches, or hand-edits its own copy of any status:

- task runtime state from durable records merged with task-branch records,
  exactly as ``tool/harness/agent.sh list`` resolves it;
- delivery-plan requirement and scheduler state from ``delivery_plan`` so the
  dashboard can never disagree with ``delivery_plan.py status``;
- roadmap milestones from ``docs/roadmap.md``;
- the trusted verification gate registry from ``.agents/manifest.yaml``.

The only output is a self-contained static HTML file written to a git-ignored
build artifact path. Nothing authoritative is written back into the tree.
"""

from __future__ import annotations

import argparse
import html
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

import delivery_plan  # noqa: E402  (local harness module, resolved above)

DEFAULT_OUTPUT = Path("build") / "dashboard" / "index.html"

# Runtime states as they appear in task records and branches.
TERMINAL_STATES = {"integrated", "done"}
ACTIVE_STATES = {"claimed", "in_progress", "review", "blocked"}

# Sentinel state for a task that a delivery plan references but whose record
# does not exist yet. Draft plans legitimately reserve future task IDs before
# their records are created, so the dashboard surfaces that as a visible state
# instead of failing the way ``delivery_plan.py status`` does.
RESERVED_STATE = "reserved"

# Presentation order for the task swimlane columns.
LANE_ORDER = [
    "ready",
    "claimed",
    "in_progress",
    "blocked",
    "review",
    "integrated",
    "done",
]


class DashboardError(RuntimeError):
    """Raised when the tracked sources cannot be aggregated."""


@dataclass
class TaskView:
    task_id: str
    title: str
    readiness: str
    workstream: str
    depends_on: list[str]
    delivery_plan: str
    requirement_ids: list[str]
    delivery_role: str
    # ``runtime_state`` mirrors ``agent.sh list``: durable records win only when
    # terminal, otherwise the task-branch record is authoritative for in-flight
    # work. ``durable_state`` is the durable record used by scheduler views.
    runtime_state: str
    durable_state: str
    owner: str
    task_type: str
    # ``branch_divergent`` marks tasks whose live branch state is not yet
    # reflected in the durable record, so the dashboard can flag the difference
    # instead of silently reconciling two authoritative sources.
    branch_divergent: bool


@dataclass
class RequirementView:
    requirement_id: str
    state: str
    acceptance_task: str
    implementation_tasks: list[str]


@dataclass
class MilestoneItem:
    text: str
    done: bool
    roadmap_id: str = ""


@dataclass
class MilestoneGroup:
    title: str
    items: list[MilestoneItem] = field(default_factory=list)


@dataclass
class DashboardModel:
    plan_titles: dict[str, str]
    tasks: list[TaskView]
    requirements: dict[str, list[RequirementView]]
    milestones: list[MilestoneGroup]
    gates: dict[str, str]


def _git_show(root: Path, revision: str, path: str) -> str | None:
    result = subprocess.run(
        ["git", "-C", str(root), "show", f"{revision}:{path}"],
        capture_output=True,
        check=False,
        text=True,
    )
    if result.returncode != 0:
        return None
    return result.stdout


def _branch_exists(root: Path, branch: str) -> bool:
    return (
        subprocess.run(
            ["git", "-C", str(root), "show-ref", "--verify", "--quiet",
             f"refs/heads/{branch}"],
            check=False,
        ).returncode
        == 0
    )


def _resolve_runtime_state(
    root: Path, task_id: str, durable: dict[str, Any] | None
) -> tuple[str, bool]:
    """Return ``(runtime_state, branch_divergent)`` matching ``agent.sh list``.

    Durable records win when their state is terminal; otherwise the task-branch
    record is authoritative so active work (claimed/in_progress/review/blocked)
    is never omitted. ``branch_divergent`` is true when a live branch record
    reports a different state than the durable record.
    """

    durable_state = durable.get("state", "") if durable else ""
    branch = f"task/{task_id}"
    branch_record: dict[str, Any] | None = None
    if _branch_exists(root, branch):
        raw = _git_show(root, branch, f".agents/records/{task_id}.json")
        if raw:
            try:
                branch_record = json.loads(raw)
            except json.JSONDecodeError as error:
                raise DashboardError(
                    f"{task_id} branch record is not valid JSON: {error}"
                ) from error

    if durable and durable_state in TERMINAL_STATES:
        return durable_state, False
    if branch_record is not None:
        branch_state = branch_record.get("state", "")
        return branch_state, branch_state != durable_state
    return durable_state, False


def _load_durable_record(root: Path, task_id: str) -> dict[str, Any] | None:
    path = root / ".agents" / "records" / f"{task_id}.json"
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise DashboardError(
            f"{task_id} durable record is not valid JSON: {error}"
        ) from error


def _load_tasks(root: Path) -> list[TaskView]:
    backlog_path = root / ".agents" / "backlog.yaml"
    try:
        backlog = json.loads(backlog_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise DashboardError(f"Cannot read backlog: {error}") from error

    views: list[TaskView] = []
    for task in backlog.get("tasks", []):
        task_id = task["id"]
        durable = _load_durable_record(root, task_id)
        runtime_state, divergent = _resolve_runtime_state(root, task_id, durable)
        owner = ""
        task_type = ""
        if durable:
            owner = durable.get("owner", "")
            task_type = durable.get("task_type", "")
        views.append(
            TaskView(
                task_id=task_id,
                title=task.get("title", ""),
                readiness=task.get("readiness", ""),
                workstream=task.get("workstream", ""),
                depends_on=list(task.get("depends_on", [])),
                delivery_plan=task.get("delivery_plan", ""),
                requirement_ids=list(task.get("requirement_ids", [])),
                delivery_role=task.get("delivery_role", ""),
                runtime_state=runtime_state,
                durable_state=durable.get("state", "") if durable else "",
                owner=owner,
                task_type=task_type,
                branch_divergent=divergent,
            )
        )
    return views


def _plan_task_state(root: Path, task_id: str) -> str:
    """Resolve a plan-referenced task's state, tolerating reserved IDs.

    ``delivery_plan._record_state`` raises when a referenced record is missing,
    which is correct for its per-plan status view but would let a single draft
    plan reserving a future task ID crash the whole dashboard. A missing record
    is reported as ``reserved`` so the aggregate view still renders; a record
    that exists but is structurally broken still raises, matching
    ``delivery_plan.py status`` for registered tasks.
    """

    path = root / ".agents" / "records" / f"{task_id}.json"
    if not path.is_file():
        return RESERVED_STATE
    return delivery_plan._record_state(root, task_id)


def _load_requirements(
    root: Path,
) -> tuple[dict[str, str], dict[str, list[RequirementView]]]:
    """Derive requirement views using ``delivery_plan`` helpers verbatim.

    Reusing ``_record_state``/``_scheduled_state``/``_requirement_state`` keeps
    the dashboard identical to ``delivery_plan.py status`` rather than
    reimplementing the state machine.
    """

    errors = delivery_plan.validate_repository(root)
    if errors:
        raise DashboardError(
            "Delivery plans do not validate:\n"
            + "\n".join(f"- {error}" for error in errors)
        )
    config = delivery_plan.load_config(root)
    _backlog, _tasks = delivery_plan.load_backlog(root)
    plans, _paths, load_errors = delivery_plan.load_plans(root, config)
    if load_errors:
        raise DashboardError("\n".join(load_errors))

    plan_titles: dict[str, str] = {}
    requirements: dict[str, list[RequirementView]] = {}
    for plan_id in sorted(plans):
        plan = plans[plan_id]
        plan_titles[plan_id] = plan.get("title", plan_id)
        mapped_ids = {
            tid
            for requirement in plan["requirements"]
            for tid in (
                list(requirement["implementation_tasks"])
                + [requirement["acceptance_task"]]
            )
        }
        states = {
            tid: _plan_task_state(root, tid)
            for tid in sorted(mapped_ids, key=delivery_plan._task_sort_key)
        }
        views: list[RequirementView] = []
        for requirement in plan["requirements"]:
            views.append(
                RequirementView(
                    requirement_id=requirement["id"],
                    state=delivery_plan._requirement_state(requirement, states),
                    acceptance_task=requirement["acceptance_task"],
                    implementation_tasks=list(
                        requirement["implementation_tasks"]
                    ),
                )
            )
        requirements[plan_id] = views
    return plan_titles, requirements


_MILESTONE_HEADING = re.compile(r"^#{1,3}\s+(?P<title>.+?)\s*$")
_CHECKBOX = re.compile(r"^\s*[-*]\s+\[(?P<mark>[ xX])\]\s+(?P<text>.+?)\s*$")
_ROADMAP_ID = re.compile(r"<!--\s*roadmap-id:\s*(?P<id>[A-Z0-9-]+)\s*-->")


def _load_milestones(root: Path) -> list[MilestoneGroup]:
    roadmap_path = root / "docs" / "roadmap.md"
    if not roadmap_path.is_file():
        return []
    groups: list[MilestoneGroup] = []
    current: MilestoneGroup | None = None
    pending_roadmap_id = ""
    for line in roadmap_path.read_text(encoding="utf-8").splitlines():
        marker = _ROADMAP_ID.search(line)
        if marker:
            pending_roadmap_id = marker.group("id")
            continue
        heading = _MILESTONE_HEADING.match(line)
        if heading:
            current = MilestoneGroup(title=heading.group("title"))
            groups.append(current)
            pending_roadmap_id = ""
            continue
        checkbox = _CHECKBOX.match(line)
        if checkbox and current is not None:
            current.items.append(
                MilestoneItem(
                    text=checkbox.group("text"),
                    done=checkbox.group("mark").lower() == "x",
                    roadmap_id=pending_roadmap_id,
                )
            )
            pending_roadmap_id = ""
    return [group for group in groups if group.items]


def _load_gates(root: Path) -> dict[str, str]:
    manifest_path = root / ".agents" / "manifest.yaml"
    gates: dict[str, str] = {}
    if not manifest_path.is_file():
        return gates
    in_commands = False
    for raw in manifest_path.read_text(encoding="utf-8").splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        if not raw.startswith(" "):
            in_commands = raw.rstrip().startswith("commands:")
            continue
        if in_commands and re.match(r"^  \S", raw):
            key, _, value = raw.strip().partition(":")
            if value:
                gates[key.strip()] = value.strip()
    return gates


def build_model(root: Path) -> DashboardModel:
    tasks = _load_tasks(root)
    plan_titles, requirements = _load_requirements(root)
    milestones = _load_milestones(root)
    gates = _load_gates(root)
    return DashboardModel(
        plan_titles=plan_titles,
        tasks=tasks,
        requirements=requirements,
        milestones=milestones,
        gates=gates,
    )


def _esc(value: str) -> str:
    return html.escape(str(value), quote=True)


_STYLE = """
:root { color-scheme: light dark; }
* { box-sizing: border-box; }
body { font-family: -apple-system, "Segoe UI", Roboto, sans-serif;
  margin: 0; padding: 24px; background: #0f1117; color: #e6e8ee; }
h1 { margin: 0 0 4px; font-size: 22px; }
.subtitle { color: #9aa4b2; margin: 0 0 24px; font-size: 13px; }
h2 { font-size: 16px; margin: 28px 0 12px; border-bottom: 1px solid #2a2f3a;
  padding-bottom: 6px; }
.lanes { display: flex; gap: 12px; overflow-x: auto; padding-bottom: 8px; }
.lane { background: #161a23; border: 1px solid #2a2f3a; border-radius: 8px;
  min-width: 190px; flex: 1; padding: 10px; }
.lane h3 { margin: 0 0 8px; font-size: 12px; text-transform: uppercase;
  letter-spacing: .04em; color: #9aa4b2; display: flex;
  justify-content: space-between; }
.card { background: #1e2430; border: 1px solid #2f3644; border-radius: 6px;
  padding: 8px; margin-bottom: 8px; font-size: 12px; }
.card .id { font-weight: 600; }
.card .title { color: #c4ccd8; margin: 2px 0; }
.card .meta { color: #7f8a99; font-size: 11px; }
.badge { display: inline-block; padding: 1px 6px; border-radius: 10px;
  font-size: 10px; margin-right: 4px; }
.badge.diverge { background: #7a3b00; color: #ffd8a8; }
.badge.role { background: #22303f; color: #8fc3ff; }
table { border-collapse: collapse; width: 100%; font-size: 13px; }
th, td { text-align: left; padding: 6px 10px; border-bottom: 1px solid #232833; }
th { color: #9aa4b2; font-weight: 600; }
.state { font-weight: 600; }
.state-done, .state-accepted { color: #6ee7a8; }
.state-acceptance-ready { color: #9ae6b4; }
.state-in-progress, .state-integrated { color: #f6c453; }
.state-partially-delivered { color: #f6c453; }
.state-claimable, .state-ready { color: #8fc3ff; }
.state-dependency-blocked, .state-blocked, .state-planned { color: #f28b82; }
.state-reserved { color: #9aa4b2; font-style: italic; }
.mile { display: flex; align-items: baseline; gap: 8px; padding: 3px 0; }
.mile .mark { width: 16px; }
.mile.done .text { color: #6ee7a8; }
.mile .rid { color: #7f8a99; font-size: 11px; }
.progress { height: 6px; background: #232833; border-radius: 3px;
  overflow: hidden; margin: 4px 0 10px; }
.progress > span { display: block; height: 100%; background: #6ee7a8; }
.gates { columns: 2; font-size: 12px; }
.gates div { break-inside: avoid; padding: 2px 0; }
.gates code { color: #8fc3ff; }
footer { margin-top: 32px; color: #6b7480; font-size: 11px; }
""".strip()


def _state_class(state: str) -> str:
    return "state-" + state.replace("_", "-") if state else ""


def _render_task_card(task: TaskView) -> str:
    badges = ""
    if task.branch_divergent:
        badges += (
            '<span class="badge diverge" title="Live task-branch state differs '
            'from the durable record">branch: '
            f"{_esc(task.runtime_state)}</span>"
        )
    if task.delivery_role:
        badges += f'<span class="badge role">{_esc(task.delivery_role)}</span>'
    reqs = ", ".join(_esc(r) for r in task.requirement_ids)
    deps = ", ".join(_esc(d) for d in task.depends_on) or "—"
    owner = _esc(task.owner) if task.owner else "unassigned"
    return (
        '<div class="card">'
        f'<div class="id">{_esc(task.task_id)} '
        f'<span class="meta">{_esc(task.workstream)}</span></div>'
        f'<div class="title">{_esc(task.title)}</div>'
        f"<div>{badges}</div>"
        f'<div class="meta">owner: {owner}</div>'
        f'<div class="meta">req: {reqs or "—"}</div>'
        f'<div class="meta">deps: {deps}</div>'
        "</div>"
    )


def _render_lanes(tasks: list[TaskView]) -> str:
    buckets: dict[str, list[TaskView]] = {state: [] for state in LANE_ORDER}
    extras: dict[str, list[TaskView]] = {}
    for task in tasks:
        state = task.runtime_state or "ready"
        if state in buckets:
            buckets[state].append(task)
        else:
            extras.setdefault(state, []).append(task)
    lanes_html = ""
    for state in LANE_ORDER:
        cards = "".join(_render_task_card(t) for t in buckets[state])
        lanes_html += (
            '<div class="lane">'
            f'<h3>{_esc(state)}<span>{len(buckets[state])}</span></h3>'
            f"{cards}</div>"
        )
    for state, tasks_in_state in sorted(extras.items()):
        cards = "".join(_render_task_card(t) for t in tasks_in_state)
        lanes_html += (
            '<div class="lane">'
            f'<h3>{_esc(state)}<span>{len(tasks_in_state)}</span></h3>'
            f"{cards}</div>"
        )
    return f'<div class="lanes">{lanes_html}</div>'


def _render_requirements(model: DashboardModel) -> str:
    sections = ""
    for plan_id in sorted(model.requirements):
        views = model.requirements[plan_id]
        rows = ""
        accepted = sum(v.state == "accepted" for v in views)
        for view in views:
            impls = ", ".join(_esc(t) for t in view.implementation_tasks)
            rows += (
                "<tr>"
                f"<td>{_esc(view.requirement_id)}</td>"
                f'<td class="state {_state_class(view.state)}">'
                f"{_esc(view.state)}</td>"
                f"<td>{_esc(view.acceptance_task)}</td>"
                f"<td>{impls}</td>"
                "</tr>"
            )
        pct = int(round(100 * accepted / len(views))) if views else 0
        sections += (
            f"<h3>{_esc(model.plan_titles.get(plan_id, plan_id))} "
            f'<span class="meta">({_esc(plan_id)})</span></h3>'
            f'<div class="meta">accepted {accepted}/{len(views)}</div>'
            f'<div class="progress"><span style="width:{pct}%"></span></div>'
            "<table><thead><tr><th>Requirement</th><th>State</th>"
            "<th>Acceptance</th><th>Implementation</th></tr></thead>"
            f"<tbody>{rows}</tbody></table>"
        )
    return sections


def _render_milestones(groups: list[MilestoneGroup]) -> str:
    out = ""
    for group in groups:
        done = sum(1 for item in group.items if item.done)
        total = len(group.items)
        pct = int(round(100 * done / total)) if total else 0
        items_html = ""
        for item in group.items:
            cls = "mile done" if item.done else "mile"
            mark = "✓" if item.done else "○"
            rid = (
                f'<span class="rid">{_esc(item.roadmap_id)}</span>'
                if item.roadmap_id
                else ""
            )
            items_html += (
                f'<div class="{cls}"><span class="mark">{mark}</span>'
                f'<span class="text">{_esc(item.text)}</span>{rid}</div>'
            )
        out += (
            f"<h3>{_esc(group.title)} "
            f'<span class="meta">{done}/{total}</span></h3>'
            f'<div class="progress"><span style="width:{pct}%"></span></div>'
            f"{items_html}"
        )
    return out


def _render_gates(gates: dict[str, str]) -> str:
    rows = "".join(
        f"<div><code>{_esc(key)}</code> → {_esc(value)}</div>"
        for key, value in sorted(gates.items())
    )
    return f'<div class="gates">{rows}</div>'


def render_html(model: DashboardModel, generated_from: str) -> str:
    total = len(model.tasks)
    done = sum(1 for t in model.tasks if t.runtime_state == "done")
    active = sum(1 for t in model.tasks if t.runtime_state in ACTIVE_STATES)
    diverging = sum(1 for t in model.tasks if t.branch_divergent)
    subtitle = (
        f"{total} tasks · {done} done · {active} active · "
        f"{diverging} with live branch state ahead of the durable record · "
        f"generated from {_esc(generated_from)}"
    )
    return (
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        '<meta name="viewport" content="width=device-width, initial-scale=1">'
        "<title>XnnTransfer delivery dashboard</title>"
        f"<style>{_STYLE}</style></head><body>"
        "<h1>XnnTransfer delivery dashboard</h1>"
        f'<p class="subtitle">{subtitle}</p>'
        "<h2>Task runtime state</h2>"
        '<p class="subtitle">Runtime state matches <code>agent.sh list</code>: '
        "durable records win only when terminal, otherwise the live task-branch "
        "record is authoritative. A <span class=\"badge diverge\">branch</span> "
        "badge flags tasks whose live state is ahead of the durable record.</p>"
        f"{_render_lanes(model.tasks)}"
        "<h2>Delivery-plan requirements</h2>"
        '<p class="subtitle">Requirement and scheduler state are derived by '
        "<code>delivery_plan</code>, identical to "
        "<code>delivery_plan.py status</code>.</p>"
        f"{_render_requirements(model)}"
        "<h2>Roadmap milestones</h2>"
        f"{_render_milestones(model.milestones)}"
        "<h2>Verification gate registry</h2>"
        f"{_render_gates(model.gates)}"
        "<footer>Read-only, purely derived from the tracked sources of truth "
        "(backlog, task records, task branches, delivery plans, roadmap, "
        "manifest). This artifact is generated and must not be committed."
        "</footer>"
        "</body></html>"
    )


def generate(root: Path, output: Path) -> Path:
    model = build_model(root)
    head = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "--short", "HEAD"],
        capture_output=True,
        check=False,
        text=True,
    ).stdout.strip()
    document = render_html(model, head or "working tree")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(document, encoding="utf-8")
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="Repository root to read the tracked sources from.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Destination HTML path (default: build/dashboard/index.html).",
    )
    args = parser.parse_args()
    root = args.root.resolve()
    output = args.output or (root / DEFAULT_OUTPUT)
    try:
        destination = generate(root, output.resolve())
    except (DashboardError, delivery_plan.DeliveryPlanError) as error:
        print(f"Dashboard error:\n{error}", file=sys.stderr)
        return 1
    print(f"Wrote {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
