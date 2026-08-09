---
id: XT-063
title: Delivery plan governance
state: ready
workstream: integration
owner: unassigned
depends_on: []
owned_paths:
  - AGENTS.md
  - .agents/plans/**
  - .agents/manifest.yaml
  - .agents/README.md
  - .agents/records/README.md
  - .agents/tasks/TASK_TEMPLATE.md
  - docs/adr/0014-delivery-plan-governance.md
  - tool/harness/delivery_plan.py
  - tool/harness/delivery_plan_test.py
  - tool/harness/governance.py
  - tool/harness/governance_test.sh
  - tool/harness/new_task.sh
  - Makefile
contract_changes:
  - Delivery Plan schema version 1
  - plan-bound task catalogue metadata
  - task claim eligibility
handoff: .agents/handoffs/XT-063.md
---

## Outcome

Introduce a reviewed Delivery Plan layer between roadmap milestones and XT
execution. Approved plans bind stable requirement IDs to implementation and
acceptance tasks, and the harness rejects incomplete coverage, invalid task
graphs, divergent plan-bound task metadata, or claims from unapproved plans.

## Context

The roadmap records product milestones while the backlog and task specs record
execution units. The repository currently has no machine-readable artifact
that proves which tasks cover a roadmap requirement or that an acceptance task
transitively depends on every implementing task. ADR 0014 defines the new
planning boundary. XT-064 will migrate the current P1 decomposition after this
governance layer is accepted.

## Constraints

- Keep task dependencies and owned paths authoritative in the backlog; plans
  reference task IDs and roles instead of duplicating the execution DAG.
- Keep lifecycle state authoritative in task records and active task branches;
  plans must not become another runtime scheduler.
- Preserve legacy XT-001 through XT-063 without synthetic plan bindings.
  Configure XT-064 as the first task that requires an approved plan.
- A draft plan may reserve future task IDs, but only registered tasks in an
  approved plan may be claimed.
- Every approved requirement has nonempty acceptance criteria, at least one
  implementation task, and exactly one acceptance task.
- The acceptance task must transitively depend on all implementation tasks for
  the requirement, and the complete backlog dependency graph must be acyclic.
- Every plan-bound task has a bidirectional requirement mapping and matching
  `id`, title, workstream, dependencies, and owned paths in its backlog entry
  and task-spec front matter.
- Use the Python standard library only, preserve macOS Bash 3.2 compatibility,
  and produce deterministic diagnostics on macOS, Linux, and Windows.
- Plan approval is explicit, attributed, and validated; generated status is
  derived from plans and task runtime rather than stored in a plan.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] Schema version 1 and ADR 0014 define roadmap references, requirements,
      task coverage, approval, compatibility, and source-of-truth boundaries.
- [ ] The CLI can initialize and validate plans and can approve only a complete
      plan using an attributed integration-owner decision.
- [ ] Validation rejects malformed IDs, duplicate requirements, unknown task
      references, orphan plan-bound tasks, one-sided mappings, dependency
      cycles, incomplete acceptance closure, metadata divergence, and claims
      from draft plans.
- [ ] `new_task.sh` records required plan, requirement, and role metadata for
      tasks at or after the configured enforcement threshold.
- [ ] Existing XT-001 through XT-063 remain valid without fabricated plan
      metadata.
- [ ] Focused Delivery Plan tests and repository governance tests pass.
- [ ] Repository verification passes.

## Verification

```bash
make governance-test
make verify
```
