# ADR 0012: Task conflict and stale-base governance

- Status: accepted
- Date: 2026-08-08
- Proposed by task: XT-056
- Accepted by task: XT-056

## Context

Task branches isolate files physically, but isolation alone does not prevent
two agents from accepting ownership of the same logical path. Active state also
lives on task branches before integration, so checking only durable records on
the integration branch misses in-flight work.

A second failure mode occurs after claim. The integration branch can advance
while a task remains based on an older commit. Delivering every stale branch
without inspection can overwrite product work or apply review rules older than
the current harness. Rejecting every stale base, however, would serialize
unrelated workstreams and make parallel work ineffective.

## Decision

### Active ownership

Claim, review, and integration compare the target task's explicit
`owned_paths` with every other task in `claimed`, `in_progress`, `blocked`,
`review`, or `integrated`. Rechecking closes concurrent activation and
hand-created-branch bypasses. In-flight state is read from local task branches;
durable `integrated` and `done` records override an older source-branch state.

Exact paths and globs are compared conservatively. A concrete match, recursive
prefix intersection, or unresolved potential intersection is a conflict.
False-positive ownership conflicts are resolved by narrowing the task
catalogue or a written handoff, not by allowing both tasks to proceed.

The overlap check and task-branch creation execute under one lock in the common
Git directory. The lock records its process owner. A live owner blocks another
claim; a dead owner is recovered before retry. Normal and error exits remove
the lock.

### Stale bases

Claim with a custom base, transition to review, and integration compare
`base_sha` with the current integration branch:

- a base that is not an ancestor of integration is rejected as diverged;
- an equal base is current;
- an older ancestor is rejected when upstream changed a task-owned path;
- an older ancestor is also rejected when upstream changed global governance;
- unrelated product changes do not block the task.

Global governance paths are:

```text
AGENTS.md
.agents/manifest.yaml
.agents/architecture/modules.json
.agents/commit-identity.json
tool/harness/**
.githooks/**
.github/workflows/**
Makefile
docs/commit-policy.md
```

Changed paths are computed with rename detection disabled so both deletion and
addition sides remain visible. Review and integration repeat the check because
upstream can advance between those states.

The remedy is to rebase the task onto the current integration branch, resolve
the conflict under task ownership, update `base_sha`, and repeat review. The
harness does not silently rewrite task history or waive the conflict.

## Consequences

- Overlapping active tasks fail before a second worktree is created.
- A task cannot deliver through product or governance changes relevant to its
  declared boundary.
- Independent product work remains parallel when integration advances outside
  both task ownership and global governance.
- Broad ownership patterns intentionally block more work and create pressure
  to keep task boundaries narrow.
- Global harness changes require active tasks to rebase before delivery.
- The local scheduler lock is recoverable after process death but still assumes
  all claims for one clone use the harness.

## Alternatives rejected

- Diff-only collision detection: two tasks can intend the same future path
  before either creates it.
- Durable-record-only active state: in-flight states are committed on task
  branches until integration.
- Reject every stale base: unrelated workstreams would become serial.
- Ignore governance changes when product paths do not overlap: an old task
  could bypass newly accepted gates.
- Advisory warnings: agents can continue and overwrite each other.
