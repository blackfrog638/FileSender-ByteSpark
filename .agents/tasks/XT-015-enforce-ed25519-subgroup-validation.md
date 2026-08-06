---
id: XT-015
title: Enforce Ed25519 subgroup validation
initial_state: ready
workstream: protocol
initial_owner: unassigned
depends_on:
  - XT-014
owned_paths:
  - protocol/testdata/security/v1/**
  - docs/adr/0002-pairing-and-transport-security.md
contract_changes: []
handoff: .agents/handoffs/XT-015.md
---

## Outcome

Resolve XT-010 finding BR-04 by requiring every Ed25519 identity or rotation
key to be a non-identity member of the prime-order subgroup, not merely a
decodable Edwards25519 point.

## Context

The third independent XT-010 review demonstrated that OpenSSL, Node, and Apple
CryptoKit can accept an identity-key signature without a unique private-key
possessor, while libsodium rejects low- and mixed-order points. XT-014 point
decompression alone therefore permits authentication failure and
cross-platform divergence.

## Constraints

- Require canonical point decoding, `P != identity`, and `[L]P = identity` for
  Ed25519 subgroup order `L`.
- Reject identity, low-order, and mixed-order keys before transcript hashing,
  pin comparison, device-ID derivation, or signature verification.
- Include the identity-key forgery evidence and a mixed-order case that defeats
  a small-order blacklist or cofactor-only check.
- Preserve all accepted RFC 8032 TEST 1/2/3 outputs and existing rejection
  outcomes.
- ADR 0002 remains `proposed`; only XT-010 may recommend acceptance.

## Acceptance criteria

- [ ] ADR 0002 normatively requires non-identity prime-subgroup keys.
- [ ] Validator performs subgroup membership rather than a fixed blacklist.
- [ ] Identity, order-2/low-order, and mixed-order vectors fail before use.
- [ ] RFC 8032 TEST 1/2/3 outputs and existing error contracts do not drift.
- [ ] Independent XT-010 review can reproduce the subgroup and forgery checks.
- [ ] Focused vector validation and `make verify` pass.

## Verification

```bash
python3 protocol/testdata/security/v1/validate_vectors.py
make verify
```
