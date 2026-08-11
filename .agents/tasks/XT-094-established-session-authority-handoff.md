---
id: XT-094
title: Established session authority handoff
state: ready
workstream: native_core
owner: unassigned
depends_on:
  - XT-093
owned_paths:
  - .agents/records/XT-094.json
  - .agents/tasks/XT-094-established-session-authority-handoff.md
  - .agents/handoffs/XT-094.md
  - native/include/xnn_transfer/core/session/**
  - native/src/session/**
  - native/tests/session/**
delivery_plan: DP-P1-TRANSPORT-COMPOSITION
requirement_ids:
  - REQ-P1-TRANSPORT-COMPOSITION-CONTRACT
delivery_role: implementation
contract_changes:
  - Expose canonical role-ordered negotiation normalization for established transport binding.
  - Add a one-shot authenticated connection handoff into SessionAuthority with ordered deactivation.
handoff: .agents/handoffs/XT-094.md
---

## Outcome

Make an authenticated established connection consumable by the transfer engine:
build the accepted normalized negotiation, bind TRANSPORT_FINISHED, transfer
the bound channel exactly once into `SessionAuthority`, and keep authorization
valid only for the lifetime of its TLS byte stream.

## Context

XT-093 supplies validated role-ordered v1 profile-context fields. XT-069 owns
the TLS stream and lends only `EstablishedTlsChannel&`, while XT-028 requires a
live `SessionAuthority::IsAuthorized` handle. The current interfaces cannot
move the only channel into `SessionAuthority::Activate` without a double owner.

Refactor the existing private negotiation normalization into a reviewed public
session-domain operation, then add a one-shot connection activation operation
that runs on the network executor, moves only a fully transport-bound channel
into `SessionAuthority`, returns the resulting `SessionHandle` through the
serialized completion boundary, and records enough ownership to deactivate it
before stream teardown.

## TDD contract

Before production edits, add only session tests that fail with both exact
lines `FAILED: normalized transport negotiation is not publicly composable`
and `FAILED: established transport cannot activate session authority`.
Checkpoint that Red revision with `agent.sh checkpoint XT-094 red`.

## Constraints

- Activation requires local finished-write completion and verified peer
  finished data on the current exact-pin TLS handshake.
- Reject pre-binding, repeated, stale-revision, revoked, stopped, and racing
  activation without leaking or duplicating channel ownership.
- Preserve serialized read/write ordering and keep the TLS stream alive after
  the channel moves into `SessionAuthority`.
- Deactivate the session handle before socket close on normal disconnect,
  transport failure, explicit close, runtime stop, and destruction.
- Stop remains a callback and worker barrier; no completion may begin after it
  returns, and retained handles must not outlive required executor services.
- Preserve the C ABI, wire bytes defined by XT-093, TLS profile, and existing
  source-compatible channel read/write methods.

## Architecture change

The record declares `refactor` mode for the canonical `session` module. It
extends the existing connection capability and authority in place, with no
parallel session provider or new production target.

## Risk profile

The schema-v4 record binds critical authorization, concurrency, compatibility,
and platform risk to `native_test`, `security_test`, and `verify`.

## Acceptance criteria

- [ ] Public normalization produces the byte-identical accepted negotiation
      object for both roles and rejects noncanonical selection inputs.
- [ ] A fully bound channel activates exactly once and produces a
      `SessionHandle` accepted by the existing one-file sender and receiver.
- [ ] Pre-binding, duplicate, stale, revoked, and post-stop activation fail
      without a synthetic handle or ownership leak.
- [ ] Normal close, error, explicit close, stop, and destruction deactivate
      before TLS stream teardown and leave zero active sessions.
- [ ] Initiator and responder paths survive reordered completions and
      concurrent close/stop races without late callbacks, double release, or
      executor lifetime faults.
- [ ] Existing connection runtime and session authority behavior remains
      source compatible.
- [ ] `make native-test`, `make security-test`, and `make verify` pass.

## Verification

```bash
make native-test
make security-test
make verify
```
