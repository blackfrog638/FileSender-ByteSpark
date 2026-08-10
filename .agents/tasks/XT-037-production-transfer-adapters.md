---
id: XT-037
title: Production transfer adapters
initial_state: ready
workstream: native_bridge
initial_owner: unassigned
depends_on:
  - XT-073
owned_paths:
  - native/include/xnn_transfer/c_api.h
  - native/src/bridge/**
  - native/tests/bridge/**
  - apps/desktop/lib/core/native/**
  - docs/adr/0008-p1-operation-c-abi.md
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-MULTI-FILE
  - REQ-P1-SCHEDULING
  - REQ-P1-RESUME
  - REQ-P1-COLLISION
delivery_role: implementation
contract_changes:
  - Extend the C ABI for manifests, destination policy, rates, pause, and resume.
handoff: .agents/handoffs/XT-037.md
---

## Outcome

Expose the completed P1 production transfer behavior through additive C ABI and
Dart adapters while preserving bounded snapshots and event recovery.

## Context

XT-033 through XT-036, XT-072, and XT-073 complete native multi-file,
scheduling, storage policy, persistence, and recovery semantics. XT-069 and
XT-070 already own the production connection and network transport. This task
changes only the additive public adapter surface.

## Constraints

- Preserve ABI v1 size/version compatibility and caller-owned memory.
- Keep local destination paths, resume authorization, and storage internals out
  of peer-controlled and display event fields.
- Bound manifest pagination and snapshot recovery after dropped events.
- Commands must reject stale transfer and destination-policy handles.
- Do not create sockets, TLS sessions, filesystem workers, or frame dispatch.

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
