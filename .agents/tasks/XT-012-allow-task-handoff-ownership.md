---
id: XT-012
title: Allow task-local governance artifacts
initial_state: ready
workstream: integration
initial_owner: unassigned
depends_on:
  - XT-008
owned_paths:
  - .agents/**
  - tool/harness/**
contract_changes: []
handoff: .agents/handoffs/XT-012.md
---

## Outcome

Allow every task to commit its own durable record and handoff without adding
those governance paths to each product task's owned-path list.

## Context

XT-011 exposed that `prepare-review` permits the task record but rejects the
required `.agents/handoffs/XT-NNN.md`. This contradicts the mandatory handoff
workflow introduced by XT-007.

## Constraints

- The exception applies only to the current task ID's record and handoff.
- It must not permit edits to another task's artifacts or arbitrary `.agents`
  paths.
- Add an isolated lifecycle regression where product owned paths exclude
  `.agents/**`.

## Acceptance criteria

- [x] A task can reach review with its own record and handoff.
- [x] Another task's record or handoff remains outside ownership.
- [x] The isolated governance lifecycle exercises the exception.
- [x] `make verify` passes.

## Verification

```bash
make verify
```
