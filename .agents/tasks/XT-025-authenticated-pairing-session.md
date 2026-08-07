---
id: XT-025
title: Authenticated pairing session
initial_state: ready
workstream: native_core
initial_owner: unassigned
depends_on:
  - XT-020
  - XT-024
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

XT-020 supplies untrusted endpoints; XT-024 accepts the identity and TLS runtime.
ADR 0002 and protocol v1 own all transcript and binding semantics.

## Constraints

- Pairing is possible only during a short-lived local pairing window.
- Presentation may request actions but cannot inject keys, SAS words,
  transcripts, exporter values, roles, or trust state.
- Bound concurrent attempts, pre-auth bytes/messages, source rate, and timeouts.
- Every failure closes partial transport and zeroizes ephemeral material.

## Acceptance criteria

- [ ] Initiator/responder state tests cover success and explicit two-sided
  confirmation.
- [ ] MITM, wrong SAS decision, replay, role swap, timeout, revoked peer,
  replaced key, duplicate attempt, and shutdown fail closed.
- [ ] Paired reconnect accepts only fresh TLS with exact active pin.
- [ ] `make security-test` passes.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
