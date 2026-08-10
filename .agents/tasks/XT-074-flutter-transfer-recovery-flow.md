---
id: XT-074
title: Flutter transfer recovery flow
state: ready
workstream: flutter_desktop
owner: unassigned
depends_on:
  - XT-038
owned_paths:
  - .agents/records/XT-074.json
  - .agents/tasks/XT-074-flutter-transfer-recovery-flow.md
  - .agents/handoffs/XT-074.md
  - apps/desktop/lib/app/**
  - apps/desktop/lib/features/**
  - apps/desktop/test/features/**
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-SCHEDULING
  - REQ-P1-RESUME
delivery_role: implementation
contract_changes: []
handoff: .agents/handoffs/XT-074.md
---

## Outcome

Add pause, reconnect, restart recovery, resume, expiry, and revocation handling
to the desktop transfer experience without making Flutter an authority for
native identity or persisted state.

## Context

XT-038 owns multi-file selection, destination decisions, and rate display.
XT-037 exposes typed recovery adapters backed by XT-036 and XT-073. This task
adds only application orchestration and presentation for those native states.

## Constraints

- Distinguish paused, disconnected-resumable, recovering, expired, failed,
  cancelled, revoked, and completed states.
- Restore only native snapshots and opaque handles; never persist peer trust,
  offsets, paths, commitments, or authorization in Flutter.
- Require explicit user action for resume when policy requires it.
- Do not restore a transfer after trust revocation, identity loss, expiry, or a
  native mismatch.
- Make duplicate pause/resume taps, stale events, restart reconciliation,
  navigation, and disposal race-safe.
- Keep large restored manifests virtualized and keyboard accessible.
- Do not duplicate native validation, persistence, or retry policy.

## Architecture change

The record declares `none`: presentation continues to depend on application
abstractions and typed native adapters.

## Acceptance criteria

- [ ] Controller tests cover pause, disconnect, reconnect, restart snapshot
      recovery, resume, expiry, revocation, cancellation, and completion.
- [ ] Widget tests cover recovery decisions, progress/rate continuity, stale
      handles, duplicate actions, keyboard flow, and accessibility labels.
- [ ] Restarted UI reconstructs state only from bounded native snapshots.
- [ ] Revoked, expired, corrupt, or mismatched native state never appears
      resumable.
- [ ] `make flutter-test` and `make verify` pass.

## Verification

```bash
make flutter-test
make verify
```
