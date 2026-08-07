# Task records

Each backlog task has one tracked JSON record. The record is the durable source
of truth for task state, review evidence, integration provenance, and document
impact. Local `branch.task/XT-NNN.xnn*` Git configuration is only a scheduler
cache for worktrees in one clone.

Records use schema version 1 and contain:

- `state`: `ready`, `claimed`, `in_progress`, `blocked`, `review`,
  `integrated`, or `done`;
- `base_sha` and `head_sha`: the task branch range reviewed for delivery;
- `handoff`: the tracked handoff document;
- `impacts`: explicit ADR, architecture, and roadmap dispositions;
- `integration`: strategy-specific source and result provenance;
- `verification`: commands and the local or CI evidence reference;
- `acceptance`: the integration owner and acceptance time.

`tool/harness/agent.sh validate` validates every record. A `done` record must
have complete acceptance and verification fields.

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
