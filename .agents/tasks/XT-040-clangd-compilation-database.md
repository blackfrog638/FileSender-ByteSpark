---
id: XT-040
title: Clangd compilation database
state: ready
workstream: integration
owner: unassigned
depends_on: []
owned_paths:
  - .clangd
contract_changes: []
handoff: .agents/handoffs/XT-040.md
---

## Outcome

Make clangd automatically use the CMake dev compilation database for native
C++ files without editor-specific command-line flags.

## Context

The root CMake project already exports compile commands to
`out/build/dev/compile_commands.json`, but that build directory is not an
ancestor of files under `native/`. Clangd therefore falls back to a generic
command, misses project include paths and C++20 flags, and reports false
diagnostics. `make bootstrap` already configures the dev preset.

## Constraints

- Keep build output ignored and machine-local; do not commit a generated
  compilation database or absolute paths.
- Reuse the existing cross-platform dev preset instead of adding
  editor-specific settings.
- Do not suppress real diagnostics or add fallback include paths that can drift
  from CMake targets.

## Acceptance criteria

- [ ] Clangd loads `out/build/dev/compile_commands.json` without an explicit
  `--compile-commands-dir` argument.
- [ ] `native/src/core/engine.cpp` passes clangd check mode with zero errors.
- [ ] No generated build artifact or machine-specific path is tracked.
- [ ] Repository verification passes.

## Verification

```bash
cmake --preset dev -S .
clangd --check=native/src/core/engine.cpp
make verify
```
