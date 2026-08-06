---
id: XT-004
title: Define Flutter transfer application state
state: done
workstream: flutter_desktop
owner: flutter-agent
depends_on: []
owned_paths:
  - apps/desktop/lib/features/transfer/**
  - apps/desktop/test/features/transfer/**
contract_changes:
  - Internal Flutter transfer gateway and application-state model
---

## Outcome

Provide immutable Flutter transfer state and deterministic controller
transitions without claiming a native transfer implementation.

## Acceptance criteria

- [x] Transfer lifecycle and command outcomes are typed.
- [x] Initialization events are buffered without loss.
- [x] Offer withdrawal races are deterministic.
- [x] Negative state transitions have focused Flutter tests.

## Verification

See `.agents/records/XT-004.json` and
`apps/desktop/test/features/transfer/XT-004_HANDOFF.md`.
