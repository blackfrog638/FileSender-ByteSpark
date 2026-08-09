---
id: XT-022
title: Protected identity storage
initial_state: ready
workstream: native_core
initial_owner: unassigned
depends_on:
  - XT-018
  - XT-051
owned_paths:
  - native/include/xnn_transfer/core/security/identity/**
  - native/src/security/identity/**
  - native/tests/security/identity/**
  - docs/adr/0009-protected-identity-storage.md
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-PAIRING
delivery_role: implementation
contract_changes: []
handoff: .agents/handoffs/XT-022.md
---

## Outcome

Generate and persist the device identity and pairing records through
platform-protected, non-synchronizing storage adapters with fail-closed errors.

## Context

ADR 0002 defines identity, revocation, rollback, and storage requirements.
XT-018 selects the supported platform facilities and dependency policy.

## Constraints

- Never persist plaintext private keys or use a filesystem fallback.
- Model locked, missing, corrupt, permission-denied, rollback, and identity-loss
  states without silently regenerating trusted relationships.
- Keep private key material out of logs, exceptions, and Flutter-visible data.
- Use injectable test stores; production adapters must target all three OSes.
- Replace the canonical `xnn_transfer_identity` placeholder in place; do not
  add a parallel identity provider target.

## Acceptance criteria

- [ ] Atomic identity creation is race-safe and stable across process restart.
- [ ] Pairing record updates, revocation, corruption, and rollback handling have
  deterministic negative tests.
- [ ] Platform tests prove non-synchronizing protected storage or document an
  explicit unsupported state that disables pairing.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
