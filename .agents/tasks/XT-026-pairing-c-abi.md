---
id: XT-026
title: Pairing c abi
initial_state: ready
workstream: native_bridge
initial_owner: unassigned
depends_on:
  - XT-021
  - XT-025
owned_paths:
  - native/include/xnn_transfer/c_api.h
  - native/src/bridge/**
  - native/tests/bridge/**
  - apps/desktop/lib/core/native/**
  - docs/adr/0008-p1-operation-c-abi.md
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-PAIRING
  - REQ-P1-FLUTTER-FLOW
delivery_role: implementation
contract_changes:
  - Add pairing, confirmation, rejection, and revocation operations to the C ABI.
handoff: .agents/handoffs/XT-026.md
---

## Outcome

Expose pairing attempts and trust-state events through bounded C ABI commands
without exposing secret or caller-controlled transcript material.

## Context

XT-025 owns the native state machine. ADR 0003 owns event delivery; ADR 0002
forbids presentation from supplying identity or transcript values.

## Constraints

- Preserve additive struct-size/version negotiation and caller-owned memory.
- SAS words are display-only bounded data tied to an opaque attempt ID.
- Commands are idempotent where specified and reject stale attempt IDs.
- Error classes must not expose a peer-enumeration or trust-record oracle.

## Acceptance criteria

- [ ] ABI tests cover open/close window, start, confirm, reject, revoke, stale
  handles, invalid states, short structs, overflow, and shutdown.
- [ ] Dart copies pairing events and never handles native secret pointers.
- [ ] Existing discovery and lifecycle ABI behavior remains compatible.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
