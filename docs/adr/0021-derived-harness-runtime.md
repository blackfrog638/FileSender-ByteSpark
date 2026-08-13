# ADR 0021: Derive Harness Runtime From Existing Git Facts

## Status

Accepted

## Context

The first Harness V2 implementation persisted approval, task state,
submissions, TDD evidence, acceptance evidence, bootstrap evidence, and failed
attempts as separate `refs/heads/**` branches. This duplicated facts already
available in protected Git history, local worktrees, temporary queue branches,
and GitHub Actions.

The repository has no compliance, multi-party distrust, or long-term audit
requirement that justifies these permanent entities. Their branch count,
recovery complexity, and verifier coupling exceeded their value.

## Decision

Harness V2 keeps:

- normative static contracts in accepted Git history;
- local `work/XT-*` branches and worktrees during development;
- remote `queue/**` branches while exact candidates are under CI;
- the accepted delivery commit after publication;
- GitHub's native workflow record.

Task status is derived:

```text
done    accepted delivery commit
queued  temporary queue ref
active  attached worktree
ready   otherwise
```

Review builds the queue candidate directly. Publication validates the live
GitHub result once, advances the protected branch by compare-and-swap, and
deletes the queue ref. Failed candidates are reopened or dropped without an
archive ref.

The following entities are removed:

```text
approve/**
state/**
submit/**
attest/**
archive/**
acceptance-only TaskSpecs
```

## Consequences

- GitHub's branch list remains clean at rest.
- There is no state reconciliation or evidence-copy recovery path.
- A deleted failed candidate is recoverable only through ordinary local Git
  objects or by reproducing the change; this is acceptable before real Harness
  adoption.
- GitHub retention policy controls CI log lifetime.
- Protected history remains the durable product and delivery record.
- Trust-root changes still require a bootstrap queue and full hosted matrix.

## Supersedes

This decision supersedes the durable-ref and evidence-retention portions of:

- ADR 0017, Harness V2 control plane;
- ADR 0019, Harness transient branch retention.

Their exact-candidate CI, compare-and-swap publication, and conservative
cleanup principles remain in force.
