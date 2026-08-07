---
id: XT-021
title: Discovery c abi
initial_state: ready
workstream: native_bridge
initial_owner: unassigned
depends_on:
  - XT-020
owned_paths:
  - native/include/xnn_transfer/c_api.h
  - native/src/bridge/**
  - native/tests/bridge/**
  - apps/desktop/lib/core/native/**
  - docs/adr/0008-p1-operation-c-abi.md
contract_changes:
  - Add discovery commands and peer lifecycle events to the stable C ABI.
handoff: .agents/handoffs/XT-021.md
---

## Outcome

Expose bounded discovery start/stop commands and peer appeared/updated/expired
events through the existing wakeup-and-drain ABI and Dart native adapter.

## Context

XT-020 owns discovery behavior. ADR 0003 owns callback safety and
`c_api.h` remains the only Flutter-facing native boundary.

## Constraints

- Preserve struct-size/version negotiation and existing ABI v1 symbols.
- Copy all peer data into caller-owned bounded event payloads.
- Treat names, addresses, and capabilities as untrusted display data.
- Overflow must be observable and recoverable through a bounded snapshot API.

## Acceptance criteria

- [ ] ABI tests cover short structs, versions, invalid states, queue overflow,
  snapshot recovery, and shutdown races.
- [ ] Dart decodes peer events without retaining native pointers.
- [ ] Existing lifecycle callback and bundle tests remain compatible.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
