---
id: XT-095
title: Transport contract acceptance
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-094
owned_paths:
  - .agents/records/XT-095.json
  - .agents/tasks/XT-095-transport-contract-acceptance.md
  - .agents/handoffs/XT-095.md
delivery_plan: DP-P1-TRANSPORT-COMPOSITION
requirement_ids:
  - REQ-P1-TRANSPORT-COMPOSITION-CONTRACT
delivery_role: acceptance
contract_changes: []
handoff: .agents/handoffs/XT-095.md
---

## Outcome

Independently accept the transport-context exchange and authorized session
handoff contracts before XT-070 resumes production network composition.

## Context

XT-093 defines the wire and ADR contract for every transport-context input.
XT-094 exposes byte-identical negotiation normalization and one-shot channel
activation into `SessionAuthority`. This task consumes their immutable
integrated behavior and closes
`REQ-P1-TRANSPORT-COMPOSITION-CONTRACT`; it does not repair implementation.

## TDD contract

Use `evidence_closure` against the exact integrated candidate. Criterion
evidence must identify every required scenario, assertion, role, platform,
gate, workflow, binary, and artifact without skips or topology substitution.

## Constraints

- Do not change protocol, session, TLS, transfer, bridge, C ABI, test runner,
  workflow, or Delivery Plan behavior during acceptance.
- Reject parser-only, mocked-authority, synthetic-handle, local-only,
  one-role, skipped, stale-SHA, or generic-verify evidence.
- Require the complete native and sanitizer gates and the registered protocol
  vectors from the exact candidate.
- Return implementation defects to XT-093 or XT-094; do not patch around them
  in this task.

## Architecture change

The record declares `none` mode and changes no production boundary.

## Risk profile

The schema-v4 record binds false-acceptance risk to `native_test`,
`protocol_vectors`, `security_test`, and `verify`.

## Acceptance criteria

- [ ] Exact v1 fields, role ordering, transcript bytes, normalized negotiation,
      and ADR 0002 transport-context construction agree across both roles.
- [ ] Missing, duplicate, zero, malformed, replayed, role-swapped, tampered,
      and pre-binding transfer inputs fail closed.
- [ ] A bound channel activates exactly once into `SessionAuthority`; invalid
      activation never produces an authorized handle.
- [ ] Disconnect, revocation, explicit close, transport failure, concurrent
      stop, and destruction deactivate before TLS teardown with no late
      callback or retained authorization.
- [ ] Required Linux, macOS, and Windows evidence is complete where the plan
      declares a platform matrix.
- [ ] Complete remote CI passes for delivery and acceptance commits before the
      plan can unblock XT-070.
- [ ] All registered plan gates pass without skips.

## Verification

```bash
make native-test
python3 protocol/testdata/v1/validate_vectors.py
make security-test
make verify
```
