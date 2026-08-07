---
id: XT-037
title: Production transfer adapters
initial_state: ready
workstream: native_bridge
initial_owner: unassigned
depends_on:
  - XT-036
owned_paths:
  - native/include/xnn_transfer/c_api.h
  - native/src/bridge/**
  - native/tests/bridge/**
  - apps/desktop/lib/core/native/**
  - docs/adr/0008-p1-operation-c-abi.md
contract_changes:
  - Extend the C ABI for manifests, destination policy, rates, pause, and resume.
handoff: .agents/handoffs/XT-037.md
---

## Outcome

Expose the completed P1 production transfer behavior through additive C ABI and
Dart adapters while preserving bounded snapshots and event recovery.

## Context

XT-036 completes native multi-file, scheduling, storage policy, and resume
semantics. This task changes only the public adapter surface.

## Constraints

- Preserve ABI v1 size/version compatibility and caller-owned memory.
- Keep local destination paths, resume authorization, and storage internals out
  of peer-controlled and display event fields.
- Bound manifest pagination and snapshot recovery after dropped events.
- Commands must reject stale transfer and destination-policy handles.

## Acceptance criteria

- [ ] ABI tests cover manifests, destination choices, rate snapshots, pause,
  resume, invalid handles, overflow, restart, and cancellation.
- [ ] Dart adapters expose typed immutable copies and stable failures.
- [ ] Existing one-file clients remain source and binary compatible.
- [ ] Packaged callback tests exercise resumed multi-file events.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
