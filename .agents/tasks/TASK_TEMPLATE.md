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

Select one schema version 3 task type:

```json
{
  "schema_version": 3,
  "task_type": "feature"
}
```

Allowed types are `feature`, `bugfix`, `refactor`, `investigation`, `test`, and
`governance`. A bugfix must violate an existing contract or invariant; missing
roadmap behavior remains a feature.

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

Declare whether the task changes a canonical architecture boundary:

```json
{
  "architecture_change": {
    "mode": "none",
    "modules": [],
    "supersedes": {"paths": [], "symbols": [], "targets": []},
    "temporary_leases": [],
    "retires_leases": []
  }
}
```

Allowed modes are `none`, `add`, `replace`, `remove`, and `refactor`. Affected
module IDs come from `.agents/architecture/modules.json`. Replacement and
removal tasks name obsolete paths, symbols, or targets; review proves those
claims against the resulting tree. Temporary production code uses a registered
`XNN-TEMPORARY(lease-id)` marker and a later removal task.

## Defect contract

Bugfix records add:

```json
{
  "defect": {
    "severity": "P1",
    "source": "test",
    "symptom": "Describe the observed failure.",
    "expected_contract": "Cite the existing contract or invariant.",
    "actual_behavior": "Describe the violating behavior.",
    "trigger": "Describe the bounded trigger.",
    "affected_since": "Commit, task, release, or unknown.",
    "proof_mode": "deterministic",
    "reproduction_commit": "",
    "regression_gate": "native_test",
    "contract_disposition": "restore"
  }
}
```

Severity is `P0` through `P3`. Proof mode is `deterministic`, `sanitizer`,
`stress`, `platform_ci`, or `manual`. The reproduction commit may remain empty
during development but must be a full SHA before review. The regression gate
is a trusted manifest gate and must also appear in `verification.gates`.
Disposition is `restore`, `preserve`, or `change`; `change` requires an ADR.

For `deterministic`, commit the focused regression gate while it still fails,
then set `reproduction_commit` to that commit. `transition review` runs the
resolved gate at that detached revision and at the reviewed head. Review
requires failure at reproduction and success at head; the harness generates
`verification.defect_proof`. Do not hand-edit that evidence. The reproduction
must be between the task base and reviewed head. Other proof modes currently
fail closed at review until their mode-specific executor is implemented.

Investigation records instead add:

```json
{
  "investigation": {
    "question": "State one bounded question.",
    "scope": "Define the evidence boundary.",
    "evidence_required": "List the required evidence.",
    "exit_criteria": "Define when the question is answered.",
    "outcome_disposition": "pending"
  }
}
```

Replace `pending` with `bugfix`, `feature`, or `no_change` before review.

## Constraints

- State security, compatibility, platform, and performance constraints.
- List behavior explicitly outside this task.

## Risk profile

Use record schema version 3 and assess every dimension:

```json
{
  "risks": {
    "functionality": {"level": "medium", "rationale": "...", "gates": ["verify"]},
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
non-`none` risk needs at least one trusted gate ID from
`.agents/manifest.yaml`. `verification.commands` is the exact command list
resolved from `verification.gates`; tasks cannot introduce shell commands.

## Acceptance criteria

- [ ] Functional behavior is covered by focused tests.
- [ ] Negative and boundary cases are covered.
- [ ] Public contracts and documentation are updated.
- [ ] Repository verification passes.

## Verification

```bash
# Declare gate IDs in the record; this block documents expected outcomes.
make verify
```

## Handoff

Complete `.agents/handoffs/HANDOFF_TEMPLATE.md` before moving to `review`.
`transition review` executes the commands recorded for the task and rejects
uncommitted or out-of-scope paths.
