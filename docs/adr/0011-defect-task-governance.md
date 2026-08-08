# ADR 0011: Typed defect task governance

- Status: accepted
- Date: 2026-08-08
- Accepted by task: XT-054
- Deterministic proof implemented by task: XT-055

## Context

The task harness currently advances feature work through one reviewed
lifecycle, but schema version 2 labels every new task as generic
`implementation`. A reported defect can therefore be planned without stating
which existing contract it violates, how it is reproduced, or whether the fix
restores, preserves, or changes externally visible behavior.

Creating a second bug-only harness would duplicate state transitions,
integration provenance, risk gates, and acceptance rules. Treating every test
failure as a bug would also let missing product behavior bypass normal feature
planning.

XT-050 established an integration-owned verification registry and made the
reviewed payload immutable. Those properties are prerequisites for storing a
trusted regression gate in defect metadata.

## Decision

### One lifecycle, explicit task types

Schema version 3 keeps the existing lifecycle and adds these task types:

```text
feature
bugfix
refactor
investigation
test
governance
```

Schema version 1 and 2 records remain valid without migration. New task
generation defaults to schema version 3 `feature`; callers select another type
explicitly.

### Bugfix contract

A schema version 3 `bugfix` record contains one `defect` object with:

```text
severity
source
symptom
expected_contract
actual_behavior
trigger
affected_since
proof_mode
reproduction_commit
regression_gate
contract_disposition
```

Severity is `P0`, `P1`, `P2`, or `P3`. Source is one of `audit`, `ci`,
`user_report`, `production`, `test`, or `investigation`. Proof mode is one of
`deterministic`, `sanitizer`, `stress`, `platform_ci`, or `manual`.

`regression_gate` is a gate ID from the integration-owned manifest, not a shell
command. It must also appear in the task's verification gates.
`reproduction_commit` may remain empty while the task is ready, claimed,
blocked, or in progress. Review and later states require a full commit ID.

For `deterministic`, review resolves the gate command from the current trusted
manifest and runs it in both states:

```text
task base <= reproduction commit < reviewed head
reproduction gate exit != 0, 126, or 127
reviewed head gate exit == 0
```

The reproduction runs in a temporary detached worktree that is removed after
execution. The head runs in the clean task worktree, which must remain clean.
The generated `verification.defect_proof` binds proof mode, gate ID, command
SHA-256, both commit IDs, both exit codes, and check time. Governance validates
that binding again at review and integration. A failed proof, verification
gate, or review mutation restores the pre-proof task record.
The low-level `mark-review` mutation executes the proof itself; callers cannot
bypass execution by pre-populating structurally valid proof fields.

`sanitizer`, `stress`, `platform_ci`, and `manual` are not treated as
deterministic aliases. They remain plan-valid but fail closed at review until
an executor defines their mode-specific sampling, environment, and evidence
requirements.

A bugfix must identify an existing contract or invariant. Unimplemented
roadmap behavior remains a feature even when a test can describe it.

### Contract disposition

Every bugfix declares exactly one disposition:

- `restore`: return behavior to an already documented contract.
- `preserve`: repair an internal defect without changing the external
  contract.
- `change`: intentionally alter the contract while fixing the defect.

`change` requires an accepted or proposed ADR in the same task. Emergency
severity changes scheduling priority, not verification or review requirements.

### Investigation contract

A schema version 3 `investigation` record contains one `investigation` object
with a bounded question, scope, required evidence, exit criteria, and outcome
disposition. The disposition starts as `pending` and must become `bugfix`,
`feature`, or `no_change` before review. An investigation does not claim a
product fix.

Other task types contain neither `defect` nor `investigation`.

## Consequences

- Defects and missing features become mechanically distinguishable.
- Reviewers can reject a fix that silently changes a contract or cites an
  untrusted command.
- Schema version 3 records carry more planning data, and bugfix review cannot
  begin until a reproduction commit exists.
- Deterministic proof executes one specialized gate twice in addition to the
  task verification list, increasing review time in exchange for direct
  regression evidence.
- Concurrency conflict detection and the first real bugfix remain separate
  tasks so executor, scheduling, and product-fix failures are attributable.
- Manual, stress, and platform-only defects remain expressible, but later proof
  policy must define evidence strong enough for their risk claims before
  review can proceed.

## Alternatives rejected

- A separate bug harness: lifecycle and integration rules would drift.
- Free-form defect prose: required evidence and contract trade-offs would not
  be machine-checkable.
- Shell commands in task records: a task could redefine its own evidence.
- Requiring deterministic proof for every defect: concurrency and
  platform-specific failures need sanitizer, stress, or platform CI evidence.
- Treating urgent defects as exempt from gates: urgency does not reduce blast
  radius or make unverifiable changes safer.
