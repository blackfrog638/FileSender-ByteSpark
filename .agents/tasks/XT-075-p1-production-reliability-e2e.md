---
id: XT-075
title: P1 production reliability e2e
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-074
owned_paths:
  - .agents/manifest.yaml
  - .agents/records/XT-075.json
  - .agents/tasks/XT-075-p1-production-reliability-e2e.md
  - .agents/handoffs/XT-075.md
  - .github/workflows/ci.yml
  - apps/desktop/integration_test/**
  - apps/desktop/test_driver/**
  - tool/harness/p1_production_e2e.py
  - tool/harness/p1_production_e2e_test.py
  - Makefile
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-MULTI-FILE
  - REQ-P1-SCHEDULING
  - REQ-P1-RESUME
  - REQ-P1-COLLISION
  - REQ-P1-ADVERSE-PLATFORM
delivery_role: implementation
contract_changes:
  - Register the trusted P1 production reliability and interoperability E2E gate.
handoff: .agents/handoffs/XT-075.md
---

## Outcome

Extend the packaged-application E2E harness with cross-platform multi-file,
low-space, sleep/wake, interface-change, process-kill, restart/resume,
collision-race, and cleanup qualification.

## Context

XT-071 establishes the vertical-slice harness. XT-033 through XT-038 and
XT-072 through XT-074 complete production behavior. XT-039 must remain a pure
acceptance task, so the adverse-platform and interoperability suites live here.

## Constraints

- Exercise packaged production applications and real authenticated transport.
- Cover macOS, Windows, and qualified Linux in both sender and receiver roles.
- Generate or sparsely allocate large inputs without committing large files.
- Inject low space, process kill, interface churn, delayed I/O, destination
  races, and restart at deterministic synchronization points.
- Verify committed bytes, native identity, checkpoint behavior, existing-file
  preservation, and complete temporary-state cleanup.
- Treat skipped physical sleep/network scenarios as missing evidence, not pass.
- Register one trusted gate and keep diagnostics bounded and secret-free.
- Do not change product behavior to satisfy the harness.

## Architecture change

The record declares `none`: this extends integration tests and CI without
changing runtime architecture.

## Acceptance criteria

- [ ] The interoperability matrix covers supported sender/receiver platform
      pairs for multi-file, destination, cancel, reconnect, and resume.
- [ ] Automated suites cover 20 GiB sparse input, low space, sleep/wake,
      interface churn, process kill, restart, resume, and destination races.
- [ ] Every failure path proves no unintended overwrite, committed partial
      file, stale checkpoint, temporary artifact, socket, or child process.
- [ ] Harness tests cover unsupported injection, timeout, crash, stale output,
      cleanup failure, and incomplete platform matrices.
- [ ] `make security-test` and `make verify` pass.

## Verification

```bash
make security-test
make verify
```
