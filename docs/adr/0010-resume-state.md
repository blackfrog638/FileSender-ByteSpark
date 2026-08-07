# ADR 0010: Persisted transfer resume state

- Status: proposed
- Date: 2026-08-07
- Owner task: XT-036
- Wire contract: `protocol/spec/v1.md`

## Context

P1 production behavior must resume an authenticated transfer after reconnect or
process restart. Persisted state affects compatibility, storage safety,
rollback behavior, retention, peer authorization, and cleanup, so it requires a
reviewed format and migration policy.

## Proposed boundary

XT-036 will define a versioned logical schema containing only the authenticated
peer scope, transfer and manifest commitments, negotiated protocol/capabilities,
verified file offsets, destination/storage identity, authorization metadata,
and expiry needed by protocol v1.

Resume state must be atomic, integrity-protected, bounded, peer-bound, and
invalidated by revocation, identity loss, incompatible versions, destination
change, file mismatch, corruption, rollback, or expiry. Persisted data never
overrides fresh TLS identity proof or explicit receiver policy.

## Acceptance boundary

The ADR remains proposed until XT-036 provides:

- schema versioning, migration, retention, and deletion rules;
- atomic update and crash-consistency evidence;
- authorization and rollback threat analysis;
- reconnect and process-restart tests;
- malformed, replayed, stale, wrong-peer, wrong-file, and revoked-peer tests;
- integration-owner review on all supported platforms.
