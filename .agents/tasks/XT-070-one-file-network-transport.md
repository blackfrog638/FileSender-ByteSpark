---
id: XT-070
title: One file network transport
state: ready
workstream: native_bridge
owner: unassigned
depends_on:
  - XT-029
  - XT-030
  - XT-069
owned_paths:
  - .agents/records/XT-070.json
  - .agents/tasks/XT-070-one-file-network-transport.md
  - .agents/handoffs/XT-070.md
  - native/src/core/**
  - native/tests/core/**
  - native/src/bridge/**
  - native/tests/bridge/**
  - native/CMakeLists.txt
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-PAIRING
  - REQ-P1-ONE-FILE-TRANSFER
  - REQ-P1-FLUTTER-FLOW
  - REQ-P1-CANCELLATION
delivery_role: implementation
contract_changes: []
handoff: .agents/handoffs/XT-070.md
---

## Outcome

Replace the fail-closed production pairing and transfer backends with real
native composition that pairs discovered peers and sends, receives, accepts,
rejects, cancels, verifies, and commits one file over authenticated TLS.

## Context

XT-028 and XT-029 own the one-file protocol engine and cleanup contract.
XT-030 exposes commands and bounded events but its production backend returns
`UNAVAILABLE`. XT-069 supplies typed authenticated connections. This task
connects those completed pieces without extending the public C ABI.

## Constraints

- Resolve outgoing addresses only from a live discovery observation and bind
  every established connection to the authenticated device identity.
- Complete `TRANSPORT_FINISHED` verification before transfer dispatch.
- Assemble complete bounded frames from partial reads and serialize outbound
  frames in exact connection message-ID order, including partial writes.
- Report ACK and decision write completion only after bytes reach the socket.
- Open sender files and receiver transactions through native platform adapters;
  never accept a peer-provided local path.
- Commit accepted files into a native-owned application receive root with no
  implicit overwrite; XT-035 later adds explicit destination policy.
- Publish incoming offers only after authenticated metadata validation.
- Stop dispatch after connection-fatal errors and make cancel, disconnect,
  process exit, and shutdown cleanup idempotent.
- Preserve all XT-030 symbols, layouts, snapshots, and error folding.

## Architecture change

The record declares `none`: production composition fills the existing core and
bridge runtime boundaries and does not add a provider or public dependency.

## Acceptance criteria

- [ ] Two real processes pair and transfer identical one-file bytes over TLS
      only after receiver acceptance.
- [ ] Reject and cancel send their protocol decisions and leave no committed
      partial file, stale transaction, authorization entry, or worker.
- [ ] Incoming offers, progress, completion, and terminal failures reach the
      existing C ABI bridge without synthetic updates.
- [ ] Tests cover fragmented reads/writes, reordered completion callbacks,
      disconnects, fatal frames, integrity failure, no-space, process exit,
      duplicate commands, and shutdown races.
- [ ] The production pairing and transfer paths no longer return
      `UNAVAILABLE` when required platform services and a reachable peer exist.
- [ ] `make native-test`, `make security-test`, and `make verify` pass.

## Verification

```bash
make native-test
make security-test
make verify
```
