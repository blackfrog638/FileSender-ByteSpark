---
id: XT-036
title: Resume persistence
initial_state: ready
workstream: native_core
initial_owner: unassigned
depends_on:
  - XT-022
  - XT-034
  - XT-035
owned_paths:
  - native/include/xnn_transfer/core/session/**
  - native/src/session/**
  - native/tests/session/**
  - native/include/xnn_transfer/core/storage/**
  - native/src/storage/**
  - native/tests/storage/**
  - native/include/xnn_transfer/core/transfer/**
  - native/src/transfer/**
  - native/tests/transfer/**
  - docs/adr/0010-resume-state.md
contract_changes:
  - Add the versioned persisted transfer-resume schema and migration policy.
handoff: .agents/handoffs/XT-036.md
---

## Outcome

Resume authenticated transfers after reconnect or process restart from
versioned, integrity-protected, peer-bound state with bounded retention.

## Context

Protocol v1 defines RESUME messages and idempotency scope. XT-022 owns protected
identity state; XT-034 and XT-035 own scheduling and destination policy.

## Constraints

- Persist only authenticated peer scope, negotiated version/capabilities,
  commitments, verified offsets, storage identity, and expiry.
- Bind authorization to the peer, transfer, manifest, roles, and live session.
- Reject rollback, corruption, incompatible schema, changed destination,
  mismatched file, expired state, or revoked peer.
- Persist atomically and cap retained records and total metadata bytes.

## Acceptance criteria

- [ ] A reviewed persisted-schema decision defines versioning and migration.
- [ ] Tests resume at multiple offsets after disconnect and process restart.
- [ ] Replay, wrong peer, wrong transfer, wrong commitment, stale snapshot,
  truncation, corruption, and expiry fail closed without overwriting data.
- [ ] Cleanup removes expired and terminal state idempotently.
- [ ] `make security-test` passes.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
