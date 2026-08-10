# ADR 0016: Future task TDD governance

- Status: proposed
- Date: 2026-08-11
- Proposed by task: XT-078
- Accepted by task: XT-082

## Context

The harness already binds tasks to approved Delivery Plans, trusted verification
gates, reviewed payloads, squash provenance, and exact remote CI revisions.
Deterministic bugfix tasks additionally prove that a focused regression passes
at task base, fails at an attributed reproduction revision, and passes at the
reviewed head.

Other task types can still implement behavior before adding tests. Delivery Plan
acceptance criteria are prose strings without stable criterion IDs or executable
evidence contracts, and workflow success does not prove which scenario,
platform, packaged binary, or artifact satisfied a requirement. A task can
therefore finish with green tests while its implementation or evidence has
drifted from the approved requirement.

Retroactively assigning test chronology to accepted or active work would
fabricate provenance. The new contract must apply only to tasks created after
the complete governance implementation is independently accepted.

## Decision

### Two independent proofs

Future governed tasks carry two distinct forms of evidence:

- `tdd_proof` proves chronology and causality: the focused trusted gate passes at
  task base, produces an attributed pre-implementation result at a checkpoint,
  and passes at reviewed head without changing the frozen proof surface.
- `criterion_evidence` proves requirement closure: each approved criterion is
  satisfied by the required scenarios, assertions, platforms, roles, packaged
  binaries, remote jobs, and artifacts from the exact integrated candidate.

Neither proof substitutes for the other. Test counts, coverage percentages,
generic repository verification, task state, or an upstream task reaching
`done` are not criterion evidence.

### Versioned planning and task contracts

Delivery Plan schema version 2 replaces anonymous acceptance strings with
stable criterion objects containing:

- a criterion ID and observable statement;
- explicit negative definitions;
- implementation and acceptance ownership;
- one or more evidence contracts naming trusted gate IDs, required scenarios,
  assertions, topology, platform or role matrices, and skip policy.

Task-record schema version 4 adds a `test_contract` that binds the task to
criterion IDs, one task-type-appropriate proof mode, a trusted focused gate,
declared proof-surface paths, expected checkpoint results, and a no-skip policy.
Commands remain integration-owned in `.agents/manifest.yaml`.

Task dependencies stay in the backlog. Runtime state stays in task records and
task branches. The new schemas do not create another scheduler.

### Proof modes

The required proof mode follows the work being performed:

- feature and governance behavior use Red-Green proof;
- bugfixes retain deterministic regression proof;
- refactors use characterization equivalence;
- test infrastructure proves that a declared sentinel or mutant is detected;
- investigations produce bounded evidence but cannot close product criteria;
- acceptance tasks prove evidence closure and cannot add product or test
  behavior.

Infrastructure failures, missing commands or tools, timeouts, crashes, skipped
tests, and unrelated assertions are never valid Red evidence.

### Checkpoint and review

`agent.sh checkpoint XT-NNN red` records a checkpoint inside the existing
`in_progress` lifecycle state. The harness:

1. resolves the focused command from the trusted gate registry;
2. proves the command passes at task base;
3. scans every commit through the checkpoint and rejects production-path
   changes, including changes later reverted;
4. proves the expected checkpoint failure or sentinel result;
5. freezes the plan, criterion, gate, fingerprint, and proof-surface digests.

Review replays base, checkpoint, and head in isolated worktrees. Any changed or
deleted frozen oracle invalidates the checkpoint. High- and critical-risk work
requires a checkpoint review by an identity other than the task owner.

### Structured acceptance evidence

Remote evidence records at least the criterion and scenario IDs, source SHA,
gate and workflow digests, packaged binary digests, platform and role matrix,
job conclusions, artifact digest, run URL, and skip disposition. Acceptance
reruns required gates for the exact integrated candidate and validates every
required job, matrix entry, and artifact before publication.

Mocks, fake gateways, in-memory transports, one-process runs, unauthenticated
sockets, stale artifacts, partial matrices, skipped required jobs, and generic
`make verify` output cannot satisfy a criterion requiring packaged E2E evidence.

### Future-only activation

XT-078 through XT-082 bootstrap and accept this governance contract under the
existing schemas. XT-083 is the first task required to use Delivery Plan schema
version 2, task-record schema version 4, TDD proof, and criterion evidence.

Every task and plan before XT-083 remains valid without synthetic migration.
Compatibility readers may parse legacy records, but must never waive the new
requirements for XT-083 or later.

## Consequences

- A green final suite is no longer sufficient evidence that a future task
  followed TDD or satisfied its original requirement.
- Requirement semantics, negative definitions, test chronology, and remote
  acceptance artifacts become mechanically attributable.
- High-risk tasks add an explicit checkpoint review before implementation.
- Focused proof runs add execution cost, so contracts must select narrow trusted
  gates rather than repeatedly invoking the whole repository suite.
- Plans and records gain schema complexity, while historical provenance remains
  unchanged.

## Alternatives rejected

- Require only final tests: this permits implementation-first work and
  requirement drift.
- Require only Red-Green chronology: a weak or irrelevant test can still prove
  the wrong behavior.
- Treat workflow success as acceptance evidence: it does not bind criteria,
  scenarios, matrix completeness, binaries, or artifacts.
- Apply the contract retroactively: historical Red checkpoints do not exist and
  inventing them would reduce, not improve, auditability.
- Add a new task lifecycle state: checkpoint status is proof metadata inside
  `in_progress`; scheduler state does not need another transition.
