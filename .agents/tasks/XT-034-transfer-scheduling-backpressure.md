---
id: XT-034
title: Transfer scheduling backpressure
initial_state: ready
workstream: native_core
initial_owner: unassigned
depends_on:
  - XT-033
owned_paths:
  - native/include/xnn_transfer/core/transfer/**
  - native/src/transfer/**
  - native/tests/transfer/**
  - native/benchmarks/**
contract_changes: []
handoff: .agents/handoffs/XT-034.md
---

## Outcome

Implement bounded chunk scheduling, connection/transfer backpressure, fairness,
and stable rate reporting for concurrent files and transfers.

## Context

Protocol v1 defines credit and unacknowledged-byte limits. XT-033 supplies
multi-file state and manifest ordering.

## Constraints

- Never exceed negotiated per-transfer or connection credit.
- Use checked arithmetic and bounded queues independent of advertised file size.
- Prevent one slow transfer from starving unrelated accepted transfers.
- Rate reporting must use monotonic time and remain presentation-only.

## Acceptance criteria

- [ ] Deterministic scheduler tests cover credit exhaustion, partial ACKs,
  unfair peers, cancellation, disconnect, and counter wrap boundaries.
- [ ] Benchmarks report 1/4/16 stream throughput, fairness, CPU, and RSS.
- [ ] Memory remains bounded under slow receiver and tiny ACK patterns.
- [ ] `make security-test` passes.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
