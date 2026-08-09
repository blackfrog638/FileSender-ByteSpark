---
id: XT-020
title: Native discovery service
initial_state: ready
workstream: native_core
initial_owner: unassigned
depends_on:
  - XT-019
owned_paths:
  - native/include/xnn_transfer/core/discovery/**
  - native/src/discovery/**
  - native/tests/discovery/**
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-DISCOVERY
delivery_role: implementation
contract_changes: []
handoff: .agents/handoffs/XT-020.md
---

## Outcome

Implement cross-platform LAN announcement, observation, bounded peer caching,
expiry, and interface-change recovery behind injectable native interfaces.

## Context

XT-019 provides the only accepted discovery wire contract. The service remains
an untrusted reachability source and does not expose pairing or transfer
authority.

## Constraints

- Parse and validate datagrams before cache insertion or allocation by counts.
- Serialize state ownership on the native engine runtime.
- Cover interface add/remove, sleep/wake rescan, duplicate endpoints, expiry,
  stop races, and self-advertisement filtering.
- No C ABI or Flutter changes are owned by this task.

## Acceptance criteria

- [ ] Deterministic tests use fake clock, interfaces, and datagram transport.
- [ ] Platform adapters discover peers on macOS, Windows, and Linux.
- [ ] Shutdown prevents callbacks or peer mutations after its barrier.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
