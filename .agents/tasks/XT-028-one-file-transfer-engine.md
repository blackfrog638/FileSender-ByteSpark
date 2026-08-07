---
id: XT-028
title: One file transfer engine
initial_state: ready
workstream: native_core
initial_owner: unassigned
depends_on:
  - XT-006
  - XT-025
  - XT-027
owned_paths:
  - native/include/xnn_transfer/core/transfer/**
  - native/src/transfer/**
  - native/tests/transfer/**
contract_changes: []
handoff: .agents/handoffs/XT-028.md
---

## Outcome

Send and receive one explicitly accepted file over an authenticated v1 session,
with bounded chunks, flow control, integrity verification, and commit.

## Context

XT-025 supplies authenticated sessions, XT-027 supplies safe storage, and
protocol v1 defines offer, acceptance, chunks, acknowledgements, and commit.

## Constraints

- No offer or file metadata is processed before transport binding completes.
- Transfer starts only after explicit receiver acceptance.
- Bound chunk size, unacknowledged bytes, queues, counters, and arithmetic.
- Keep all file bytes inside the authenticated TLS channel.
- Replace the canonical `xnn_transfer_transfer` placeholder in place; do not
  add a parallel transfer engine target.

## Acceptance criteria

- [ ] Loopback integration tests complete a one-file sender/receiver transfer.
- [ ] Rejection produces no temporary or destination file.
- [ ] Corrupt, replayed, reordered, oversized, duplicate, early, and
  wrong-transfer frames fail at the specified scope.
- [ ] Hash verification precedes atomic commit and completion acknowledgement.
- [ ] `make security-test` passes.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
