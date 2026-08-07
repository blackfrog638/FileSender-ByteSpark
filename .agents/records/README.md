# Task records

Each backlog task has one tracked JSON record. The record is the durable source
of truth for task state, review evidence, integration provenance, and document
impact. Local `branch.task/XT-NNN.xnn*` Git configuration is only a scheduler
cache for worktrees in one clone.

Archived records may use schema version 1. Risk-governed tasks use schema
version 2 and contain:

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
- `verification`: commands and the local or CI evidence reference;
- `acceptance`: the integration owner and acceptance time.

`tool/harness/agent.sh validate` validates every record. A `done` record must
have complete acceptance and verification fields.

Each schema version 2 risk uses one of `none`, `low`, `medium`, `high`, or
`critical`. Every dimension needs a concrete rationale. A non-`none` risk must
name at least one gate, and each gate must exactly match a command in the
record's `verification.commands`. A `none` risk has no gates. This makes the
claimed mitigation executable during review and acceptance instead of leaving
it as prose.

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
source cleanup.

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
