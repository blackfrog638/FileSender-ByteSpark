---
id: XT-069
title: Authenticated connection runtime
state: ready
workstream: native_core
owner: unassigned
depends_on:
  - XT-026
  - XT-061
  - XT-062
owned_paths:
  - .agents/records/XT-069.json
  - .agents/tasks/XT-069-authenticated-connection-runtime.md
  - .agents/handoffs/XT-069.md
  - native/include/xnn_transfer/core/security/tls/**
  - native/src/security/tls/**
  - native/tests/security/tls/**
  - native/include/xnn_transfer/core/session/**
  - native/src/session/**
  - native/tests/session/**
  - native/src/core/**
  - native/tests/core/**
  - native/CMakeLists.txt
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-PAIRING
delivery_role: implementation
contract_changes:
  - Add the typed native connection-runtime interface required by the accepted pairing and established-transport security profiles.
handoff: .agents/handoffs/XT-069.md
---

## Outcome

Implement the production native connection runtime that accepts and opens TLS
connections, selects exactly one registered ALPN mode, resolves established
peers against active exact pins, and drives authenticated pairing channels.

## Context

XT-023, XT-025, XT-060, and XT-061 provide the TLS, pairing, wire, and
certificate contracts. XT-026 intentionally returns `UNAVAILABLE` because no
same-port dispatcher, inbound pin resolver, or socket owner exists. This task
closes that explicit prerequisite without adding transfer behavior.

## Constraints

- Offer and accept only `xnn-transfer-pairing/1` or `xnn-transfer/1`; never
  negotiate or fall back between them.
- Resolve established inbound connections against the protected active trust
  repository before returning a typed established capability.
- Treat discovery addresses as untrusted reachability hints, not identity.
- Assemble bounded pairing-control frames, enforce monotonic deadlines, and
  report local decision-write completion to the session state machine.
- Keep socket, SSL, timer, executor, and callback lifetime behind one stop
  barrier; no callback may outlive engine shutdown.
- Pairing connections must never dispatch transfer frames.
- Do not change the C ABI or implement file transfer publication.

## Architecture change

The record declares `none`: this fills the existing engine-owned C++ worker
runtime and accepted TLS/session boundaries without introducing a parallel
provider or a new dependency direction.

## Acceptance criteria

- [ ] One listener demultiplexes both registered ALPN modes into distinct typed
      capabilities without returning a generic unpinned connection.
- [ ] Real loopback peers complete the pairing transcript, SAS decision, trust
      commit, timeout, rejection, and shutdown paths.
- [ ] Established inbound TLS requires an active exact pin; unknown, revoked,
      rotated, expired, or mismatched identity fails before session dispatch.
- [ ] Tests cover partial handshakes, malformed certificates, ALPN confusion,
      frame fragmentation, deadline expiry, callback races, and stop barriers.
- [ ] `make native-test`, `make security-test`, and `make verify` pass.

## Verification

```bash
make native-test
make security-test
make verify
```
