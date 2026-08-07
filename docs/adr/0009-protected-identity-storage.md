# ADR 0009: Protected identity and pairing-record storage

- Status: proposed
- Date: 2026-08-07
- Owner task: XT-022
- Security profile: `docs/adr/0002-pairing-and-transport-security.md`

## Context

The device identity private key and pairing records are durable
security-sensitive state. Their encoding, atomicity, rollback behavior,
identity-loss behavior, and platform protection must be explicit before
pairing can be implemented.

## Proposed boundary

XT-022 will define a versioned logical record schema and platform adapters for
non-synchronizing protected storage. Private keys must never use a plaintext
filesystem fallback or cross the Flutter ABI. Pairing records must bind the
peer key, minimum security profile, revocation state, monotonic rotation state,
and integrity/version metadata.

Locked, unavailable, corrupt, rolled-back, or identity-mismatched storage fails
closed. Missing local identity may create a new identity only after invalidating
all previous local pairing relationships; it must never silently repair them.

## Acceptance boundary

The ADR remains proposed until XT-022 documents and tests:

- canonical logical fields and versioning;
- atomic create/update/revoke behavior;
- platform protection and non-synchronization guarantees;
- corruption, permission, rollback, restore, and identity-loss handling;
- bounded record counts and secret lifetime;
- migration and deletion policy on macOS, Windows, and Linux.
