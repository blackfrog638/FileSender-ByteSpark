---
id: XT-035
title: Destination collision policy
initial_state: ready
workstream: native_core
initial_owner: unassigned
depends_on:
  - XT-033
owned_paths:
  - native/include/xnn_transfer/core/storage/**
  - native/src/storage/**
  - native/tests/storage/**
contract_changes: []
handoff: .agents/handoffs/XT-035.md
---

## Outcome

Implement explicit destination selection and collision policy without replacing,
merging, or renaming receiver files implicitly.

## Context

XT-033 provides multi-file storage transactions. Protocol v1 requires local
storage policy to remain receiver-controlled and outside peer authority.

## Constraints

- Canonicalize destination and every final path at the native boundary.
- Model reject, choose-another-root, and reviewed rename behavior explicitly.
- Recheck collisions at commit to close time-of-check/time-of-use races.
- Never follow links or overwrite existing objects by default.

## Acceptance criteria

- [ ] Tests cover files/directories, normalized-name collisions, case behavior,
  reserved names, links, races, permissions, and cross-device destinations.
- [ ] Policy decisions are opaque IDs; peers cannot inject local paths.
- [ ] Failed policy or commit leaves existing destinations untouched.
- [ ] `make security-test` passes.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
