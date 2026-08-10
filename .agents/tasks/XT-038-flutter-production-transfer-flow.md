---
id: XT-038
title: Flutter multi file destination flow
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
  - REQ-P1-COLLISION
delivery_role: implementation
contract_changes: []
handoff: .agents/handoffs/XT-038.md
---

## Outcome

Add multi-file and directory selection, virtualized manifests, explicit
destination and collision decisions, and rate reporting to the desktop
experience.

## Context

XT-037 provides typed production adapters. Flutter remains an application and
presentation client of native policy and never validates hostile wire data.
XT-074 separately owns pause, reconnect, restart recovery, and resume.

## Constraints

- Require explicit destination and collision decisions.
- Distinguish accepted, rejected, failed, cancelled, and completed file sets.
- Do not infer trust from names or expose native destination paths.
- Keep large manifests virtualized and commands race-safe.
- Do not implement recovery or persisted-state presentation.

## Acceptance criteria

- [ ] Controller and widget tests cover multi-file send/receive, collision
      prompts, destination changes, rate updates, cancellation, and completion.
- [ ] Large manifests do not block rendering or duplicate commands.
- [ ] Accessibility and keyboard navigation cover all decision dialogs.
- [ ] UI claims match the completed native behavior exactly.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
