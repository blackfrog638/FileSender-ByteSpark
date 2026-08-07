---
id: XT-052
title: Enforce repository commit identity
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-047
owned_paths:
  - .agents/**
  - .githooks/**
  - tool/harness/bootstrap.sh
  - tool/harness/commit_message.py
  - tool/harness/commit_message_test.py
  - tool/harness/governance_test.sh
  - docs/commit-policy.md
contract_changes:
  - New commits must use the versioned repository author and committer identity
  - Bootstrap repairs repository-local identity and installs the commit hook
handoff: .agents/handoffs/XT-052.md
---

## Outcome

Reject commits whose author or committer differs from the versioned
`blackfrog638 <blackfrog638@gmail.com>` repository identity.

## Context

XT-047 validates message quality but does not validate commit attribution.
Independent clones repeatedly inherited the unrelated global
`chenzhuoran <chenzhuoran.638@bytedance.com>` identity because a local config
only protects one clone. This task extends the existing commit policy so
bootstrap repairs each clone, local hooks fail immediately, and the range gate
still rejects `--no-verify` bypasses.

## Constraints

- Store the expected name and email in one machine-readable versioned file.
- Validate both author and committer; do not accept a correct author with an
  incorrect committer.
- Activate identity range validation only when the policy file is introduced,
  preserving older multi-contributor history.
- Read Git identities through Git's structured formatting and `git var`; do
  not parse human-oriented `git log` output.
- Bootstrap must install `.githooks` and restore repository-local identity.
- Keep global Git identity untouched because other repositories use it.
- Add negative tests for wrong author, wrong committer, malformed policy, and
  `--no-verify` range validation.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 2 risk dimension in the task record. Every
non-none risk must name commands that also appear in `verification.commands`.

## Acceptance criteria

- [ ] A normal commit with incorrect local identity is rejected by the hook.
- [ ] A wrong-identity commit created with `--no-verify` is rejected by
      `make commit-message-test`.
- [ ] Correct author and committer identities pass both hook and range checks.
- [ ] Commits before policy activation remain valid.
- [ ] Bootstrap writes the versioned identity into repository-local config.
- [ ] The commit policy documents configuration and enforcement boundaries.
- [ ] Repository verification passes.

## Verification

```bash
make commit-message-test
make verify
```
