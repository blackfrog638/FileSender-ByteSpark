---
id: XT-000
title: Replace with a concrete outcome
initial_state: ready
workstream: integration
initial_owner: unassigned
depends_on: []
owned_paths: []
contract_changes: []
handoff: .agents/handoffs/XT-000.md
---

## Outcome

Describe one observable outcome. Avoid broad goals such as "implement
networking".

## Context

Link the relevant architecture section, ADR, protocol version, and predecessor
tasks.

Create the matching machine-readable `.agents/records/XT-000.json`. Declare
ADR, architecture, and roadmap impact there. Use `not_required` only with a
concrete rationale.

Declare the delivery commit metadata in that record:

```json
{
  "commit": {
    "type": "feat",
    "scope": "native",
    "summary": "implement one concrete outcome"
  }
}
```

The summary is the user-visible repository outcome, not an XT identifier or a
task lifecycle phrase. See `docs/commit-policy.md`.

## Constraints

- State security, compatibility, platform, and performance constraints.
- List behavior explicitly outside this task.

## Risk profile

Use record schema version 2 and assess every dimension:

```json
{
  "risks": {
    "functionality": {"level": "medium", "rationale": "...", "gates": ["make verify"]},
    "security": {"level": "none", "rationale": "...", "gates": []},
    "performance": {"level": "none", "rationale": "...", "gates": []},
    "compatibility": {"level": "none", "rationale": "...", "gates": []},
    "concurrency": {"level": "none", "rationale": "...", "gates": []},
    "platform": {"level": "none", "rationale": "...", "gates": []},
    "persistence": {"level": "none", "rationale": "...", "gates": []}
  }
}
```

Allowed levels are `none`, `low`, `medium`, `high`, and `critical`. Every
non-`none` risk needs at least one gate, and every gate must exactly match a
command in `verification.commands`.

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
`transition review` executes the commands recorded for the task and rejects
uncommitted or out-of-scope paths.
