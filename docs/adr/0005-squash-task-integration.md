# ADR 0005: Squash task integration

- Status: superseded
- Date: 2026-08-07
- Supersedes: the default integration strategy in `AGENTS.md`
- Superseded by: ADR 0017 Harness V2 Control Plane

## Context

Task branches intentionally record claim, lifecycle, implementation, handoff,
and review commits. Copying every one of those commits to `harness` preserves
provenance but obscures the delivery history with scheduler state changes.

The integration branch still needs durable evidence that the reviewed task
range is exactly the change delivered, verified, and accepted. Historical task
branches may be deleted after acceptance, so this evidence cannot depend on
the branch remaining available.

## Decision

`agent.sh integrate` defaults to one squash delivery commit for the complete
task range from `base_sha` through the review-state branch tip. The record's
`head_sha` identifies the reviewed implementation head and must be a member of
that range. The task branch retains its development history until acceptance
and cleanup.

The task record stores the source base and head, the ordered source commit
list and its SHA-256 digest, an aggregate stable patch ID for the source range,
the integrated result SHA, and the result patch ID. The source base, head, list
digest, and patch ID are also bound into delivery commit trailers. Aggregate
patch comparison excludes the current task's generated record file; that
record is validated as structured provenance. Governance validation requires
the source list to describe the exact reviewed range while the task branch
exists, requires the result commit to remain available, and requires both
payload patch IDs to agree.

An integrated record initially leaves `result` empty because a commit cannot
contain its own SHA. `agent.sh accept` verifies that the current HEAD is the
trailer-bound delivery commit, then stores that SHA as both `result` and
`verified_sha` in the separate acceptance commit. No unrelated commit may
intervene between integration and acceptance.

Acceptance remains a separate integration-owner commit after repository
verification. Existing one-to-one cherry-pick records remain valid. An
integration owner may explicitly select cherry-pick integration only when the
individual commit topology is itself a reviewed delivery requirement.

## Consequences

- A normal requirement contributes a planning commit, one delivery commit, and
  one acceptance commit to `harness`.
- Task branches keep detailed development and lifecycle history during review.
- Cleanup can prove complete integration without retaining the source branch.
- Conflicts are resolved before provenance is recorded; any resulting patch
  mismatch fails closed.
- Integration fixes that alter the payload require another task review.
- Old task records do not require migration.

## Alternatives rejected

- Continue copying every task commit: this preserves detail at the cost of an
  unreadable integration history.
- Delete lifecycle commits from task branches: this weakens active-task audit
  history and complicates scheduler recovery.
- Store only source and result heads: this cannot prove that every reviewed
  source commit belonged to the integrated range.
