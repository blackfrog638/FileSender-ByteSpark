# ADR 0020: Project Blueprint And Semantic Re-baseline

- Status: accepted
- Date: 2026-08-13
- Requester: project owner
- Decision makers: project owner, integration owner
- Scope: product planning, architecture governance, documentation authority,
  and code requalification

## Context

Harness V2 made each Delivery Plan internally strict and independently
auditable. It did not provide a machine-readable model above Plans. A
collection of valid Plans could therefore duplicate an outcome, introduce a
semantic dependency cycle, weaken a project invariant, or diverge from the
handwritten roadmap without violating a local Plan contract.

The repository also contains substantial accepted implementation and test
assets from Harness V1. Deleting and regenerating all product code would discard
security, concurrency, ABI, platform, and hostile-input knowledge that cannot
be reconstructed from a high-level planning document. Treating all historical
acceptance as current qualification would be equally unsound.

## Decision

The repository has one machine-readable Project Blueprint above Delivery
Plans. It owns:

- project goals and ordered milestones;
- capability and outcome dependency graphs;
- outcome criteria and negative definitions;
- architecture, security, persistence, compatibility, delivery, and quality
  invariants;
- quality budgets and explicit deferrals;
- implementation-unit and production-path projections;
- the composition baseline and target state of each outcome.

A Blueprint Change Set is the reviewed design increment. It binds exact
Blueprint node revisions and exact Plan content revisions. A Delivery Plan is
the executable transition for that increment. A TaskSpec remains a Plan-local
engineering subdivision.

```text
Blueprint target
  -> Change Set
  -> Delivery Plan
  -> TaskSpec
  -> exact-candidate evidence
```

Plans do not redefine product criteria. The validator requires their criterion
statements and negative definitions to equal the referenced Blueprint
criteria. Plans bind implementation tasks and evidence.

Change Sets compose over an explicit state order. The validator rejects:

- graph cycles and orphan nodes;
- duplicate transition writers;
- unsatisfied or undeclared preconditions;
- stale Plan, outcome, or precondition revisions;
- incomplete criterion mappings;
- transitions that omit applicable invariants;
- target gaps without an explicit deferral.

The current project model has a project-owner approval digest over all
Blueprint source documents and Change Sets. Node revision binding prevents an
unrelated Blueprint edit from making every Plan stale.

## Code Re-baseline

Existing assets are classified as:

- `preserve`: retain an independent oracle or currently qualified control-plane
  asset;
- `requalify`: retain the canonical implementation and obtain current
  Blueprint evidence;
- `replace`: rewrite the canonical implementation in place after defect
  evidence;
- `remove`: delete an obsolete or duplicate asset after absence evidence.

Historical V1 acceptance is evidence input, not automatic current Blueprint
qualification. The initial baseline does not classify product implementation
as `replace` or `remove` without module-level evidence.

Product remediation follows dependency order and evolves canonical modules in
place. Parallel `new_*`, `*_v2`, or replacement providers remain prohibited.

## Documentation

Machine-readable files own normative project facts. Deterministic generated
documents present the roadmap, capability map, module map, and asset baseline.
ADRs and explanatory architecture documents remain hand-authored because they
record reasoning rather than duplicate state.

Runtime task state remains outside product commits and is displayed by the
Harness dashboard.

## Consequences

- Independent Plans can no longer silently contradict the project model.
- Blueprint changes require owner approval before implementation Plans become
  claimable.
- Existing code is audited and requalified without a destructive repository
  rewrite.
- The Blueprint is intentionally a semantic compression, not a source-code
  generator. It cannot replace implementation-level concurrency, memory,
  cryptographic, ABI, filesystem, or platform tests.
