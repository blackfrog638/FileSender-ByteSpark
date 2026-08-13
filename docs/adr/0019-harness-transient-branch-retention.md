# ADR 0019: Harness Transient Branch Retention

- Status: accepted
- Date: 2026-08-13
- Requester: project owner
- Decision makers: project owner, integration owner
- Scope: Harness runtime refs, branch protection, and recovery

## Context

Harness V2 originally treated every runtime ref as immutable. This correctly
protected authoritative state and evidence, but it also retained local
`work/**` branches and remote `queue/**` branches after their useful lifetime.
GitHub presents refs under `refs/heads/**` as branches, so completed candidates
accumulated in the branch catalogue even though their durable submission,
attestation, state, archive, and protected commit already existed.

Artifact retention was configured, but branch retention and garbage collection
were not. Manual deletion could not distinguish a redundant queue ref from an
in-flight candidate and was blocked by the runtime immutability ruleset.

## Decision

Runtime refs are divided into two classes:

| Class | Namespaces | Policy |
| --- | --- | --- |
| Durable evidence | `approve/**`, `state/**`, `submit/**`, `attest/**`, `archive/**` | No deletion and no non-fast-forward update |
| Transient delivery | local `work/**`, remote/local `queue/**` | No non-fast-forward update; expected-SHA deletion after durable evidence |

Cleanup ordering is:

1. A submission ref and `active -> queued` state are durable before the local
   worktree and `work/**` branch are deleted.
2. A failed queue candidate is archived before the original `queue/**` ref is
   deleted.
3. A successful candidate reaches the protected branch and durable `done`
   state before its `queue/**` ref is deleted.
4. A bootstrap candidate has an immutable bootstrap attestation and is
   published before its queue ref is deleted.
5. Acceptance closure reaches durable `done` before its zero-payload work
   branch is deleted.

Every deletion is compare-and-swap against the reviewed candidate or source
SHA. A moved ref is retained and reported as a conflict.

An idempotent `branch-gc` command repairs cleanup interrupted by network or
process failure. Dry-run is the default. Eligibility comes only from durable
task state, acceptance or bootstrap attestation, or a matching archive ref.
Age and branch name alone are insufficient.

The GitHub ruleset for durable evidence excludes `queue/**`. A separate queue
ruleset prohibits non-fast-forward updates but permits deletion. The queue
worker remains the operational deletion identity until a dedicated
least-privilege credential is introduced.

## Consequences

- GitHub no longer accumulates completed queue candidates as active branches.
- Publication completion remains authoritative even if cleanup needs a later
  GC retry.
- Failed candidates remain auditable through `archive/**`.
- Deletion failures are visible and recoverable; they are not silently treated
  as successful cleanup.
- The branch catalogue still contains durable evidence refs because GitHub
  hosts these custom records under `refs/heads/**`. Moving those records to a
  non-branch store is a separate architecture decision.
