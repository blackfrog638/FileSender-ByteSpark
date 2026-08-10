---
id: XT-036
title: Resume state persistence
initial_state: ready
workstream: native_core
initial_owner: unassigned
depends_on:
  - XT-022
  - XT-034
  - XT-035
  - XT-072
owned_paths:
  - native/include/xnn_transfer/core/storage/**
  - native/src/storage/**
  - native/tests/storage/**
  - docs/adr/0010-resume-state.md
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-RESUME
delivery_role: implementation
contract_changes:
  - Add the versioned persisted transfer-resume schema and migration policy.
handoff: .agents/handoffs/XT-036.md
---

## Outcome

Define and implement the versioned, integrity-protected, peer-bound persistent
state required for bounded transfer recovery.

## Context

Protocol v1 defines RESUME messages and idempotency scope. XT-022 owns protected
identity state; XT-034 and XT-035 own scheduling and destination policy; XT-072
owns multi-file transfer identity. XT-073 separately owns live-session
authorization and recovery state transitions.

## Constraints

- Persist only authenticated peer scope, negotiated version/capabilities,
  commitments, verified offsets, storage identity, and expiry.
- Bind authorization to the peer, transfer, manifest, roles, and live session.
- Reject rollback, corruption, incompatible schema, changed destination,
  mismatched file, expired state, or revoked peer.
- Persist atomically and cap retained records and total metadata bytes.
- Do not implement reconnect dispatch, RESUME wire state, or live authorization.

## Acceptance criteria

- [ ] A reviewed persisted-schema decision defines versioning and migration.
- [ ] Replay, wrong peer, wrong transfer, wrong commitment, stale snapshot,
  truncation, corruption, and expiry fail closed without overwriting data.
- [ ] Atomic store tests reopen valid checkpoints at multiple verified offsets.
- [ ] Cleanup removes expired and terminal state idempotently.
- [ ] `make security-test` passes.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
