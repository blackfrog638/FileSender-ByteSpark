---
id: XT-045
title: Windows abi dll search path
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-044
owned_paths:
  - .agents/**
  - native/tests/abi/**
contract_changes: []
handoff: .agents/handoffs/XT-045.md
---

## Outcome

Run the ABI v1 legacy client and binary export probe on Windows with an
explicit, bounded DLL dependency search path.

## Context

XT-043 added the ABI v1 gate and XT-044 bounded every runtime probe. CI run
31159543576 proved the compile fixtures, current layout, and frozen layout on
Windows, then isolated two loader failures: the legacy client timed out and
Python could not resolve `libxnn_transfer_core.dll` or its dependencies.

## Constraints

- Do not copy runtime DLLs, change production linkage, or weaken export checks.
- Prepend the built core directory only for tests that execute or load it.
- On Windows, register existing PATH directories with Python's secure DLL
  loader while keeping those directory handles alive through symbol checks.
- Preserve non-Windows loader behavior and all 30-second test timeouts.

## Risk profile

Resolve every schema version 2 risk dimension in the task record. Every
non-none risk must name commands that also appear in `verification.commands`.

## Acceptance criteria

- [ ] CTest prepends the actual core target directory for the legacy client and
  export probe on Windows.
- [ ] Python explicitly registers valid Windows DLL search directories before
  calling `ctypes.CDLL`.
- [ ] Linux and macOS behavior is unchanged.
- [ ] `make abi-compat-test` and all three native CI platforms pass.
- [ ] No production contract changes.
- [ ] Repository verification passes.

## Verification

```bash
make abi-compat-test
make verify
```
