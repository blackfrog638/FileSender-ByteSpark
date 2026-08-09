---
id: XT-064
title: P1 delivery plan migration
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-063
owned_paths:
  - .agents/backlog.yaml
  - .agents/plans/DP-P1-PLAN-MIGRATION.json
  - .agents/plans/DP-P1-DELIVERY.json
  - .agents/tasks/XT-*.md
  - docs/roadmap.md
  - tool/harness/delivery_plan.py
  - tool/harness/delivery_plan_test.py
  - Makefile
delivery_plan: DP-P1-PLAN-MIGRATION
requirement_ids:
  - REQ-P1-PLAN-MIGRATION
delivery_role: implementation_acceptance
contract_changes: []
handoff: .agents/handoffs/XT-064.md
---

## Outcome

Represent every current P1 roadmap requirement and its XT implementation and
acceptance coverage in an approved Delivery Plan, reconcile the existing task
DAG with reviewed specifications, and replace the stale execution snapshot
with a mechanically checked view.

## Context

ADR 0014 and XT-063 define Delivery Plan schema, approval, inverse task
bindings, acceptance closure, and claim gating. `docs/roadmap.md` defines the
P1 product outcomes, while `.agents/backlog.yaml` remains the only task
dependency graph. Existing accepted task records remain execution provenance,
not migration targets.

## Constraints

- Add stable source markers for all ten P1 roadmap requirements.
- Map the complete P1 task set without copying dependencies into the plan.
- Keep XT-032 and XT-039 as the unique vertical-slice and production
  acceptance owners.
- Reconcile backlog dependencies with reviewed task specifications, including
  XT-025 and XT-032 prerequisites.
- Add inverse Delivery Plan metadata to the backlog and task specifications
  without rewriting accepted task-record provenance.
- Generate the roadmap execution view deterministically from the approved plan,
  backlog, and task records, and fail validation when the checked-in view
  drifts.
- Do not change runtime code, public interfaces, wire behavior, persisted
  formats, security profiles, or platform support.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] Every P1 roadmap item has one stable marker and reviewed requirement
  mapping.
- [ ] XT-032 and XT-039 transitively cover all implementation tasks for the
  requirements they accept.
- [ ] Plan, backlog, task-spec metadata, and dependency edges validate without
  drift.
- [ ] The checked-in P1 execution view is reproducible from approved planning
  and durable runtime state.
- [ ] Existing accepted task records retain their lifecycle and integration
  provenance.
- [ ] Repository verification passes.

## Verification

```bash
make delivery-plan-test
make governance-test
make verify
```
