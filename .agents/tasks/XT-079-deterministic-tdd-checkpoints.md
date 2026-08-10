---
id: XT-079
title: Deterministic tdd checkpoints
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-078
owned_paths:
  - .agents/records/XT-079.json
  - .agents/tasks/XT-079-deterministic-tdd-checkpoints.md
  - .agents/handoffs/XT-079.md
  - .agents/manifest.yaml
  - tool/harness/agent.sh
  - tool/harness/governance.py
  - tool/harness/governance_test.sh
  - tool/harness/task_conflicts.py
  - tool/harness/task_conflicts_test.py
  - tool/harness/tdd_proof.py
  - tool/harness/tdd_proof_test.py
  - Makefile
delivery_plan: DP-FUTURE-TDD
requirement_ids:
  - REQ-FUTURE-TDD-PROOF
  - REQ-FUTURE-TDD-ADOPTION
delivery_role: implementation
contract_changes:
  - Add the trusted Red checkpoint command and generated TDD proof contract.
handoff: .agents/handoffs/XT-079.md
---

## Outcome

Add a trusted Red checkpoint and review proof that mechanically establishes
base-pass, attributed pre-implementation failure, and reviewed-head success for
future governed tasks.

## Context

XT-055 and XT-058 already replay deterministic bugfix regressions at base,
reproduction, and head revisions. XT-078 generalizes the contract vocabulary to
features, refactors, tests, governance, investigations, and acceptance work.
This task implements the chronological proof and path controls; XT-080 separately
binds final criterion evidence to remote CI.

## Constraints

- Add `agent.sh checkpoint XT-NNN red` without adding a new lifecycle state.
- Resolve the focused command only from the integration-owned trusted gate
  registry and reject task-authored shell.
- Inspect every commit from task base through the Red revision, not only the
  aggregate diff, so added-then-reverted production changes are still detected.
- Allow only declared test, fixture, scenario, test-registration, and task-record
  paths before Red; require explicit review of test-registration changes.
- Reject command-not-found, missing tools, timeout, crash, skip, and unrelated
  failures as Red evidence.
- Freeze criterion mapping, gate digest, failure fingerprint, and proof-surface
  blobs. A later oracle change invalidates the checkpoint.
- Run detached proof worktrees with bounded cleanup on success, failure, and
  interruption. Preserve the existing bugfix proof and task lifecycle.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] The checkpoint runner proves the focused gate passes at base and fails at
      Red with every declared exact fingerprint before recording the checkpoint.
- [ ] Review replays the same trusted gate at base, Red, and head and generates
      immutable `verification.tdd_proof`; task-authored proof fields are rejected.
- [ ] A production-path commit before Red, even if reverted later, fails proof.
- [ ] Deleting, changing, or weakening a frozen test, fixture, scenario, or
      oracle after Red fails review until a fresh checkpoint is recorded.
- [ ] Feature and governance Red-Green, bugfix regression, refactor
      characterization, and test sentinel modes have deterministic fixtures;
      investigations and acceptance tasks cannot manufacture product proof.
- [ ] Infrastructure failures, skipped tests, unregistered commands, unrelated
      fingerprints, non-ancestor checkpoints, and dirty proof runs fail closed.
- [ ] Relevant plan, task-spec, manifest, workflow, or proof-runner changes make
      the governed task stale and require a new checkpoint and review.
- [ ] Existing bugfix proof and all pre-XT-083 task lifecycles remain valid.
- [ ] `make governance-test` and `make verify` pass.

## Verification

```bash
make governance-test
make verify
```
