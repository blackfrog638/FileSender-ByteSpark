---
id: XT-068
title: Remote CI acceptance
state: ready
workstream: integration
owner: unassigned
depends_on: []
owned_paths:
  - AGENTS.md
  - .agents/backlog.yaml
  - .agents/manifest.yaml
  - .agents/plans/DP-REMOTE-ACCEPTANCE.json
  - .agents/records/README.md
  - .agents/records/XT-068.json
  - .agents/tasks/XT-068-remote-ci-acceptance.md
  - .agents/handoffs/XT-068.md
  - .github/workflows/ci.yml
  - tool/harness/agent.sh
  - tool/harness/github_ci.py
  - tool/harness/github_ci_test.py
  - tool/harness/governance.py
  - tool/harness/governance_test.sh
  - Makefile
  - docs/adr/0015-remote-ci-acceptance.md
delivery_plan: DP-REMOTE-ACCEPTANCE
requirement_ids:
  - REQ-REMOTE-ACCEPTANCE
delivery_role: implementation_acceptance
contract_changes: []
handoff: .agents/handoffs/XT-068.md
---

## Outcome

A task reaches `done` only after the integrated delivery and generated
acceptance commit both pass the complete GitHub Actions workflow, and the
protected `harness` branch advances to the exact verified acceptance SHA.

## Context

`agent.sh accept` currently treats local verification as sufficient, records a
`local:*` reference, and leaves the integration branch unpushed. GitHub Actions
therefore does not participate in the state transition even when task
specifications require cross-platform evidence. ADR 0015 defines the remote
candidate and protected-branch publication sequence that closes this gap.

## Constraints

- Stage each immutable SHA on `ci/XT-NNN`; never weaken `harness` protection or
  push an unverified SHA directly to the integration branch.
- Resolve GitHub credentials through the configured Git credential helper.
  Never print, persist, or pass credentials in command arguments.
- Bind acceptance evidence to the integrated delivery SHA and a successful
  GitHub Actions workflow URL.
- Treat missing, cancelled, timed-out, skipped, stale, or failed workflows as
  rejection. Keep the task `integrated` and leave `harness` unchanged.
- Make interrupted publication retryable without duplicating delivery or
  acceptance commits.
- Delete ephemeral CI refs after success or failure without hiding the primary
  failure.
- Preserve the existing squash/cherry-pick provenance, commit identity, task
  ownership, and local verification contracts.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] `agent.sh accept` runs local verification, stages the integrated SHA,
      waits for successful remote CI, and records the resulting GitHub Actions
      URL before creating the acceptance commit.
- [ ] The acceptance commit passes the same remote workflow before `harness`
      advances and the scheduler cache becomes `done`.
- [ ] Failed or unavailable remote CI leaves the durable task state
      `integrated`, leaves `harness` unchanged, and supports a clean retry.
- [ ] CI polling has deterministic unit tests for delayed discovery, success,
      terminal failure, timeout, authentication failure, and SHA/ref mismatch.
- [ ] GitHub protects `harness` from force pushes, deletion, and every commit
      missing a required workflow check.
- [ ] ADR 0015, `AGENTS.md`, and the record documentation describe the enforced
      remote acceptance contract.
- [ ] `make governance-test` and `make verify` pass.

## Verification

```bash
make governance-test
make verify
```
