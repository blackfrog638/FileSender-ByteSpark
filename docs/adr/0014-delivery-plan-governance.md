# ADR 0014: Delivery Plan governance

- Status: accepted
- Date: 2026-08-09
- Proposed by task: XT-063
- Accepted by task: XT-063

## Context

The roadmap records product milestones and task records prove execution,
review, integration, and acceptance. Between those layers, the repository has
only manually maintained backlog entries and task specifications. Nothing
proves which tasks cover a roadmap requirement, whether an acceptance task
depends on every implementing task, or whether the task graph still matches
the reviewed decomposition.

This gap is visible in the current P1 plan. Task specifications gained security
prerequisites that were not copied into the backlog, while the roadmap kept a
separate scheduling table. The execution harness can safely deliver a known XT
task, but it cannot prove that a product requirement was completely and
correctly converted into those tasks.

Making the roadmap itself the scheduler would mix product outcomes with
implementation ownership and runtime state. Adding requirement fields only to
individual task records would leave no reviewed aggregate that can detect
missing coverage.

## Decision

### One planning layer

Add versioned Delivery Plan documents under `.agents/plans/`. A plan owns:

- a stable plan ID and roadmap or governance source;
- stable requirement IDs and observable acceptance criteria;
- the implementation task IDs and one acceptance task for each requirement;
- explicit plan status and attributed approval.

The backlog remains authoritative for task titles, workstreams, dependencies,
and owned paths. Task records and active task branches remain authoritative for
runtime state. A plan references those facts and does not copy them.

Task catalogue entries and task-spec front matter carry the inverse binding:

- plan ID;
- covered requirement IDs;
- task role: `implementation`, `acceptance`, or
  `implementation_acceptance`.

The validator checks both directions. This makes an omitted or stale mapping a
hard error without introducing another task graph.

### Plan lifecycle

Schema version 1 supports:

```text
draft -> approved -> superseded
```

A draft may reserve future task IDs so decomposition can be reviewed before
all tasks are registered. Draft tasks cannot be claimed.

Approval is an attributed integration-owner decision recorded with an
RFC 3339 timestamp. Approval succeeds only after the complete plan and all
registered task bindings validate. Any semantic plan change returns the plan
to draft and requires a new approval.

Approval stores a canonical SHA-256 over the plan source, requirements,
acceptance criteria, and task mappings. Status, approval metadata, and
supersession bookkeeping are excluded. Governance recomputes the digest, so an
approved plan cannot retain approval after an unreviewed semantic edit.

Approved plan entries become scheduler-ready, but claim still requires every
declared dependency to have durable `done` state. Claim checks plan approval
and dependencies before creating a branch or worktree.

Plans do not store derived task status. Status views are generated from the
approved plan, durable task records, and active task branches.

### Coverage and closure

Every approved requirement has:

- a nonempty statement and acceptance-criteria list;
- at least one implementation task;
- exactly one acceptance task.

Every registered plan-bound task covers at least one requirement, and every
declared task mapping has an equal inverse mapping in the backlog and task
specification.

For each requirement, the acceptance task must transitively depend on all its
implementation tasks. The complete registered backlog graph must be acyclic.
An acceptance task may close multiple requirements, but a requirement cannot
have multiple acceptance owners.

### Compatibility boundary

Tasks XT-001 through XT-063 predate this contract and remain valid without
synthetic plan metadata. The manifest records XT-064 as the first task that
requires a plan binding. New task generation at or after that threshold
requires plan ID, requirement IDs, and task role.

XT-064 migrates the current P1 decomposition, reconciles plan-bound task
metadata, and generates the roadmap execution view. It does not retroactively
change accepted task provenance.

### Enforcement

Repository governance and claim-time validation reject:

- malformed or duplicate plan and requirement IDs;
- missing or malformed approval;
- unknown task references in approved plans;
- plan-bound tasks absent from their plan;
- one-sided or orphan requirement mappings;
- divergent plan-bound backlog and task-spec metadata;
- dependency cycles;
- acceptance tasks that do not transitively cover implementation tasks;
- required new tasks without an approved plan;
- claims for tasks whose plan is not approved.

The validator and CLI use the Python standard library and deterministic,
path-qualified diagnostics. Focused plan tests are registered as a trusted
gate and remain part of repository verification.

## Consequences

- A roadmap milestone has a reviewable and mechanically complete conversion to
  XT tasks before execution begins.
- AI agents may propose decomposition, but cannot claim generated tasks until
  coverage, dependencies, ownership metadata, and approval are valid.
- The task DAG keeps one source of truth in the backlog.
- Current tasks require a one-time P1 migration instead of fabricated history.
- Planning changes add schema and review overhead before implementation work.
- Acceptance dependencies become explicit and mechanically enforced.

## Alternatives rejected

- Keep a prose-only roadmap table: it cannot prove coverage or detect drift.
- Put task dependencies in the plan: this creates a second scheduler and
  repeats the divergence being fixed.
- Infer requirements from task titles or paths: naming is not a contract and
  cannot prove negative or acceptance coverage.
- Require plans retroactively for all accepted tasks: invented mappings would
  rewrite historical planning evidence without improving delivered proof.
- Let draft plans authorize claims: review would occur after implementation had
  already consumed ownership and could no longer constrain decomposition.
