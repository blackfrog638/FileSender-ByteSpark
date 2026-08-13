# Harness V2 Threat Model

## Protected Assets

- Blueprint, Plan, TaskSpec, Gate, and risk semantics;
- reviewed source and Red/Green chronology;
- exact queue candidate and hosted result;
- protected branch lineage;
- credentials and private user data.

## Trust Boundaries

```text
task worktree
    | untrusted payload
    v
review + trusted Gate executor
    | exact temporary candidate
    v
GitHub Actions exact-SHA run
    | live validation
    v
protected-branch CAS
```

Accepted protected history is trusted. Local worktrees and queue candidates are
untrusted until their required checks complete.

## Threats And Controls

### Contract Bypass

Threat: a task weakens Blueprint criteria, lowers risk, embeds a command, or
changes another task's contract.

Controls:

- strict JSON fields and IDs;
- complete Blueprint/Change Set/Plan composition validation;
- TaskSpec command prohibition;
- risk-routing minimums;
- active/queued owned-path conflict checks;
- trust-root changes excluded from standard queue.

### Payload Substitution

Threat: source changes after review or a different commit is published.

Controls:

- review requires a clean worktree;
- candidate is built immediately from the reviewed base-to-head patch;
- queue ref is creation-only;
- delivery commit includes payload digest;
- publication rereads exact queue SHA and parent;
- GitHub run and artifacts bind exact head SHA;
- protected push uses compare-and-swap.

### False TDD

Threat: implementation precedes Red, an infrastructure failure is called Red,
or the proof is weakened after Red.

Controls:

- every base-to-Red commit may change only declared proof paths;
- base Gate must pass;
- Red must be an attributed assertion failure with exact fingerprint;
- skip, crash, timeout, and missing tool fail;
- submit replays the Red commit from Git history;
- proof and oracle paths are frozen through Green.

No TDD attestation is stored; commit chronology is the source.

### CI Identity Confusion

Threat: an old, unrelated, partial, or skipped workflow result authorizes
publication.

Controls:

- bind repository, workflow path/blob, branch, head SHA, run ID, and attempt;
- select the latest completed exact-candidate run;
- require every routed job and platform;
- reject skipped or non-success jobs;
- download bounded artifacts and reject unsafe ZIP paths;
- verify source SHA, Gate IDs, criterion digests, and artifact digest.

The validated result exists only in process memory.

### Queue Worker Overreach

Threat: publisher overwrites protected history or publishes a candidate whose
parent is stale.

Controls:

- remote head must equal candidate parent;
- push uses force-with-lease as compare-and-swap;
- publisher verifies remote head after push;
- standard candidate workflow blob must equal its protected parent;
- worker has no approval/state/attestation namespace to write.

### Speculative Train Contamination

Threat: a successor publishes while a failed predecessor is absent.

Controls:

- each candidate has exactly one parent;
- publication order is enforced by parent equality;
- a failed predecessor prevents successor CAS;
- successors are rebuilt from the latest accepted head.

### State Drift

Threat: a stored state says `done` while protected history differs.

Control: no state store exists. `done` is derived only from accepted delivery
trailers. `queued` and `active` are projections over transient refs/worktrees.

### Resource Leaks

Threat: worktrees or remote branches accumulate indefinitely.

Controls:

- queue construction releases local worktree and branch;
- publish/reopen/drop delete queue refs by expected SHA;
- recovery retries post-publication deletion;
- GC selects only refs already reachable from protected history;
- GC is dry-run by default and never uses age.

### Destructive Cleanup

Threat: GC deletes unpublished user commits or a moved queue ref.

Controls:

- attached worktrees are never selected;
- unattached work refs require protected ancestry;
- queue refs require protected ancestry;
- all deletion uses compare-and-swap;
- failed candidates require explicit reopen/drop.

### Trust-Root Self-Validation

Threat: a candidate changes Harness or workflow code and uses that new code to
approve itself.

Controls:

- standard queue rejects trust-root paths;
- cutover uses `queue/bootstrap/**`;
- full platform and security matrix is mandatory;
- operator explicitly invokes bootstrap publication;
- publication still validates exact live result and protected parent.

## Deliberate Non-Assets

Harness V2 does not preserve:

- approval copies;
- task-state histories;
- immutable submission manifests;
- TDD/acceptance/bootstrap attestations;
- failed-candidate archives;
- acceptance-only closure tasks.

The project has no compliance or multi-party audit requirement that justifies
these permanent entities. Git history, protected branch rules, and GitHub's
native run record are sufficient for current engineering needs.
