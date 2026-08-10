---
id: XT-073
title: Resume authorization and recovery
state: ready
workstream: native_core
owner: unassigned
depends_on:
  - XT-036
owned_paths:
  - .agents/records/XT-073.json
  - .agents/tasks/XT-073-resume-authorization-and-recovery.md
  - .agents/handoffs/XT-073.md
  - native/include/xnn_transfer/core/session/**
  - native/src/session/**
  - native/tests/session/**
  - native/include/xnn_transfer/core/transfer/**
  - native/src/transfer/**
  - native/tests/transfer/**
  - native/benchmarks/**
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-RESUME
delivery_role: implementation
contract_changes: []
handoff: .agents/handoffs/XT-073.md
---

## Outcome

Authorize and resume transfers after reconnect or process restart by binding
the live authenticated session and protocol state to validated durable
checkpoints from XT-036.

## Context

XT-036 owns the versioned checkpoint codec, integrity, retention, and atomic
store. This task owns RESUME wire behavior, peer/session authorization,
verified-offset restoration, idempotency, and terminal cleanup.

## Constraints

- Bind resume to authenticated peer, roles, transfer ID, manifest commitment,
  destination identity, negotiated capabilities, and the live session.
- Restore only verified offsets represented by an accepted checkpoint.
- Reject rollback, wrong peer, revoked trust, changed manifest or destination,
  incompatible capability, stale terminal fact, and expired state.
- Keep resume negotiation and restored scheduling bounded under hostile input.
- Preserve connection-wide message sequencing and transport-finished binding.
- Checkpoint and terminal cleanup must remain idempotent across cancellation,
  disconnect, restart, and concurrent completion.
- Do not redefine the persisted schema, destination policy, C ABI, or UI.

## Architecture change

The record declares `none`: recovery remains within the canonical session and
transfer modules and consumes the existing storage contract.

## Acceptance criteria

- [ ] Tests resume at multiple verified offsets after reconnect and full
      process restart with identical final bytes.
- [ ] Wrong peer, role, transfer, manifest, destination, capability, session,
      offset, expiry, revocation, and replay fail closed.
- [ ] Corrupt or absent checkpoints never downgrade to unauthenticated or
      offset-zero continuation under the same transfer identity.
- [ ] Completion, rejection, cancellation, and expiry remove live authority and
      durable state exactly once.
- [ ] Benchmarks report checkpoint recovery latency and retransmitted bytes.
- [ ] `make native-test`, `make security-test`, `make benchmark`, and
      `make verify` pass.

## Verification

```bash
make native-test
make security-test
make benchmark
make verify
```
