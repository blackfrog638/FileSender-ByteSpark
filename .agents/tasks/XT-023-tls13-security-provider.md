---
id: XT-023
title: Tls13 security provider
initial_state: ready
workstream: native_core
initial_owner: unassigned
depends_on:
  - XT-018
  - XT-015
owned_paths:
  - native/include/xnn_transfer/core/security/tls/**
  - native/src/security/tls/**
  - native/tests/security/tls/**
contract_changes: []
handoff: .agents/handoffs/XT-023.md
---

## Outcome

Implement the ADR 0002 cryptographic and TLS 1.3 provider boundary and prove it
against the accepted security-profile vectors on every supported platform.

## Context

XT-009 through XT-015 provide byte-exact vectors and subgroup validation.
XT-018 selects the provider and pins its dependency version.

## Constraints

- Enforce TLS 1.3 only, fresh ECDHE, exact Ed25519 pinning, approved suites,
  ALPN/profile identifiers, exporter labels, no 0-RTT, and no resumption.
- Validate public keys before transcript or possession use.
- Keep keys, exporter values, SAS material, and precise trust failures secret.
- Use production randomness only; deterministic inputs are test-only.

## Acceptance criteria

- [ ] Provider outputs match every accepted positive golden vector.
- [ ] Wrong pin, wrong key, malformed point, downgrade, resumption, early data,
  exporter mismatch, replay, and role substitution fail closed.
- [ ] TLS configuration tests run on macOS, Windows, and Linux CI.
- [ ] `make security-test` passes.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
