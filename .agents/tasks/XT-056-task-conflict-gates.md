---
id: XT-056
title: Task conflict gates
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-055
owned_paths:
  - AGENTS.md
  - .agents/records/README.md
  - .agents/tasks/TASK_TEMPLATE.md
  - docs/adr/0012-task-conflict-governance.md
  - tool/harness/agent.sh
  - tool/harness/governance_test.sh
  - tool/harness/task_conflicts.py
  - tool/harness/task_conflicts_test.py
contract_changes:
  - Claim rejects overlap with active task ownership
  - Review and integration reject path-relevant stale bases
  - Claim conflict checks execute under one recoverable scheduler lock
handoff: .agents/handoffs/XT-056.md
---

## Outcome

Prevent two active tasks from owning intersecting paths and prevent a stale
task from entering review or integration after relevant upstream changes.

## Context

XT-054 and XT-055 added typed defect work and dual-revision evidence. The
identity and proof deliveries also exposed scheduler races: a task can be
claimed while another active branch owns the same path, and an old worktree can
review against governance or product files changed after its base. ADR 0012
defines the coordination contract without introducing a second scheduler.

## Constraints

- Compare explicit backlog ownership, not observed diffs, at claim time.
- Treat exact paths, globs, and recursive directory patterns
  conservatively; uncertain pattern intersections must block.
- Resolve active state from task branches while work is in flight and from the
  durable integration record after integration.
- Serialize conflict-check plus branch creation with a recoverable lock so two
  concurrent claims cannot both pass.
- Reject a diverged base unconditionally.
- For an ancestor base, block only when upstream changed a task-owned path or a
  global governance path. Unrelated product paths remain parallel.
- Recheck staleness before review and immediately before integration.
- Report the conflicting task IDs, ownership patterns, and changed paths.
- Preserve schema version 1/2 tasks and non-overlapping current workflows.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] Claim rejects exact, directory, glob, and conservative potential overlap
      with every active task state.
- [ ] Done and ready tasks do not block a claim.
- [ ] Concurrent claim decisions are protected by a recoverable lock.
- [ ] Review and integration reject diverged bases.
- [ ] Relevant upstream product or governance changes reject stale tasks.
- [ ] Unrelated upstream product changes do not serialize task delivery.
- [ ] Rename/delete paths are checked without rename collapsing.
- [ ] ADR 0012 and task documentation define the coordination contract.
- [ ] Repository verification passes.

## Verification

```bash
make governance-test
make verify
```
