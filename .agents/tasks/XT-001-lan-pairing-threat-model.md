---
id: XT-001
title: Define the LAN pairing threat model
initial_state: done
workstream: protocol
initial_owner: security-agent
depends_on: []
owned_paths:
  - docs/adr/0002-pairing-and-transport-security.md
  - protocol/security/**
contract_changes:
  - Proposed pairing and authenticated transport security profile
---

## Outcome

Define hostile-LAN trust boundaries, security invariants, negative coverage,
and a proposed authenticated pairing and transport profile without claiming an
implementation.

## Acceptance criteria

- [x] Threat model identifies assets, attackers, boundaries, and failure modes.
- [x] Proposed ADR defines pairing, identity, forward secrecy, and downgrade
      behavior.
- [x] Negative-test matrix covers the release-blocking security invariants.
- [x] Production security claims remain explicitly blocked.

## Verification

See `.agents/records/XT-001.json` and
`protocol/security/XT-001-handoff.md`.
