---
id: XT-017
title: Squash task integration
initial_state: ready
workstream: integration
initial_owner: unassigned
depends_on:
  - XT-008
owned_paths:
  - AGENTS.md
  - .agents/**
  - tool/harness/**
  - docs/adr/**
contract_changes:
  - Extend task-record integration provenance with a squash strategy.
handoff: .agents/handoffs/XT-017.md
---

## Outcome

Keep complete task-development history on the task branch while integrating one
auditable squash delivery commit into `harness`, followed by a separate
acceptance commit.

## Context

XT-007 and XT-008 made lifecycle state and cherry-pick provenance durable, but
the one-to-one integration strategy copies every task lifecycle commit into the
integration branch. ADR 0005 defines a compact history model without weakening
source-range, patch-equivalence, verification, or cleanup checks.

## Constraints

- Preserve the reviewed task branch and its complete source commit list until
  the task is accepted and cleaned up.
- Default to one non-merge delivery commit whose tree diff is equivalent to the
  reviewed `base_sha..head_sha` range.
- Record source base, source head, ordered source commits, aggregate source
  patch ID, integrated result SHA, and result patch ID.
- Continue validating existing cherry-pick records without rewriting XT-001
  through XT-016.
- Reject incomplete source ranges, mismatched patch IDs, unavailable result
  commits, and cleanup without complete provenance.
- Keep acceptance as a separate integration-owner commit after `make verify`.

## Acceptance criteria

- [ ] `agent.sh integrate` creates one squash delivery commit for a reviewed
  task with multiple source commits.
- [ ] Squash provenance proves the ordered source range and aggregate patch
  equivalence to the integrated result.
- [ ] Governance validation rejects tampered source head, source commit list,
  source patch ID, result SHA, and result patch ID.
- [ ] Cleanup accepts complete squash provenance and legacy cherry-pick records
  remain valid.
- [ ] ADR 0005 and harness documentation describe the default strategy and
  explicit compatibility boundary.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
