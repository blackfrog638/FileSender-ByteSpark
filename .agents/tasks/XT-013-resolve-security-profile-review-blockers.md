---
id: XT-013
title: Resolve security profile review blockers
initial_state: ready
workstream: protocol
initial_owner: unassigned
depends_on:
  - XT-009
owned_paths:
  - protocol/testdata/security/v1/**
  - docs/adr/0002-pairing-and-transport-security.md
contract_changes: []
handoff: .agents/handoffs/XT-013.md
---

## Outcome

Resolve XT-010 findings BR-01 and BR-02 by making authenticated rejection
semantics and stable device-identifier derivation byte-exact in ADR 0002 and
the security-profile vectors. This task does not accept the ADR.

## Context

XT-010 remains blocked with its review report and handoff on
`task/XT-010`. XT-009 supplied the current vectors. After this task is
accepted, XT-010 returns to `in_progress` for independent recalculation and
ADR disposition.

## Constraints

- A valid authenticated `decision=00` is a terminal rejection, never
  affirmative peer consent or permission to persist trust.
- Trust requires both local and peer decisions to be exactly `01` for the same
  live attempt; invalid decisions fail closed.
- Device identifier derivation must define exact label octets, canonical input,
  SHA-256 output, and representation without weakening public-key pinning.
- Extend positive and hostile vectors without changing unrelated profile
  outputs or introducing a fallback decoder.
- ADR 0002 remains `proposed`; only the independent XT-010 reviewer may
  recommend acceptance.

## Acceptance criteria

- [ ] Role-specific authenticated reject outputs and typed terminal semantics
      are covered.
- [ ] Reject-where-confirm-required, invalid-decision, and substitution vectors
      map to P-09, SEC-03, and SEC-18.
- [ ] Device identifier derivation and output representation are byte-exact.
- [ ] Positive device-ID plus alternate-label, alternate-encoding, and
      wrong-key negatives are covered.
- [ ] Existing positive vector outputs remain stable unless the changed
      contract explicitly requires otherwise.
- [ ] Focused vector validation and `make verify` pass.

## Verification

```bash
python3 protocol/testdata/security/v1/validate_vectors.py
make verify
```
