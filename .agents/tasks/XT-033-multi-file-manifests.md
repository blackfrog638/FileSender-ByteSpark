---
id: XT-033
title: Multi file manifest storage
initial_state: ready
workstream: native_core
initial_owner: unassigned
depends_on:
  - XT-032
owned_paths:
  - native/include/xnn_transfer/core/storage/**
  - native/src/storage/**
  - native/tests/storage/**
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-MULTI-FILE
delivery_role: implementation
contract_changes: []
handoff: .agents/handoffs/XT-033.md
---

## Outcome

Extend validated storage state from one file to bounded multi-file manifests
and directory transactions without weakening containment or atomicity.

## Context

XT-032 accepts the one-file slice. Protocol v1 and XT-011 already define
manifest limits, path normalization, ordering, and commitment behavior.
XT-072 separately owns transfer-engine orchestration after this storage
contract is complete.

## Constraints

- Validate the complete manifest before creating destination artifacts.
- Enforce entry/count/size/depth/path and checked-sum limits.
- Detect platform-normalized collisions, parent conflicts, and unsafe links.
- Commit each file only after hash verification; report partial terminal state.
- Do not add transfer scheduling, wire dispatch, C ABI, or presentation code.

## Acceptance criteria

- [ ] All XT-011 multi-entry vectors drive native tests.
- [ ] Empty directories, nested files, duplicates, collisions, overflow, and
  partial I/O failure have deterministic behavior.
- [ ] Storage transaction tests preserve manifest commitment, verified bytes,
      empty directories, and deterministic partial terminal state.
- [ ] `make security-test` passes.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
