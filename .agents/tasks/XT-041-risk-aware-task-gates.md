---
id: XT-041
title: Risk aware task gates
state: ready
workstream: integration
owner: unassigned
depends_on: []
owned_paths:
  - AGENTS.md
  - .agents/**
  - tool/harness/**
contract_changes: []
handoff: .agents/handoffs/XT-041.md
---

## Outcome

Require every newly planned task and every unclaimed P1 task to declare
machine-validated functionality, security, performance, compatibility,
concurrency, platform, and persistence risks, with each non-none risk bound to
commands that the harness actually executes.

## Context

`AGENTS.md` requires focused verification, negative tests, and explicit
contract impact, but schema version 1 records do not connect a task's stated
risks to its executable verification commands. Several P1 task specifications
therefore mention sanitizer, fuzz, interoperability, or benchmark evidence
while their records execute only `make verify`.

This task establishes the forward governance contract before the remaining P1
tasks are claimed. It does not change product runtime behavior.

## Constraints

- Preserve validation of archived schema version 1 records and the active
  XT-019 task without rewriting another owner's branch.
- Schema version 2 must enumerate all seven risk dimensions with a level,
  concrete rationale, and executable gate bindings.
- Every gate named by a risk must occur in the record's verification command
  list; a non-none risk cannot have no gate.
- New task generation must fail governance validation until every generated
  risk placeholder is resolved.
- Migrate ready, unclaimed P1 records to schema version 2 without changing
  their product contracts or lifecycle state.
- Do not claim that a generic repository command proves cross-platform,
  security, or performance behavior that it does not exercise.

## Acceptance criteria

- [ ] Governance accepts valid schema version 1 archives and schema version 2
  risk profiles.
- [ ] Governance rejects missing dimensions, invalid levels, placeholder
  rationales, empty mitigation gates, duplicate gates, and gates absent from
  verification.
- [ ] `new_task.sh` and the task template emit the same schema version 2 shape.
- [ ] Every ready, unassigned P1 record uses schema version 2 with concrete
  risk dispositions and executable verification commands.
- [ ] `AGENTS.md` documents the risk-to-evidence rule.
- [ ] Governance lifecycle tests cover positive and tampered records.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
