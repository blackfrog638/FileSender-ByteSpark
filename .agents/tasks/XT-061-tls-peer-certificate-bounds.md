---
id: XT-061
title: Tls peer certificate bounds
state: ready
workstream: native_core
owner: unassigned
depends_on:
  - XT-023
  - XT-060
owned_paths:
  - native/src/security/tls/tls_provider.cpp
  - native/tests/security/tls/tls_provider_test.cpp
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-PAIRING
delivery_role: implementation
contract_changes:
  - Enforce the pairing profile certificate byte and chain limits before adoption
handoff: .agents/handoffs/XT-061.md
---

## Outcome

Enforce the XT-060 peer-certificate and chain ceilings in the canonical TLS
provider before hostile handshake input can cause unbounded certificate
allocation or produce a verified capability.


## Context
XT-023 enforces TLS 1.3, Ed25519 identity certificates, X25519, exact ALPN,
pinning, and exporter separation. XT-024 recorded that OpenSSL still lacks a
profile-owned peer-certificate byte ceiling. XT-060 registers the exact limit
before this task changes provider behavior.

## Constraints
- Configure OpenSSL's peer certificate-list limit from the registered profile
  before any handshake starts; do not rely only on post-handshake chain checks.
- Preserve the existing requirement for exactly one self-signed Ed25519
  identity certificate and exact raw-key pinning.
- Fail closed for a certificate or encoded chain at, below, and above every
  specified boundary without accepting partial parsing.
- Exercise client and server roles, fragmented input, oversized single
  certificates, oversized chains, and valid maximum-sized input.
- Keep tickets, cache, early data, TLS versions, ciphers, group, signature
  algorithm, ALPN, pin, and exporter behavior unchanged.
- Do not add a parallel TLS provider or move policy into XT-025.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] The canonical context enforces the XT-060 certificate-list ceiling before
      handshake certificate processing.
- [ ] Both endpoint roles reject oversized single-certificate and chain input.
- [ ] Exact boundary tests prove valid input still completes profile and pin
      verification.
- [ ] Existing TLS profile, exporter, no-resumption, and early-data tests pass.
- [ ] No public C++ or C ABI surface changes.
- [ ] Repository verification passes.

## Verification

```bash
make native-test
make security-test
make verify
```
