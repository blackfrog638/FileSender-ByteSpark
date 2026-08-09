---
id: XT-065
title: Restore storage formatting
state: ready
workstream: native_core
owner: unassigned
depends_on:
  - XT-027
owned_paths:
  - native/include/xnn_transfer/core/storage/storage.hpp
  - native/src/storage/filesystem_backend.cpp
  - native/src/storage/path_validator.cpp
  - native/src/storage/receive_transaction.cpp
delivery_plan: DP-STORAGE-FORMAT-REPAIR
requirement_ids:
  - REQ-STORAGE-FORMAT-REPAIR
delivery_role: implementation_acceptance
contract_changes: []
handoff: .agents/handoffs/XT-065.md
---

## Outcome

Restore the four XT-027 storage sources changed by `c412222` to the
repository's checked-in `.clang-format` without changing C++ tokens or runtime
behavior.

## Context

XT-027 delivered the accepted safe one-file storage implementation. The later
local style commit `c412222` reflowed four sources using a non-project style and
made the repository-level format gate fail. This task restores the existing
format contract so XT-063 and later tasks can pass `make verify`.

## Constraints

- Change only whitespace and line wrapping in the four owned storage files.
- Preserve the accepted XT-027 API, control flow, constants, comments, and
  platform behavior byte-for-byte after whitespace is ignored.
- Use the repository `.clang-format` with the Xcode clang-format selected by
  the verification gate; do not introduce another style configuration.
- Do not change storage tests, build files, public contracts, or roadmap state.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] The four owned sources pass clang-format dry-run with the repository
      configuration.
- [ ] A whitespace-ignoring diff from `c412222` contains no changes.
- [ ] Existing native storage and repository tests continue to pass.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
