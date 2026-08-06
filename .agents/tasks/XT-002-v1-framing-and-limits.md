---
id: XT-002
title: Specify v1 framing and protocol limits
state: done
workstream: protocol
owner: protocol-agent
depends_on: []
owned_paths:
  - protocol/spec/v1.md
  - protocol/testdata/v1/**
contract_changes:
  - Version 1 wire framing, negotiation, limits, and error semantics
---

## Outcome

Specify a bounded, fail-closed v1 framing and negotiation contract with
machine-checked legal and malformed vectors.

## Acceptance criteria

- [x] Fixed header, TLV, message, error, state, and compatibility rules exist.
- [x] Hard limits and directional message identifiers are explicit.
- [x] Golden vectors cover legal and hostile framing transcripts.
- [x] Security-dependent behavior remains bound to the security ADR.

## Verification

See `.agents/records/XT-002.json` and
`protocol/testdata/v1/HANDOFF.md`.
