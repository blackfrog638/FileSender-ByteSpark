---
id: XT-030
title: Transfer c abi
initial_state: ready
workstream: native_bridge
initial_owner: unassigned
depends_on:
  - XT-026
  - XT-029
owned_paths:
  - native/include/xnn_transfer/c_api.h
  - native/src/bridge/**
  - native/tests/bridge/**
  - apps/desktop/lib/core/native/**
  - docs/adr/0008-p1-operation-c-abi.md
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-ONE-FILE-TRANSFER
  - REQ-P1-FLUTTER-FLOW
  - REQ-P1-CANCELLATION
delivery_role: implementation
contract_changes:
  - Add one-file offer, acceptance, progress, completion, and cancellation C ABI.
handoff: .agents/handoffs/XT-030.md
---

## Outcome

Expose one-file send/receive commands and bounded progress events through the C
ABI and implement the real Dart `TransferGateway`.

## Context

XT-026 establishes pairing ABI ordering. XT-029 provides terminal cancellation
semantics. Existing Flutter application state defines the gateway contract.

## Constraints

- File selection crosses the ABI as copied, bounded platform path input.
- Incoming offers expose only authenticated peer display data and validated
  one-file metadata.
- Progress is monotonic, bounded, and recoverable after event overflow.
- No Dart code opens transfer sockets or processes protocol frames.

## Acceptance criteria

- [ ] ABI tests cover send, offer, accept, reject, cancel, progress, completion,
  invalid IDs, short structs, versions, overflow, and shutdown.
- [ ] Dart gateway maps native failures to stable application error classes.
- [ ] Packaged callback tests exercise the real transfer event path.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
