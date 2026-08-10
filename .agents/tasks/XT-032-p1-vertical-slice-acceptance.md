---
id: XT-032
title: P1 vertical slice acceptance
initial_state: ready
workstream: integration
initial_owner: unassigned
depends_on:
  - XT-071
owned_paths:
  - docs/roadmap.md
  - docs/architecture.md
  - docs/testing.md
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-DISCOVERY
  - REQ-P1-PAIRING
  - REQ-P1-ONE-FILE-TRANSFER
  - REQ-P1-FLUTTER-FLOW
  - REQ-P1-CANCELLATION
delivery_role: acceptance
contract_changes: []
handoff: .agents/handoffs/XT-032.md
---

## Outcome

Prove the one-file vertical slice across supported desktop platforms and accept
the first P1 roadmap section only when security and cleanup gates pass.

## Context

XT-031 completes the user flow, XT-069 and XT-070 provide the authenticated
connection and real one-file network transport, and XT-071 supplies the
cross-platform E2E harness. This task accepts their immutable evidence and
updates milestone documentation; it does not invent runtime or test behavior.

## Constraints

- Test two real processes over loopback and controlled LAN interfaces.
- Cover macOS, Windows, and Linux sender/receiver combinations available in CI.
- Require the exact qualified Linux Secret Service profile; a fail-closed
  unsupported backend is correct runtime behavior but is not Linux conformance.
- Include hostile peer, cancellation, process exit, and artifact cleanup checks.
- Do not add product code, test harnesses, or CI behavior during acceptance.
- Keep every P1 production-transfer checkbox open.

## Acceptance criteria

- [ ] Automated E2E transfers verify bytes and authenticated peer identity.
- [ ] Remote CI passes native, Flutter, packaging, sanitizer, fuzz, and E2E jobs.
- [ ] Architecture/testing docs describe only implemented behavior.
- [ ] The P1 vertical-slice roadmap checkboxes close with task evidence.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
