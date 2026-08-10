---
id: XT-076
title: P1 performance qualification
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-037
  - XT-075
owned_paths:
  - .agents/manifest.yaml
  - .agents/records/XT-076.json
  - .agents/tasks/XT-076-p1-performance-qualification.md
  - .agents/handoffs/XT-076.md
  - .github/workflows/ci.yml
  - tool/harness/p1_performance.py
  - tool/harness/p1_performance_test.py
  - Makefile
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-SCHEDULING
delivery_role: implementation
contract_changes:
  - Register the trusted scheduled P1 performance qualification gate.
handoff: .agents/handoffs/XT-076.md
---

## Outcome

Create repeatable scheduled qualification for transfer throughput, CPU, RSS,
fairness, recovery latency, and retransmitted bytes using the production
network and packaged native adapters.

## Context

XT-034 owns scheduler behavior and focused benchmarks. XT-037 exposes stable
production metrics, and XT-075 proves functional reliability. This task turns
those measurements into bounded cross-platform evidence before XT-039 accepts
P1.

## Constraints

- Measure production transport and adapters; do not substitute isolated codec
  or memory-copy microbenchmarks for end-to-end results.
- Run 1, 4, and 16 concurrent streams with controlled file sizes, credit, and
  receiver delay.
- Record throughput, CPU, peak RSS, fairness, recovery latency, and
  retransmitted bytes with platform and build metadata.
- Separate informational baselines from hard resource bounds and fail on
  missing, malformed, or incomparable results.
- Keep scheduled runs bounded and generated datasets out of Git.
- Register one trusted gate and emit machine-readable results without local
  paths, credentials, or identity material.
- Do not tune production code or alter scheduler semantics in this task.

## Architecture change

The record declares `none`: this adds performance qualification infrastructure,
not runtime modules or dependencies.

## Acceptance criteria

- [ ] Scheduled macOS, Windows, and Linux runs report the complete metric set
      for 1, 4, and 16 streams.
- [ ] Slow-receiver and concurrent-transfer scenarios prove bounded RSS and
      prevent one stream or peer from monopolizing connection credit.
- [ ] Reconnect/resume runs report recovery latency and retransmitted bytes.
- [ ] Harness tests reject missing metrics, invalid units, stale baselines,
      partial platform results, timeout, and child-process failure.
- [ ] `make benchmark` and `make verify` pass.

## Verification

```bash
make benchmark
make verify
```
