# Harness V2 Architecture

## Principles

1. Do not create an entity when an existing Git fact answers the question.
2. Normative contracts live in protected Git history.
3. Runtime status is a read-only projection.
4. Hosted CI is consumed once at publication time.
5. Only temporary queue refs participate in remote coordination.
6. Exact-candidate publication uses compare-and-swap.

## Planes

### Contract Plane

```text
Blueprint -> Change Set -> Delivery Plan -> TaskSpec
```

Files under `.agents/` define project semantics, criteria, ownership, risk, and
trusted Gate commands. Accepted protected history is the fact that a contract
revision was reviewed.

### Derived Runtime Plane

```text
accepted delivery commit  -> done
remote queue/** ref        -> queued
attached work/XT-* tree    -> active
otherwise                  -> ready
```

There is no state transition log. Wait reasons remain operational context and
do not become stored states.

### Temporary Candidate Plane

```text
protected parent
      |
reviewed source patch
      |
temporary queue candidate
      |
one exact-SHA hosted run
      |
protected CAS
      |
delete queue ref
```

The remote runtime namespace is only:

```text
refs/heads/queue/TRAIN/NNN-XT-NNN
refs/heads/queue/bootstrap/CUTOVER
```

`work/**` exists only locally. No approval, state, submission, attestation,
closure, or archive ref is created.

## Components

```text
agent.py -> agent_v2.py
               |
               +-> model.py / project_model.py
               +-> runtime.py -> git_ops.py
               +-> workspace.py -> runtime.py
               +-> tdd.py -> executor.py
               +-> delivery.py -> github_evidence.py + ci_validation.py
               +-> cleanup.py -> runtime.py
               +-> dashboard.py -> runtime.py
```

- `model.py` validates static contracts.
- `project_model.py` composes Blueprint transitions and generates docs.
- `runtime.py` performs no writes.
- `workspace.py` owns local claim/recovery/release.
- `tdd.py` validates Red/Green directly from commit chronology.
- `delivery.py` reviews, builds temporary candidates, validates live CI, and
  publishes.
- `ci_validation.py` validates in-memory workflow results and has no store.
- `cleanup.py` deletes only refs proven redundant by protected ancestry.

## Merge Train

Independent reviewed payloads can form a cumulative train:

```text
H0 -- A -- B -- C
      |    |    |
     CI-A CI-B CI-C
```

Each candidate has one parent and one task delivery commit. CI may run in
parallel. Publication remains ordered because candidate parent must equal the
current protected head. If a predecessor fails, successors cannot publish and
must be rebuilt from the latest accepted head.

## Trust-Root Cutover

Standard candidates cannot modify `.agents/`, Harness code, workflows,
`AGENTS.md`, or `Makefile`. Such changes use `queue/bootstrap/**`, run the full
platform and security matrix, validate the live result once, advance the
protected branch by CAS, and delete the bootstrap ref.

## Failure Model

- Review or local Gate failure leaves the worktree active.
- Hosted candidate failure requires explicit `queue-reopen` or `queue-drop`.
- CAS failure leaves the queue candidate unchanged for rebuild or retry.
- Publication success plus cleanup failure is repaired by `recover`; accepted
  history already proves `done`.
- GC never guesses from age and never deletes unpublished work.
