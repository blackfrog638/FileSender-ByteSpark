---
id: XT-019
title: Discovery v1 contract
initial_state: ready
workstream: protocol
initial_owner: unassigned
depends_on:
  - XT-018
owned_paths:
  - protocol/spec/discovery-v1.md
  - protocol/testdata/discovery/v1/**
  - docs/adr/0007-lan-discovery.md
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-DISCOVERY
delivery_role: implementation
contract_changes:
  - Add the unauthenticated LAN discovery v1 wire contract.
handoff: .agents/handoffs/XT-019.md
---

## Outcome

Define a bounded, versioned discovery advertisement and lifecycle contract with
golden vectors before any socket accepts LAN input.

## Context

Discovery is explicitly untrusted in ADR 0002 and `docs/architecture.md`.
XT-018 selects the runtime constraints; this task owns only discovery wire
semantics, expiry, duplicate handling, and interface-change behavior.

## Constraints

- Advertisements contain reachability hints only, never trust or file metadata.
- Bound datagram size, fields, rates, peer count, TTL, and parser allocation.
- Specify interface add/remove, sleep/wake, address churn, self-filtering, and
  deterministic peer expiry.
- Unknown versions and malformed inputs fail closed without peer creation.

## Acceptance criteria

- [ ] ADR 0007 and `discovery-v1.md` define byte-exact wire behavior.
- [ ] Positive and hostile vectors cover truncation, spoofing-relevant fields,
  limits, duplicate advertisements, and expiry calculations.
- [ ] Compatibility and privacy sections forbid authentication claims.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
