---
id: XT-050
title: Trusted review gates
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-048
owned_paths:
  - AGENTS.md
  - .agents/manifest.yaml
  - .agents/records/README.md
  - .agents/tasks/TASK_TEMPLATE.md
  - tool/harness/**
contract_changes: []
handoff: .agents/handoffs/XT-050.md
---

## Outcome

Reject task payload changes made after review and reject verification commands
that are not resolved from the integration-owned gate registry.

## Context

XT-041 made risk declarations executable, XT-047 governed commit ranges, and
XT-048 bound architecture declarations to reviewed diffs. A repository audit
found two remaining integrity gaps: a task could append payload commits after
review, and a task could replace specialized verification with an arbitrary
successful shell command. This task hardens those existing contracts without
changing the lifecycle or record schema.

## Constraints

- Preserve schema v1 and v2 records and every accepted lifecycle transition.
- Keep `verification.commands` readable for legacy records, but reject commands
  absent from the integration-owned registry.
- New generated tasks declare gate IDs and execute commands resolved from the
  registry instead of trusting task-authored shell.
- Every task continues to run the repository-level `verify` gate.
- Compare reviewed and current aggregate payloads while excluding only the
  current task record; handoff or production changes after review must fail.
- A correction returns `review -> in_progress` and repeats verification.
- Add positive and negative lifecycle fixtures for both integrity properties.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 2 risk dimension in the task record. Every
non-none risk must name commands that also appear in `verification.commands`.

## Acceptance criteria

- [ ] Unregistered verification commands fail governance validation.
- [ ] New task records bind risks to trusted gate IDs.
- [ ] Legacy registered verification commands remain accepted.
- [ ] A payload commit added after review is rejected before integration.
- [ ] Record-only review lifecycle commits remain integrable.
- [ ] Harness documentation describes the trusted gate boundary.
- [ ] Repository verification passes.

## Verification

```bash
make governance-test
make verify
```
