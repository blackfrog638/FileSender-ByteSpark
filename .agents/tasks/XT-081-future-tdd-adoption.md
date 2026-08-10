---
id: XT-081
title: Future tdd adoption
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-078
  - XT-079
  - XT-080
owned_paths:
  - .agents/records/XT-081.json
  - .agents/tasks/XT-081-future-tdd-adoption.md
  - .agents/handoffs/XT-081.md
  - AGENTS.md
  - .agents/manifest.yaml
  - .agents/tasks/TASK_TEMPLATE.md
  - .agents/plans/README.md
  - docs/testing.md
  - tool/harness/agent.sh
  - tool/harness/governance.py
  - tool/harness/governance_test.sh
  - tool/harness/new_task.sh
  - tool/harness/task_conflicts.py
  - tool/harness/task_conflicts_test.py
  - Makefile
delivery_plan: DP-FUTURE-TDD
requirement_ids:
  - REQ-FUTURE-TDD-CONTRACT
  - REQ-FUTURE-TDD-PROOF
  - REQ-FUTURE-TDD-EVIDENCE
  - REQ-FUTURE-TDD-ADOPTION
delivery_role: implementation
contract_changes:
  - Activate mandatory future-task TDD governance beginning with XT-083.
handoff: .agents/handoffs/XT-081.md
---

## Outcome

Activate the reviewed TDD contract for tasks at or after XT-083 across task
generation, prompts, claim, stale-base detection, review, integration, and
acceptance without changing earlier task provenance.

## Context

XT-078 defines the versioned contracts, XT-079 implements chronological proof,
and XT-080 implements criterion evidence closure. This task connects those
capabilities to the normal agent workflow and documents the future-only
activation boundary. XT-082 independently accepts the complete governance
behavior.

## Constraints

- Set the mechanical activation threshold to XT-083. Do not infer activation
  from dates, branches, titles, or task authors.
- Generate complete schema-v4 records and criterion bindings for future tasks;
  generated placeholders must remain unclaimable.
- Prompts must include approved criteria, negative definitions, proof mode,
  focused gates, and allowed Red paths before implementation begins.
- Claims require an approved plan and complete test contract, but do not execute
  expensive gates merely to create a worktree.
- Stale checks must include the relevant plan, backlog registration, task spec,
  manifest, workflow, and proof/evidence runners.
- Keep completed, active, and planned XT-001 through XT-082 records valid and
  unchanged. Compatibility readers cannot waive requirements for XT-083 onward.
- Do not introduce another scheduler, runtime state, or product dependency.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] The manifest records XT-083 as the first TDD-governed task and governance
      validates the threshold as an immutable compatibility boundary.
- [ ] `new_task.sh` and `TASK_TEMPLATE.md` generate complete criterion and
      test-contract structures for every supported task and delivery role.
- [ ] Claim rejects missing criteria, placeholder proof modes, untrusted focused
      gates, invalid Red paths, and plans whose evidence owner is incomplete.
- [ ] `agent.sh prompt` prints the exact approved criteria, negative definitions,
      proof mode, focused gates, and Red path scope for the claimed task.
- [ ] Review, integration, and acceptance verify the checkpoint and criterion
      evidence digests without weakening existing payload immutability or remote
      CI publication.
- [ ] Relevant upstream governance changes force rebase and fresh proof, while
      unrelated product changes may remain parallel.
- [ ] Negative fixtures prove legacy compatibility cannot be used to create an
      ungoverned XT-083-or-later task.
- [ ] AGENTS.md, plan documentation, and testing documentation describe the
      enforced workflow and clearly exclude retroactive compliance claims.
- [ ] `make delivery-plan-test`, `make governance-test`, and `make verify` pass.

## Verification

```bash
make delivery-plan-test
make governance-test
make verify
```
