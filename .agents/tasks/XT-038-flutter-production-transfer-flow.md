---
id: XT-038
title: Flutter production transfer flow
initial_state: ready
workstream: flutter_desktop
initial_owner: unassigned
depends_on:
  - XT-037
owned_paths:
  - apps/desktop/lib/app/**
  - apps/desktop/lib/features/**
  - apps/desktop/test/features/**
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-MULTI-FILE
  - REQ-P1-SCHEDULING
  - REQ-P1-RESUME
  - REQ-P1-COLLISION
delivery_role: implementation
contract_changes: []
handoff: .agents/handoffs/XT-038.md
---

## Outcome

Add multi-file/directory selection, destination/collision decisions, rate
reporting, pause, restart recovery, and resume to the desktop experience.

## Context

XT-037 provides typed production adapters. Flutter remains an application and
presentation client of native policy and never validates hostile wire data.

## Constraints

- Require explicit destination and collision decisions.
- Distinguish paused, disconnected-resumable, expired, failed, cancelled, and
  completed terminal states.
- Do not infer trust from names or restore revoked/identity-lost sessions.
- Keep large manifests virtualized and commands race-safe.

## Acceptance criteria

- [ ] Controller and widget tests cover multi-file send/receive, collision
  prompts, rate updates, pause, reconnect, restart, resume, expiry, and revoke.
- [ ] Large manifests do not block rendering or duplicate commands.
- [ ] Accessibility and keyboard navigation cover all decision dialogs.
- [ ] UI claims match the completed native behavior exactly.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
