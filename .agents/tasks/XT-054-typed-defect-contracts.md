---
id: XT-054
title: Typed defect contracts
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-050
owned_paths:
  - AGENTS.md
  - .agents/records/README.md
  - .agents/tasks/TASK_TEMPLATE.md
  - docs/adr/0011-defect-task-governance.md
  - tool/harness/governance.py
  - tool/harness/governance_test.sh
  - tool/harness/new_task.sh
contract_changes:
  - Schema version 3 task records use explicit task types
  - Bugfix records declare a defect and contract disposition
  - Investigation records declare a bounded question and reviewed disposition
handoff: .agents/handoffs/XT-054.md
---

## Outcome

Reject new bugfix and investigation tasks that do not carry complete,
machine-readable evidence and contract-disposition metadata.

## Context

XT-050 made verification commands and reviewed payloads trustworthy, but schema
version 2 still models every task as generic implementation work. ADR 0011
defines schema version 3 task classification and separates defect restoration
from feature delivery. XT-055 will execute the reproduction proof introduced
by this contract; this task only defines and validates the durable schema.

## Constraints

- Preserve schema version 1 and 2 records without migration.
- Support `feature`, `bugfix`, `refactor`, `investigation`, `test`, and
  `governance` task types.
- Require bugfix severity, source, symptom, expected and actual behavior,
  trigger, affected-since value, proof mode, regression gate, reproduction
  commit placeholder, and contract disposition.
- Accept `restore`, `preserve`, or `change` as contract dispositions; `change`
  requires an ADR.
- Require investigations to define a question, scope, required evidence, exit
  criteria, and a reviewed outcome disposition.
- Keep shell commands out of defect metadata; regression evidence references a
  trusted gate ID from `.agents/manifest.yaml`.
- Generate schema version 3 records without making existing task creation
  invocations invalid.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 2 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] Schema version 1 and 2 repository records still validate unchanged.
- [ ] New tasks default to schema version 3 `feature`.
- [ ] `new_task.sh --task-type bugfix` emits an explicit unresolved defect
      contract that cannot be claimed until completed.
- [ ] Invalid task types, defect fields, proof modes, regression gates, and
      contract dispositions are rejected.
- [ ] Contract-changing bugfixes without an ADR are rejected.
- [ ] Investigation records cannot finish review with a pending disposition.
- [ ] ADR 0011 and repository task documentation define the trade-offs.
- [ ] Repository verification passes.

## Verification

```bash
make governance-test
make verify
```
