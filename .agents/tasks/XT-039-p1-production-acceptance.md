---
id: XT-039
title: P1 production acceptance
initial_state: ready
workstream: integration
initial_owner: unassigned
depends_on:
  - XT-075
  - XT-076
owned_paths:
  - docs/roadmap.md
  - docs/architecture.md
  - docs/testing.md
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-MULTI-FILE
  - REQ-P1-SCHEDULING
  - REQ-P1-RESUME
  - REQ-P1-COLLISION
  - REQ-P1-ADVERSE-PLATFORM
delivery_role: acceptance
contract_changes: []
handoff: .agents/handoffs/XT-039.md
---

## Outcome

Close P1 only after cross-platform large-file, low-space, sleep/wake,
network-change, restart/resume, interoperability, and cleanup evidence passes.

## Context

XT-075 supplies cross-platform reliability and interoperability evidence, and
XT-076 supplies performance evidence. This task accepts their immutable
results and owns roadmap state, architecture truth, and residual-risk
reporting; it does not build product or test behavior.

## Constraints

- Exercise real packaged applications and two-process authenticated transfers.
- Include macOS, Windows, and Linux sender/receiver roles.
- Use generated or sparse data without committing large artifacts.
- Report skipped physical-network or sleep tests explicitly; skipped is not pass.
- Do not add product code, test harnesses, benchmarks, or CI behavior.

## Acceptance criteria

- [ ] The interoperability matrix covers supported cross-platform pairs.
- [ ] 20 GiB, low-space, sleep/wake, interface churn, process kill, reconnect,
  resume, cancellation, and destination-race suites pass.
- [ ] Scheduled performance baselines report throughput, CPU, RSS, fairness,
  recovery latency, and retransmitted bytes.
- [ ] Security fuzz targets and all remote CI jobs pass.
- [ ] Roadmap, architecture, and testing docs close only evidenced P1 items.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
