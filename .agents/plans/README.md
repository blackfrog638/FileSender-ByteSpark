# Delivery Plans

Delivery Plans are the reviewed bridge between roadmap or governance
requirements and executable XT tasks. JSON documents in this directory use
schema version 1 for legacy planning or schema version 2 for criterion-level
TDD planning. They contain:

- one stable `DP-*` plan ID;
- a roadmap or governance source document;
- stable `REQ-*` requirements with observable acceptance criteria;
- implementation task IDs and exactly one acceptance task per requirement;
- an explicit `draft`, `approved`, or `superseded` status;
- attributed approval with a canonical content SHA-256 for approved plans.

The backlog remains authoritative for task dependencies, workstreams, and
owned paths. Task records and active branches remain authoritative for runtime
state. A plan never copies either source of truth.

Create and validate plans with:

```bash
python3 tool/harness/delivery_plan.py init \
  DP-EXAMPLE "Example delivery" \
  --source-kind roadmap \
  --source-path docs/roadmap.md
python3 tool/harness/delivery_plan.py validate
python3 tool/harness/delivery_plan.py approve \
  DP-EXAMPLE --by integration-owner
```

New plan-bound tasks are registered with matching `delivery_plan`,
`requirement_ids`, and `delivery_role` values in the backlog, task-spec front
matter, and task record. Draft-plan tasks remain blocked. Approval validates
coverage and metadata, then makes those catalogue entries claimable.

The approval digest excludes lifecycle fields but binds the plan source,
requirements, criteria, and task mappings. Editing approved semantics without
returning the plan to draft invalidates governance.

## Future TDD plans

Tasks at or after the manifest's `tdd_governance.required_from_task` boundary
require schema version 2. Each requirement contains stable `CRIT-*` entries
with an observable statement, explicit negative definitions, implementation
task mappings, and one or more `EVD-*` contracts. Evidence contracts bind a
producer task, trusted gate, level, scenarios, assertions, platform/role
matrix, topology, and no-skip policy.

The task record copies only immutable bindings: the approved plan digest,
mapped criterion IDs, proof mode, executor, focused gate, Red surface, failure
fingerprints, and no-skip policy. Generated placeholders are deliberately
unclaimable until those fields are concrete. The prompt renders the approved
criterion semantics before implementation, review proves chronology, and
acceptance closes criteria from the exact integrated candidate.

The activation boundary is future-only. Schema-v1 plans and records before
XT-083 remain valid with their original evidence. Compatibility readers do not
permit XT-083-or-later tasks to omit schema-v2 criteria or schema-v4 proof and
evidence contracts.

For a roadmap source, each approved requirement's `source_ref` must have an
exact marker in the source document:

```markdown
<!-- roadmap-id: RM-P1-EXAMPLE -->
```

ADR 0014 defines coverage, dependency closure, compatibility, and approval
semantics.
