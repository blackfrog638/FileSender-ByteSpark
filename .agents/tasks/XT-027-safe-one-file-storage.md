---
id: XT-027
title: Safe one file storage
initial_state: ready
workstream: native_core
initial_owner: unassigned
depends_on:
  - XT-011
  - XT-018
owned_paths:
  - native/include/xnn_transfer/core/storage/**
  - native/src/storage/**
  - native/tests/storage/**
contract_changes: []
handoff: .agents/handoffs/XT-027.md
---

## Outcome

Implement a one-file receive transaction that validates an untrusted relative
path, writes a temporary file, verifies integrity, and atomically commits.

## Context

XT-011 defines hostile path and manifest fixtures. XT-018 defines platform
boundaries. This task performs no networking and accepts no file automatically.

## Constraints

- Enforce destination containment without traversal or link following.
- Bound declared size, actual bytes, temporary usage, and free-space checks.
- Keep incomplete data non-visible and clean it idempotently on failure.
- Preserve platform-native Unicode and collision semantics for later policy.
- Replace the canonical `xnn_transfer_storage` placeholder in place; do not add
  a parallel storage provider target.

## Acceptance criteria

- [ ] All XT-011 one-file path and manifest vectors drive native tests.
- [ ] Tests cover symlink/reparse races, short/extra data, hash mismatch,
  low space, permission failure, existing destination, and restart cleanup.
- [ ] Successful commit is integrity-verified and atomic on supported filesystems.
- [ ] `make security-test` passes.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
