---
id: XT-078
title: Future tdd contract governance
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-077
owned_paths:
  - .agents/records/XT-078.json
  - .agents/tasks/XT-078-future-tdd-contract-governance.md
  - .agents/handoffs/XT-078.md
  - tool/harness/delivery_plan.py
  - tool/harness/delivery_plan_test.py
  - tool/harness/governance.py
  - tool/harness/governance_test.sh
  - tool/harness/tdd_contract.py
  - tool/harness/tdd_contract_test.py
  - docs/adr/0016-future-tdd-governance.md
delivery_plan: DP-FUTURE-TDD
requirement_ids:
  - REQ-FUTURE-TDD-CONTRACT
  - REQ-FUTURE-TDD-ADOPTION
delivery_role: implementation
contract_changes:
  - Add Delivery Plan schema version 2 criterion and evidence contracts.
  - Add task-record schema version 4 test contracts for future governed tasks.
handoff: .agents/handoffs/XT-078.md
---

## Outcome

Introduce versioned Delivery Plan criterion and task-record test contracts that
are mandatory beginning with XT-083, while every earlier accepted, active, and
already-planned task remains valid without fabricated TDD history.

## Context

ADR 0014 and XT-063 established Delivery Plan schema version 1 and requirement
closure. XT-054 through XT-058 established typed bugfix contracts and
deterministic regression proof, but features and other task types do not bind
criteria to a pre-implementation test contract. `DP-FUTURE-TDD` is a schema-v1
bootstrap plan; this task records the durable schema-v2 and record-v4 decision
before XT-079 implements checkpoint execution.

## Constraints

- Add stable criterion IDs, negative definitions, evidence contracts, and
  task-type proof modes without adding another task dependency graph.
- Keep task dependencies authoritative in the backlog and runtime state
  authoritative in records and task branches.
- Make schema version 4 and structured criteria mandatory only from XT-083.
- Preserve validation of every pre-XT-083 record and approved plan byte-for-byte;
  do not synthesize criterion IDs or TDD evidence for historical tasks.
- Keep commands out of task-authored test contracts; contracts reference trusted
  gate IDs from `.agents/manifest.yaml`.
- Define the compatibility and approval decision in ADR 0016. Do not implement
  Red checkpoint execution, remote evidence collection, or task generation in
  this task.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] Delivery Plan schema version 2 represents stable criterion IDs,
      statements, negative definitions, implementation mappings, and evidence
      contracts while retaining one acceptance owner and dependency closure.
- [ ] Task-record schema version 4 validates a task-type-appropriate
      `test_contract`, criterion references, proof surface, trusted gate, and
      no-skip policy for governed future tasks.
- [ ] Approval digests bind the complete criterion and evidence semantics; a
      changed negative definition or evidence contract invalidates approval.
- [ ] Unknown criteria, duplicate IDs, missing negative definitions, orphan
      task mappings, untrusted gates, and invalid proof modes fail focused
      governance fixtures.
- [ ] Every existing schema-v1 plan and schema-v1/v2/v3 record continues to
      validate unchanged, and no task before XT-083 is required to claim TDD.
- [ ] ADR 0016 documents the bootstrap boundary, proof/evidence split, schema
      compatibility, and future-only activation.
- [ ] `make delivery-plan-test`, `make governance-test`, and `make verify` pass.

## Verification

```bash
make delivery-plan-test
make governance-test
make verify
```
