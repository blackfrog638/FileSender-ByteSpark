---
id: XT-008
title: Make governance validation remote-safe
initial_state: ready
workstream: integration
initial_owner: unassigned
depends_on:
  - XT-007
owned_paths:
  - .agents/**
  - tool/harness/**
contract_changes: []
---

## Outcome

Make governance validation pass in a fresh remote clone without weakening
result-commit and patch-provenance checks.

## Context

The first remote XT-007 CI run showed that historical task source commits are
not reachable because legacy task branches were never pushed. Their integrated
result commits and stable patch IDs are durable on `harness`.

The same run exposed macOS Bash 3.2 empty-array handling in `new_task.sh`.

## Constraints

- Require active review source commits to exist locally.
- Permit unavailable historical source commits only after integration.
- Continue validating result commits, patch IDs, and verified ancestry.
- Keep task creation compatible with Bash 3.2 and empty dependency lists.

## Acceptance criteria

- [x] Fresh-clone governance validation passes without legacy task branches.
- [x] Tampered result patch IDs still fail validation.
- [x] `new_task.sh` supports a task with no dependencies on macOS Bash 3.2.
- [x] Repository verification passes.

## Verification

```bash
make verify
```
