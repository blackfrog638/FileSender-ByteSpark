---
id: XT-048
title: Subtractive architecture
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-020
  - XT-042
  - XT-047
owned_paths:
  - AGENTS.md
  - .agents/**
  - tool/harness/**
  - docs/architecture.md
contract_changes: []
handoff: .agents/handoffs/XT-048.md
---

## Outcome

Reject parallel runtime providers, incomplete placeholder replacements, stale
temporary-code leases, and architecture-changing tasks without an explicit
add/replace/remove/refactor declaration.

## Context

XT-042 enforces dependency direction but does not prove that one capability has
one canonical implementation. The current identity, TLS, session, storage, and
transfer targets are deliberate `INTERFACE` placeholders for later tasks.
XT-048 makes those replacement obligations and temporary-code lifetimes
machine-readable without changing runtime behavior.

## Constraints

- Keep the existing target names and dependency matrix as the stable module
  boundaries; do not modify product CMake files in this task.
- Inventory every production native target exactly once and reject undeclared
  `xnn_transfer_*` providers under `native/` and `native/src/`.
- A placeholder target may remain `INTERFACE` only while its declared
  replacement task is still `ready`; once that task starts, the target must be
  concrete in the task worktree.
- New governed tasks must declare architecture change mode, affected modules,
  and concrete paths, symbols, or targets they supersede.
- Temporary production code must use a registered lease marker with an owner,
  rationale, and removal task. A completed removal task makes the lease fail.
- Preserve compatibility for archived records and existing tasks that do not
  own a registered placeholder replacement.
- Use deterministic Python and JSON only; add no new toolchain dependency.

## Risk profile

Resolve every schema version 2 risk dimension in the task record. Every
non-none risk must name commands that also appear in `verification.commands`.

## Acceptance criteria

- [x] A versioned module inventory defines canonical target, CMake definition,
  implementation roots, allowed project dependencies, and replacement task.
- [x] The architecture gate rejects duplicate or undeclared providers,
  premature placeholder retention, and concrete targets outside their declared
  definition path.
- [x] Temporary lease validation rejects missing markers, unknown tasks,
  duplicate lease IDs, and leases whose removal task is complete.
- [x] Task governance validates explicit architecture change declarations and
  absence claims for superseded paths and symbols before review.
- [x] XT-022, XT-023, XT-025, XT-027, and XT-028 are bound to their existing
  placeholder replacement obligations without changing their owned paths.
- [x] New task generation requires an explicit architecture change mode.
- [x] Architecture documentation and agent instructions describe the
  single-provider, replacement, and lease rules.
- [x] `make architecture-test` passes.
- [x] `make governance-test` passes.
- [x] Repository verification passes.

## Verification

```bash
make architecture-test
make governance-test
make verify
```
