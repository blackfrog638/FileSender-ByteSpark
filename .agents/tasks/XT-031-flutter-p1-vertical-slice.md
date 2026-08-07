---
id: XT-031
title: Flutter p1 vertical slice
initial_state: ready
workstream: flutter_desktop
initial_owner: unassigned
depends_on:
  - XT-021
  - XT-026
  - XT-030
owned_paths:
  - apps/desktop/lib/app/**
  - apps/desktop/lib/features/**
  - apps/desktop/test/features/**
contract_changes: []
handoff: .agents/handoffs/XT-031.md
---

## Outcome

Deliver the first usable Flutter flow: discover a peer, pair with visible SAS,
choose one file, accept or reject an incoming offer, and observe progress.

## Context

XT-021, XT-026, and XT-030 provide the only native adapters. Flutter owns
presentation and application orchestration but no networking or trust logic.

## Constraints

- Clearly distinguish untrusted discovered peers from authenticated peers.
- Require explicit user actions for pairing confirmation and incoming offers.
- Never display raw secrets, keys, filesystem internals, or attacker-controlled
  rich text.
- Commands must handle duplicate taps, stale events, cancellation, and disposal.

## Acceptance criteria

- [ ] Widget tests cover peer expiry, pairing success/rejection/failure,
  send selection, incoming prompt, progress, cancellation, and completion.
- [ ] Accessibility and keyboard flows work on desktop form factors.
- [ ] The production app uses native gateways rather than fake transfer data.
- [ ] UI text makes no unsupported multi-file or resume claim.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
