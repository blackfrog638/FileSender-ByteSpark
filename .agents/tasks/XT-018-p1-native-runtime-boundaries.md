---
id: XT-018
title: P1 native runtime boundaries
initial_state: ready
workstream: integration
initial_owner: unassigned
depends_on:
  - XT-017
owned_paths:
  - docs/adr/0006-p1-native-runtime.md
  - docs/architecture.md
  - native/CMakeLists.txt
  - native/src/**/CMakeLists.txt
  - native/tests/**/CMakeLists.txt
contract_changes:
  - Select the P1 networking, TLS, crypto, and protected-storage runtime.
  - Establish independent native module build boundaries.
handoff: .agents/handoffs/XT-018.md
---

## Outcome

Accept a cross-platform P1 runtime stack and make each future native module
independently buildable without competing for `native/CMakeLists.txt`.

## Context

ADR 0002 defines the mandatory TLS, identity, and secure-storage properties.
XT-017 provides auditable squash integration. This task chooses dependencies
and build boundaries before networking or security implementation starts.

## Constraints

- Support macOS, Windows, and Linux with reproducible dependency versions.
- Reject providers that cannot enforce ADR 0002 TLS 1.3, pinning, no 0-RTT,
  no resumption, Ed25519, X25519, HKDF-SHA256, and exporter requirements.
- Do not add plaintext, file-backed, or synchronizing secret-storage fallback.
- Create leaf CMake entry points for discovery, identity, TLS, session,
  storage, and transfer modules without claiming those modules exist.

## Acceptance criteria

- [ ] ADR 0006 records selected versions, supply-chain policy, alternatives,
  platform constraints, and upgrade ownership.
- [ ] `docs/architecture.md` names durable module and dependency boundaries.
- [ ] Empty leaf module CMake entry points configure on all current presets.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
