---
id: XT-053
title: Make commit identity policy immutable
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-052
owned_paths:
  - .agents/**
  - tool/harness/commit_message.py
  - tool/harness/commit_message_test.py
  - docs/commit-policy.md
contract_changes:
  - The activated repository identity policy is immutable
  - Hooks trust committed policy instead of mutable working-tree content
handoff: .agents/handoffs/XT-053.md
---

## Outcome

Reject attempts to change `.agents/commit-identity.json` after its activation,
even when the same commit uses the proposed replacement identity.

## Context

XT-052 added repository identity checks, but both hook and range validation
read the policy from the content being committed. That lets a policy-changing
commit authorize its own identity. This task pins trust to the committed
parent for hooks and the first activation commit for range validation.

## Constraints

- The activation commit remains valid and defines the permanent identity.
- Hooks use `HEAD:.agents/commit-identity.json` once that path exists.
- Range checks reject modification, deletion, or re-addition after activation.
- Bootstrap reads committed policy when available and working content only for
  the initial activation.
- Preserve pre-activation history and existing message-policy behavior.
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

- [ ] A working-tree policy replacement cannot authorize a wrong local identity.
- [ ] A policy replacement committed with `--no-verify` fails range validation.
- [ ] Bootstrap continues to repair local identity from committed policy.
- [ ] The original activation commit and all existing valid history pass.
- [ ] Repository verification passes.

## Verification

```bash
make commit-message-test
make verify
```
