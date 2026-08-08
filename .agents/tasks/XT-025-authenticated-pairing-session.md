---
id: XT-025
title: Authenticated pairing session
initial_state: ready
workstream: native_core
initial_owner: unassigned
depends_on:
  - XT-020
  - XT-024
  - XT-060
  - XT-061
owned_paths:
  - native/include/xnn_transfer/core/session/**
  - native/src/session/**
  - native/tests/session/**
contract_changes: []
handoff: .agents/handoffs/XT-025.md
---

## Outcome

Implement bounded first-pair and paired-session state machines with explicit
local confirmation, rejection, revocation, timeout, and exact pin enforcement.

## Context

XT-020 supplies untrusted endpoints; XT-024 accepts the identity and TLS
provider boundaries. XT-060 registers the complete pairing-control wire
profile and XT-061 enforces its peer-certificate ceiling. ADR 0002 and the
versioned pairing specification own every transcript and binding semantic.

## Constraints

- Pairing is possible only during a short-lived local pairing window.
- Presentation may request actions but cannot inject keys, SAS words,
  transcripts, exporter values, roles, or trust state.
- Bound concurrent attempts, pre-auth bytes/messages, source rate, and timeouts.
- Every failure closes partial transport and zeroizes ephemeral material.
- An unpinned TLS capability is valid only inside one bounded first-pairing
  attempt. Established transport always supplies the active repository pin.
- `CommitPeer`, rotation, and revocation require current authenticated state;
  untrusted network, discovery, C ABI, or presentation input cannot directly
  invoke repository trust transitions.
- Identity reset, revocation, and active-key replacement invalidate live and
  cached authorization before another application message is dispatched.
- Linux storage unavailability disables Linux pairing without weakening the
  session or falling back to another store; XT-062 gates the later
  three-platform acceptance claim.
- Replace the canonical `xnn_transfer_session` placeholder in place; do not add
  a parallel session provider target.

## Acceptance criteria

- [ ] Initiator/responder state tests cover success and explicit two-sided
  confirmation.
- [ ] MITM, wrong SAS decision, replay, role swap, timeout, revoked peer,
  replaced key, duplicate attempt, and shutdown fail closed.
- [ ] Paired reconnect accepts only fresh TLS with exact active pin.
- [ ] Identity reset, revocation, and rotation cancel stale live and cached
      session authority.
- [ ] Every implemented transition and error value matches XT-060 golden
      vectors without private wire extensions.
- [ ] `make security-test` passes.
- [ ] Repository verification passes.

## Verification

```bash
make native-test
make security-test
make verify
```
