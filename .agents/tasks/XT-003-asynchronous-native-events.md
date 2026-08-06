---
id: XT-003
title: Add the asynchronous native event ABI
initial_state: done
workstream: native_bridge
initial_owner: native-bridge-agent
depends_on:
  - XT-002
owned_paths:
  - native/include/xnn_transfer/c_api.h
  - native/src/bridge/**
  - native/tests/bridge/**
  - apps/desktop/lib/core/native/**
contract_changes:
  - Additive asynchronous C ABI event extension
---

## Outcome

Expose bounded lifecycle events through a wakeup-only callback and caller-owned
polling boundary that is safe for delayed Dart listeners and shutdown races.

## Acceptance criteria

- [x] ABI structs preserve version and size negotiation.
- [x] Event delivery has bounded queue and overflow semantics.
- [x] Callback ownership, reentry, and shutdown barriers are documented.
- [x] Native and packaged Dart callback tests cover concurrency regressions.

## Verification

See `.agents/records/XT-003.json` and
`native/tests/bridge/XT-003_HANDOFF.md`.
