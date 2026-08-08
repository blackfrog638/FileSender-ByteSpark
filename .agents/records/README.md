# Task records

Each backlog task has one tracked JSON record. The record is the durable source
of truth for task state, review evidence, integration provenance, and document
impact. Local `branch.task/XT-NNN.xnn*` Git configuration is only a scheduler
cache for worktrees in one clone.

Archived records may use schema version 1. Schema version 2 introduced risk
governance. New tasks use schema version 3 and contain:

- `task_type`: `feature`, `bugfix`, `refactor`, `investigation`, `test`, or
  `governance`;
- `state`: `ready`, `claimed`, `in_progress`, `blocked`, `review`,
  `integrated`, or `done`;
- `base_sha` and `head_sha`: the task branch range reviewed for delivery;
- `handoff`: the tracked handoff document;
- `commit`: Conventional Commit type, scope, and delivery summary for
  commit-governed tasks;
- `architecture_change`: explicit add/replace/remove/refactor intent, affected
  canonical modules, supersession claims, and temporary-code lease lifecycle;
- `risks`: functionality, security, performance, compatibility, concurrency,
  platform, and persistence levels, rationales, and executable gates;
- `impacts`: explicit ADR, architecture, and roadmap dispositions;
- `integration`: strategy-specific source and result provenance;
- `verification`: trusted gate IDs, their resolved commands, and the local or
  CI evidence reference;
- `acceptance`: the integration owner and acceptance time.

A `bugfix` also contains a `defect` object with severity, report source,
symptom, existing expected contract, actual behavior, bounded trigger,
affected-since value, proof mode, reproduction commit, trusted regression
gate, and contract disposition. The reproduction commit may be empty only
before review. The regression gate must be registered and included in
`verification.gates`. Dispositions are:

- `restore`: return to an existing documented contract;
- `preserve`: repair an internal defect without external behavior change;
- `change`: intentionally change the contract and bind an ADR.

A deterministic bugfix cannot enter review until `transition review` executes
the exact regression gate in a detached reproduction worktree and at the
reviewed head. The first execution must return a nonzero, non-infrastructure
exit code; the second must return zero. The generated
`verification.defect_proof` object binds:

- proof mode and regression gate ID;
- SHA-256 of the resolved trusted command;
- reproduction and reviewed-head commit IDs;
- both exit codes and the check time.

The reproduction commit must be an ancestor of the reviewed head and no older
than the task base. Governance revalidates the binding from durable metadata.
`mark-review` executes and overwrites the generated proof at the mutation
boundary, so calling the low-level command cannot substitute task-authored
fields for execution.
`sanitizer`, `stress`, `platform_ci`, and `manual` remain valid planning modes
but have no review executor yet, so they fail closed at review.

An `investigation` contains a bounded question, scope, required evidence, exit
criteria, and outcome disposition. `pending` is allowed during investigation
but not at review; final dispositions are `bugfix`, `feature`, or `no_change`.
Other task types contain neither `defect` nor `investigation`.

`tool/harness/agent.sh validate` validates every record. A `done` record must
have complete acceptance and verification fields.

Each schema version 2 or 3 risk uses one of `none`, `low`, `medium`, `high`, or
`critical`. Every dimension needs a concrete rationale. For new tasks, a
non-`none` risk names one or more gate IDs from `.agents/manifest.yaml`, and
`verification.commands` is the exact resolved command list. Legacy records may
name commands directly, but every command must still exist in that registry.
The task cannot replace a specialized gate with arbitrary shell, and every task
must include the repository-level `verify` gate.

Active task transitions update the task branch copy of its record. Integration
defaults to one squash delivery commit and moves the task to `integrated`. Its
provenance contains:

- `source_base` and `source_head`;
- the complete ordered `source_commits` array and its
  `source_commits_sha256` digest;
- aggregate `source_patch_id` and `result_patch_id` values;
- `result`, filled with the delivery SHA during acceptance;
- `verified_sha`, also filled during acceptance.

Payload patch IDs exclude `.agents/records/XT-NNN.json` for the current task
because integration generates that metadata inside the delivery commit. The
record itself is validated structurally, source commits are checked against
the exact range while available, and the result commit remains mandatory after
source cleanup. Before integration, the aggregate payload at the task branch
tip must equal the payload at `head_sha`, which is the commit submitted for
review. A post-review payload change is not an integration correction; it
requires `review -> in_progress -> review`.

Legacy `cherry-pick` and `merge` records retain `integration.mappings`, where
each source/result pair must have the same stable patch ID. Only
`agent.sh accept` can move an integrated task to `done`.

The harness uses `commit` metadata for squash delivery subjects. Lifecycle
commits use the task title and stable workstream scope. In both cases the
subject explains the repository or governance action while the task number is
stored only in the `Xnn-Task` trailer. Pre-policy records without `commit`
metadata use their workstream and title as a compatibility fallback.

Architecture-governed tasks use module IDs from
`.agents/architecture/modules.json`. A `replace` task must touch exactly its
declared module roots and convert the registered placeholder target in place.
`supersedes.paths`, `supersedes.symbols`, and `supersedes.targets` are absence
claims checked before review.

Temporary production code is not an informal comment. The introducing task
records a unique lease ID, exact path, rationale, and `remove_by_task`, and the
source carries `XNN-TEMPORARY(lease-id)`. The removal task lists the ID in
`retires_leases`. Architecture verification rejects unregistered markers,
missing active markers, duplicate leases, and markers surviving their removal
task.
