---
id: XT-072
title: Multi file transfer orchestration
state: ready
workstream: native_core
owner: unassigned
depends_on:
  - XT-033
owned_paths:
  - .agents/records/XT-072.json
  - .agents/tasks/XT-072-multi-file-transfer-orchestration.md
  - .agents/handoffs/XT-072.md
  - native/include/xnn_transfer/core/transfer/**
  - native/src/transfer/**
  - native/tests/transfer/**
  - native/benchmarks/**
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-MULTI-FILE
delivery_role: implementation
contract_changes: []
handoff: .agents/handoffs/XT-072.md
---

## Outcome

Extend the transfer engine from one file to bounded manifest and directory
orchestration while preserving per-entry integrity, ordering, cancellation,
and deterministic partial terminal results.

## Context

XT-033 owns complete manifest validation and multi-entry storage transactions.
This task consumes that contract inside the transfer engine. Scheduling,
destination policy, persistence, public adapters, and Flutter remain in later
tasks.

## Constraints

- Reject an invalid complete manifest before opening transfer streams.
- Preserve the canonical manifest commitment and deterministic entry order.
- Bind each stream to exactly one validated entry and storage transaction.
- Commit files only after their individual integrity checks and report the
  bounded manifest-level partial result.
- Cancellation and connection-fatal errors must close every stream and abort
  every uncommitted entry idempotently.
- Keep one-file wire behavior compatible.
- Do not implement fairness scheduling, destination choice, resume, C ABI, or
  presentation behavior.

## Architecture change

The record declares `none`: multi-file orchestration extends the canonical
transfer module and consumes the existing storage contract.

## Acceptance criteria

- [ ] Native loopback tests transfer files, nested directories, empty
      directories, and mixed-size manifests with exact commitments and bytes.
- [ ] Duplicate entries, parent conflicts, entry failure, short I/O, integrity
      failure, cancellation, and disconnect produce deterministic bounded
      results and cleanup.
- [ ] One-file golden behavior and protocol compatibility remain unchanged.
- [ ] Benchmarks bound manifest memory and per-entry orchestration overhead.
- [ ] `make native-test`, `make security-test`, `make benchmark`, and
      `make verify` pass.

## Verification

```bash
make native-test
make security-test
make benchmark
make verify
```
