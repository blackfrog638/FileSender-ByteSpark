---
id: XT-014
title: Validate Ed25519 fixture keys
initial_state: ready
workstream: protocol
initial_owner: unassigned
depends_on:
  - XT-013
owned_paths:
  - protocol/testdata/security/v1/**
contract_changes: []
handoff: .agents/handoffs/XT-014.md
---

## Outcome

Resolve XT-010 finding BR-03 by ensuring every fixture value described as an
Ed25519 public key is a canonical RFC 8032 point. The wrong-key device-ID case
must compare two distinct valid keys, with invalid-point input covered
separately.

## Context

XT-010 remains blocked on `task/XT-010`. XT-013 closed the rejection and
device-ID encoding issues, but the independent reviewer proved that baseline
responder bytes `60..7f` are not an Ed25519 point. ADR 0002 requires the exact
pin bytes to represent a valid proved identity key.

## Constraints

- Use independently published or generated Ed25519 public keys with documented
  provenance; length alone is not validity.
- Validate canonical RFC 8032 compressed-point decoding for every positive key
  fixture without a network or third-party runtime dependency.
- Reject noncanonical and non-decodable points before context or device-ID use.
- Recompute every output affected by replacing invalid identity-key bytes and
  explicitly document unavoidable golden-vector drift.
- Do not change labels, algorithms, role ordering, reject semantics, or make
  ADR 0002 accepted.

## Acceptance criteria

- [ ] All positive initiator, responder, rotation, and wrong-key public-key
      fixtures decode as canonical RFC 8032 points.
- [ ] The device-ID wrong-key vector uses a second distinct valid public key.
- [ ] Invalid-point and noncanonical-point negatives have stable failures.
- [ ] All outputs affected by valid-key replacement are recomputed and drift
      is documented.
- [ ] The independent XT-010 reviewer can reproduce point validity and amended
      outputs without importing the fixture oracle.
- [ ] Focused vector validation and `make verify` pass.

## Verification

```bash
python3 protocol/testdata/security/v1/validate_vectors.py
make verify
```
