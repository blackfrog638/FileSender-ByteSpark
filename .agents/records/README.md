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
- `integration.mappings`: source-to-result commit pairs for cherry-picks;
- `verification`: commands and the local or CI evidence reference;
- `acceptance`: the integration owner and acceptance time.

`tool/harness/agent.sh validate` validates every record. A `done` record must
have complete acceptance and verification fields, and every cherry-pick mapping
must have the same stable patch ID on both sides.

Active task transitions update the task branch copy of its record. Integration
copies the reviewed commits with `git cherry-pick -x`, records the resulting
SHA mapping on the integration branch, and moves the task to `integrated`.
Only `agent.sh accept` can move an integrated task to `done`.
