---
id: XT-055
title: Deterministic defect proof
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-054
owned_paths:
  - AGENTS.md
  - .agents/records/README.md
  - .agents/tasks/TASK_TEMPLATE.md
  - docs/adr/0011-defect-task-governance.md
  - tool/harness/agent.sh
  - tool/harness/governance.py
  - tool/harness/governance_test.sh
  - tool/harness/defect_proof.py
  - tool/harness/defect_proof_test.py
contract_changes:
  - Deterministic bugfix review requires failure-at-reproduction and
    pass-at-head evidence from one trusted gate
  - Review records bind generated defect proof metadata to both revisions
handoff: .agents/handoffs/XT-055.md
---

## Outcome

Reject deterministic bugfix review unless the trusted regression gate fails at
the declared reproduction commit and passes at the reviewed head.

## Context

XT-054 added schema version 3 defect metadata but deliberately did not execute
the declared proof. This task implements the deterministic proof policy in ADR
0011 and binds generated evidence into `verification.defect_proof`.

## Constraints

- Resolve the command only from the reviewed head's trusted gate registry.
- Execute the exact same command in a detached reproduction worktree and the
  current clean task worktree.
- Require reproduction to be within `base_sha..head` and reject unavailable or
  unrelated commits.
- Treat any passing reproduction gate or failing head gate as review failure.
- Bind gate ID, command SHA-256, proof mode, both commits, both exit codes, and
  timestamp into the generated record.
- Restore the pre-proof task record if proof, verification, or review
  validation fails.
- Support only `deterministic` in this task. Fail closed for `sanitizer`,
  `stress`, `platform_ci`, and `manual` until mode-specific evidence policy is
  implemented.
- Remove temporary worktrees even when the gate fails or is interrupted.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] A failing reproduction and passing head generate bound proof evidence.
- [ ] A passing reproduction, failing head, unrelated reproduction, changed
      command hash, or forged exit code is rejected.
- [ ] A missing proof cannot enter review, and a failed transition restores the
      original in-progress record.
- [ ] Non-bugfix tasks retain the existing review path without proof work.
- [ ] Unsupported proof modes fail with an explicit message.
- [ ] Temporary reproduction worktrees are removed on every tested path.
- [ ] ADR 0011 and task documentation define deterministic proof semantics.
- [ ] Repository verification passes.

## Verification

```bash
make governance-test
make verify
```
