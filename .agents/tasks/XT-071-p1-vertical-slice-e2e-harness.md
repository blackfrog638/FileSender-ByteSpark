---
id: XT-071
title: P1 vertical slice e2e harness
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-031
  - XT-070
owned_paths:
  - .agents/manifest.yaml
  - .agents/records/XT-071.json
  - .agents/tasks/XT-071-p1-vertical-slice-e2e-harness.md
  - .agents/handoffs/XT-071.md
  - .github/workflows/ci.yml
  - apps/desktop/integration_test/**
  - apps/desktop/test_driver/**
  - tool/harness/p1_vertical_slice_e2e.py
  - tool/harness/p1_vertical_slice_e2e_test.py
  - Makefile
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-DISCOVERY
  - REQ-P1-PAIRING
  - REQ-P1-ONE-FILE-TRANSFER
  - REQ-P1-FLUTTER-FLOW
  - REQ-P1-CANCELLATION
delivery_role: implementation
contract_changes:
  - Register the trusted cross-platform P1 vertical-slice E2E gate.
handoff: .agents/handoffs/XT-071.md
---

## Outcome

Create a deterministic cross-platform E2E harness that drives two packaged
desktop processes through discovery, pairing, one-file offer acceptance,
transfer progress, byte verification, cancellation, and cleanup.

## Context

XT-031 supplies the Flutter flow and XT-070 supplies real authenticated
networking. XT-032 must remain an evidence-only acceptance task, so reusable
test orchestration and CI wiring are implemented here.

## Constraints

- Exercise packaged applications and production native libraries; do not call
  transfer state machines or fake gateways directly.
- Use isolated temporary identities, receive roots, ports, and files for every
  run without committing generated artifacts.
- Bound process startup, discovery, pairing, transfer, cancellation, and
  shutdown waits with deterministic diagnostics.
- Cover sender and receiver roles on macOS, Windows, and qualified Linux CI.
- Treat unavailable required platform services, skipped jobs, stale artifacts,
  and partial matrices as failures.
- Register one trusted gate in `.agents/manifest.yaml`; task-authored shell
  outside that gate is not acceptance evidence.
- Do not change product behavior to accommodate the harness.

## Architecture change

The record declares `none`: this adds test infrastructure and CI coverage, not
runtime modules or dependency directions.

## Acceptance criteria

- [ ] The harness launches two packaged processes and verifies authenticated
      identity plus exact destination bytes after explicit acceptance.
- [ ] Negative runs cover rejection, cancellation, process exit, hostile peer,
      integrity failure, and temporary-artifact cleanup.
- [ ] CI runs the trusted gate across supported sender/receiver roles and
      retains bounded failure diagnostics.
- [ ] Harness unit tests cover timeout, child crash, port collision, stale
      output, skipped platform service, and cleanup failure.
- [ ] `make flutter-test`, `make security-test`, and `make verify` pass.

## Verification

```bash
make flutter-test
make security-test
make verify
```
