---
id: XT-029
title: Cancellation cleanup
initial_state: ready
workstream: native_core
initial_owner: unassigned
depends_on:
  - XT-028
owned_paths:
  - native/include/xnn_transfer/core/transfer/**
  - native/src/transfer/**
  - native/tests/transfer/**
  - native/src/storage/**
  - native/tests/storage/**
  - native/src/session/**
  - native/tests/session/**
contract_changes: []
handoff: .agents/handoffs/XT-029.md
---

## Outcome

Make cancellation and shutdown deterministic and idempotent across session,
transfer, temporary storage, and worker lifecycle boundaries.

## Context

Protocol v1 defines CANCEL/CANCEL_ACK semantics. XT-028 provides the one-file
engine whose resource and concurrency paths this task closes.

## Constraints

- Cancellation must stop new reads/writes and bound already queued work.
- Repeated local or remote cancellation returns the same terminal result.
- Engine stop is a barrier for network callbacks, file writes, and events.
- Never delete a committed destination or another transfer's temporary file.

## Acceptance criteria

- [ ] Deterministic tests cancel before acceptance, during each transfer phase,
  during commit, after completion, and concurrently from both peers.
- [ ] Fault injection proves descriptors, sockets, buffers, workers, and
  temporary files are released exactly once.
- [ ] Stop/cancel race tests pass under sanitizers.
- [ ] `make security-test` passes.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
