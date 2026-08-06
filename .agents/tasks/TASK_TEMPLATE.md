---
id: XT-000
title: Replace with a concrete outcome
state: planned
workstream: integration
owner: unassigned
depends_on: []
owned_paths: []
contract_changes: []
---

## Outcome

Describe one observable outcome. Avoid broad goals such as "implement
networking".

## Context

Link the relevant architecture section, ADR, protocol version, and predecessor
tasks.

## Constraints

- State security, compatibility, platform, and performance constraints.
- List behavior explicitly outside this task.

## Acceptance criteria

- [ ] Functional behavior is covered by focused tests.
- [ ] Negative and boundary cases are covered.
- [ ] Public contracts and documentation are updated.
- [ ] Repository verification passes.

## Verification

```bash
# Add deterministic commands with expected outcomes.
make verify
```

## Handoff

Complete `.agents/handoffs/HANDOFF_TEMPLATE.md` before moving to `review`.
