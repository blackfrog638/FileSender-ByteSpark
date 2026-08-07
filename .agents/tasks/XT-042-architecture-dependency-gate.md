---
id: XT-042
title: Architecture dependency gate
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-041
owned_paths:
  - AGENTS.md
  - .agents/**
  - Makefile
  - tool/harness/**
  - .github/workflows/ci.yml
contract_changes: []
handoff: .agents/handoffs/XT-042.md
---

## Outcome

Reject Flutter layer violations, native private-header coupling, and forbidden
production CMake target dependencies before they can enter review.

## Context

`AGENTS.md` and `docs/architecture.md` define the allowed Flutter and native
dependency direction. The current layout check only restricts `dart:ffi`, so
most boundaries remain reviewer conventions. XT-018 established leaf native
targets and XT-041 requires declared risks to bind to executable gates.

## Constraints

- Parse repository source and CMake files deterministically without installing
  a language server, package manager, or third-party parser.
- Preserve current legal imports and target links while rejecting newly
  introduced reverse or private dependencies.
- Flutter domain and application code must not reach `core/native` or
  presentation; presentation must not reach `core/native`.
- Only `lib/core/native/` may import `dart:ffi`; native adapters must not import
  application, presentation, or app composition code.
- Native production code outside bridge must not include the public C ABI, and
  production code must not include private paths through `src`, parent
  traversal, Flutter, or Dart headers.
- Enforce an explicit allow-list for project production target links without
  constraining test and fuzz targets.
- Keep the scanner fast enough for every `make verify` and CI harness run.

## Risk profile

Resolve every schema version 2 risk dimension in the task record. Every
non-none risk must name commands that also appear in `verification.commands`.

## Acceptance criteria

- [ ] The current repository passes deterministic Flutter import, native
  include, and CMake target dependency checks.
- [ ] Unit fixtures reject every prohibited Flutter layer edge, misplaced
  `dart:ffi`, C ABI use outside bridge, private native include, and forbidden
  project target link.
- [ ] Valid same-layer, downward, composition-root, standard-library, and
  allowed native target dependencies remain accepted.
- [ ] `make architecture-test`, `make verify`, and the CI harness job execute
  the same scanner and tests.
- [ ] `AGENTS.md` identifies the mechanical dependency gate.
- [ ] Repository verification passes.

## Verification

```bash
make architecture-test
make verify
```
