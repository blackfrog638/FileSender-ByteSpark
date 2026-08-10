---
id: XT-077
title: Project delivery dashboard
state: ready
workstream: integration
owner: unassigned
depends_on: []
owned_paths:
  - .agents/backlog.yaml
  - .agents/manifest.yaml
  - .agents/plans/DP-PROJECT-DASHBOARD.json
  - .agents/records/XT-077.json
  - .agents/tasks/XT-077-project-delivery-dashboard.md
  - .agents/handoffs/XT-077.md
  - tool/harness/agent.sh
  - tool/harness/dashboard.py
  - tool/harness/dashboard_test.py
  - tool/harness/governance_test.sh
  - Makefile
  - .gitignore
delivery_plan: DP-PROJECT-DASHBOARD
requirement_ids:
  - REQ-PROJECT-DASHBOARD
delivery_role: implementation_acceptance
contract_changes: []
handoff: .agents/handoffs/XT-077.md
---

## Outcome

The integration owner can regenerate a self-contained static HTML dashboard
that shows, in one view, every task's runtime state, delivery-plan requirement
progress, roadmap milestone completion, and verification gate and risk posture.
The dashboard is purely derived: it reads the tracked sources of truth on every
run and never persists, caches, or hand-edits its own copy of any status.

## Context

Task runtime state, requirement progress, and milestones are already tracked in
authoritative sources: `.agents/records/XT-*.json` plus task-branch records for
active state, `.agents/backlog.yaml` for dependencies and owned paths,
`.agents/plans/DP-*.json` for requirement coverage, `docs/roadmap.md` for
milestones, and `.agents/manifest.yaml` for the trusted gate registry. Two text
reports already exist (`agent.sh list` and `delivery_plan.py status`) but there
is no single visual overview. This task adds a read-only aggregator and HTML
renderer that reuses the existing runtime-state and requirement-state logic so
the dashboard can never disagree with those reports.

## Constraints

- The generator is read-only and purely derived. It must not maintain, cache,
  or hand-edit its own copy of task, requirement, or milestone status.
- Resolve task runtime state exactly as `agent.sh list` does, merging durable
  records with task-branch records, so active states (claimed, in_progress,
  review, blocked) are not omitted.
- Resolve requirement state exactly as `delivery_plan.py status` does; reuse the
  existing helpers rather than reimplementing the state machine.
- Emit one self-contained static HTML file to a git-ignored build artifact path.
  Never commit generated HTML.
- HTML-escape every task title and free-form field so untrusted backlog content
  cannot inject markup.
- Expose generation through an `agent.sh dashboard` subcommand that delegates to
  the standalone `dashboard.py`.
- Do not change the C ABI, wire protocol, persisted schema, or any shared
  cross-workstream contract. This is an integration-owned tooling change.

## Architecture change

The record declares `none` mode. No production runtime module moves and no
dependency direction changes. Keep affected modules, superseded
paths/symbols/targets, temporary leases, and lease retirements machine-readable
in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk names trusted gate IDs that also appear in `verification.gates`;
commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] `agent.sh dashboard` (delegating to `tool/harness/dashboard.py`)
      regenerates one self-contained static HTML dashboard from live sources.
- [ ] Every task runtime state in the dashboard equals `agent.sh list` for the
      same commit, including active states resolved from task branches.
- [ ] Every requirement-level state in the dashboard equals
      `delivery_plan.py status`, including accepted, acceptance-ready,
      in-progress, partially-delivered, planned, claimable, and
      dependency-blocked.
- [ ] The generated HTML is written to a git-ignored artifact path and is never
      committed; generation is deterministic for a fixed repository state.
- [ ] All task titles and free-form text are HTML-escaped.
- [ ] `dashboard_test` asserts on fixtures that the aggregation agrees with
      `agent.sh list` and `delivery_plan.py status`, and it runs within
      `make governance-test` and `make verify`.
- [ ] `make dashboard-test`, `make governance-test`, and `make verify` pass.

## Verification

```bash
make dashboard-test
make governance-test
make verify
```
