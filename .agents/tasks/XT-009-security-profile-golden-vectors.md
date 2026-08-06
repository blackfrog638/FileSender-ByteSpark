---
id: XT-009
title: Define security profile golden vectors
initial_state: ready
workstream: protocol
initial_owner: unassigned
depends_on:
  - XT-001
  - XT-002
owned_paths:
  - protocol/testdata/security/v1/**
  - docs/adr/0002-pairing-and-transport-security.md
contract_changes: []
handoff: .agents/handoffs/XT-009.md
---

## Outcome

Provide deterministic, byte-exact v1 security-profile vectors and a validator
covering transcript construction, role separation, exporter inputs, SAS words,
peer confirmations, transport-finished values, and key-rotation proof inputs.
This task defines test evidence only and does not implement TLS or pairing.

## Context

ADR 0002 defines the proposed security profile. `protocol/spec/v1.md` defines
framing and negotiation, while XT-001 and XT-002 delivered the threat model and
wire contract. XT-010 uses these vectors for independent ADR acceptance.

## Constraints

- Fixtures must use canonical encodings, explicit roles, fixed labels, lengths,
  and expected failure classes; host byte order and platform Unicode behavior
  must not affect results.
- Cover legal initiator/responder transcripts plus malformed, swapped-role,
  downgrade, replay, context-omission, and length-boundary cases.
- Exporter material is fixture input; do not claim a live TLS implementation.
- Do not add an unreviewed runtime crypto dependency or production fallback.
- ADR 0002 remains `proposed`; clarifications must not silently weaken it.

## Acceptance criteria

- [ ] A versioned manifest identifies every vector, input encoding, expected
      output or stable failure, and security invariant.
- [ ] Positive vectors cover both roles, all domain-separated contexts, SAS,
      confirmations, transport binding, and rotation proof inputs.
- [ ] Negative vectors cover malformed encoding, role/key swaps, replay,
      downgrade, omitted context, wrong lengths, and output mismatch.
- [ ] A deterministic validator passes on macOS, Linux, and Windows without
      network access.
- [ ] ADR clarifications remain proposed and production behavior remains
      explicitly unimplemented.
- [ ] Focused vector validation and `make verify` pass.

## Verification

```bash
python3 protocol/testdata/security/v1/validate_vectors.py
make verify
```
