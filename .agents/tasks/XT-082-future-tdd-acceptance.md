---
id: XT-082
title: Future tdd acceptance
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-081
owned_paths:
  - .agents/records/XT-082.json
  - .agents/tasks/XT-082-future-tdd-acceptance.md
  - .agents/handoffs/XT-082.md
  - docs/testing.md
delivery_plan: DP-FUTURE-TDD
requirement_ids:
  - REQ-FUTURE-TDD-CONTRACT
  - REQ-FUTURE-TDD-PROOF
  - REQ-FUTURE-TDD-EVIDENCE
  - REQ-FUTURE-TDD-ADOPTION
delivery_role: acceptance
contract_changes: []
handoff: .agents/handoffs/XT-082.md
---

## Outcome

Independently prove the future-task TDD contract, deterministic checkpoint,
criterion evidence closure, and XT-083 activation boundary before declaring the
governance plan accepted.

## Context

XT-078 through XT-081 implement the four requirements in `DP-FUTURE-TDD`.
This acceptance task consumes their immutable integrated behavior and updates
testing documentation only. It must not repair the implementation, add
fixtures, or weaken the approved plan while evaluating it.

## Constraints

- Do not add or modify harness implementation, test runners, task generation,
  workflow behavior, gates, or product code during acceptance.
- Execute adversarial fixtures against the exact integrated candidate and treat
  every skipped required gate as failure.
- Verify both sides of the compatibility boundary: pre-XT-083 records remain
  valid, while an invalid XT-083-or-later fixture fails claim and review.
- Confirm criterion evidence binds the exact candidate SHA and cannot be
  satisfied by dependency state, generic verification, stale artifacts, mocks,
  or partial matrices.
- Return any implementation defect to its owning task; do not patch around it
  in this task.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] Delivery Plan schema-v2 and record schema-v4 positive and negative
      fixtures prove stable criteria, negative definitions, proof modes, and
      evidence contracts.
- [ ] A fixture feature task proves base pass, test-only Red failure, frozen
      proof surface, reviewed-head pass, and generated immutable proof.
- [ ] Adversarial fixtures reject production-before-Red, reverted production
      history, mutable tests, unrelated and infrastructure failures, skipped
      tests, untrusted commands, stale governance, and fabricated proof.
- [ ] Evidence fixtures reject stale source or binary SHA, missing or skipped
      jobs, partial matrices, fake E2E topology, altered artifacts, and generic
      verification substituted for specialized evidence.
- [ ] Every existing pre-XT-083 plan and task record validates unchanged, and a
      noncompliant XT-083 fixture cannot be claimed, reviewed, integrated, or
      accepted.
- [ ] Documentation states only the implemented future workflow and explicitly
      disclaims retroactive TDD compliance.
- [ ] Complete remote CI passes for the integrated delivery and acceptance
      commits before the protected integration branch advances.
- [ ] `make delivery-plan-test`, `make governance-test`, and `make verify` pass.

## Verification

```bash
make delivery-plan-test
make governance-test
make verify
```
