# ADR 0006: P1 native runtime and dependency boundaries

- Status: proposed
- Date: 2026-08-07
- Owner task: XT-018
- Security profile: `docs/adr/0002-pairing-and-transport-security.md`

## Context

P1 adds cross-platform multicast discovery, TLS 1.3, Ed25519 identity,
protected storage, authenticated sessions, filesystem transactions, and
transfer scheduling. The repository must choose reproducible dependencies and
module boundaries before those implementations can make portability or
security claims.

## Decision drivers

- Enforce every mandatory ADR 0002 security property on macOS, Windows, and
  Linux.
- Keep domain state independent from sockets, TLS providers, operating-system
  secret stores, filesystems, clocks, and Flutter.
- Pin and audit third-party source and binary provenance.
- Preserve sanitizer, fuzz, packaging, and offline/reproducible build gates.
- Avoid overlapping ownership of central CMake files across P1 tasks.

## Candidate decision

XT-018 will evaluate the supported networking/TLS/crypto providers, protected
storage facilities, dependency acquisition policy, upgrade cadence, and
platform-specific interface monitoring. It will accept one stack only after a
cross-platform configure/build probe and ADR 0002 capability audit.

Native discovery, identity, TLS, session, storage, and transfer will have
separate leaf CMake targets and injectable domain-facing interfaces. This
proposal does not claim that any P1 runtime behavior is implemented.

## Acceptance boundary

The ADR remains proposed until XT-018 records:

- selected providers and pinned versions;
- mandatory build and runtime configuration;
- platform support and unsupported failure behavior;
- supply-chain verification and update policy;
- rejected alternatives and migration cost;
- successful repository verification on all supported platforms.
