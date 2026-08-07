---
id: XT-053
title: Immutable corrected commit identity
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-052
owned_paths:
  - .agents/**
  - tool/harness/commit_message.py
  - tool/harness/commit_message_test.py
  - tool/harness/governance_test.sh
  - docs/commit-policy.md
contract_changes:
  - The canonical repository identity is blackfrog638
    <blackfrog638@gmail.com>
  - The schema v2 repository identity policy is immutable after activation
  - Hooks trust committed policy instead of mutable working-tree content
handoff: .agents/handoffs/XT-053.md
---

## Outcome

Restore `blackfrog638 <blackfrog638@gmail.com>` as the canonical
repository identity, then reject attempts to change the schema v2 policy after
its activation.

## Context

XT-052 added repository identity checks but encoded
`chenzhuoran <chenzhuoran.638@bytedance.com>` as canonical, reversing the previously
confirmed repository identity. The current implementation also lets a
policy-changing commit authorize itself because hook and range validation read
the proposed content. This corrected plan restores the intended identity
before claim, then makes the task delivery upgrade that policy to an immutable
schema v2 trust anchor.

## Constraints

- Schema v1 history remains valid and is evaluated against the policy stored in
  each historical commit.
- The first schema v2 policy commit defines the permanent identity.
- Hooks use `HEAD:.agents/commit-identity.json` once schema v2 is active.
- Range checks reject modification, deletion, or re-addition after schema v2
  activation.
- Bootstrap reads committed policy when available and working content only for
  the schema v2 activation.
- Preserve pre-activation history and existing message-policy behavior.
- Resolve inherited trusted gates from the manifest in isolated lifecycle
  fixtures instead of maintaining a partial command map.
- Add actual Git hook and `--no-verify` range bypass fixtures.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 2 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] The schema v2 policy names `blackfrog638
      <blackfrog638@gmail.com>`.
- [ ] A working-tree policy replacement cannot authorize a wrong local identity.
- [ ] A policy replacement committed with `--no-verify` fails range validation.
- [ ] Bootstrap continues to repair local identity from committed policy.
- [ ] Schema v1 history and the schema v2 activation commit pass.
- [ ] Governance fixtures accept inherited records with any registered gate.
- [ ] Repository verification passes.

## Verification

```bash
make commit-message-test
make verify
```
