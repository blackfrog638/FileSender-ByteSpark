# Agent Engineering Contract

This file is the canonical instruction entrypoint for every agent working in
this repository. Read it together with `.agents/manifest.yaml` and the task
file assigned to you before editing code.

## Non-negotiable rules

1. One agent owns one active task and one primary module boundary.
2. Do not edit another task's owned paths without a written handoff.
3. Public cross-workstream interfaces are contracts. Changes to the C ABI,
   wire protocol, persisted schema, security profile, or shared types crossing
   workstream boundaries require an ADR and an integration-owner review.
4. Do not claim unimplemented networking, security, or transfer behavior.
5. Generated Flutter runner files are infrastructure, not business-logic
   locations.
6. Every handoff includes commands run, results, residual risks, and changed
   contracts.
7. `make verify` is the repository-level completion gate.
8. `done` is an acceptance result, not an agent-controlled transition. Only
   the integration owner may run `agent.sh accept` after integration.
9. A bugfix restores or deliberately changes an existing contract. Missing
   roadmap behavior is a feature, not a bug, and severity never removes gates.

## Architecture boundaries

- `native/include/xnn_transfer/c_api.h` is the only Flutter-facing native API.
- C++ implementation details stay under `native/src/`.
- Flutter features depend on abstractions in `domain/` and `application/`;
  presentation code must not call `dart:ffi` directly.
- Wire behavior is specified under `protocol/spec/` before implementation.
- Discovery data is untrusted input. File paths, sizes, peer metadata, and
  protocol frames must be validated at the native boundary.

Allowed dependency direction:

```text
Flutter presentation -> Flutter application -> native adapter -> C ABI
                                                        |
                                                        v
C ABI bridge -> C++ application -> C++ domain <- C++ infrastructure
```

`make architecture-test` mechanically enforces the reviewed Flutter import,
native include, and production CMake target dependency matrices. A new module
or legal dependency requires updating that gate and its positive and negative
fixtures in the same reviewed task.

`.agents/architecture/modules.json` assigns each runtime capability one
canonical target and implementation boundary. Replace placeholders in place;
do not add parallel providers. Architecture-governed tasks declare
`none`, `add`, `replace`, `remove`, or `refactor`, affected modules, and
concrete supersession claims. Temporary production code requires an
`XNN-TEMPORARY(lease-id)` marker and a task-record removal lease.

## Task workflow

1. Run `tool/harness/agent.sh list` from the integration worktree.
2. Claim one ready task with
   `tool/harness/agent.sh claim <task-id> <owner>`.
3. Give the output of `tool/harness/agent.sh prompt <task-id>` to the agent.
4. The agent works only in the generated task worktree and moves the runtime
   state to `in_progress` before code changes.
5. Keep changes inside the owned paths. Request a handoff for shared files.
6. Run focused tests while developing, then `make verify`.
7. Complete `.agents/handoffs/HANDOFF_TEMPLATE.md` in the task file or PR and
   move the runtime state to `review`.
8. The integration owner runs `agent.sh integrate <task>`, then
   `agent.sh accept <task> <reviewer>`. A required integration fix returns the
   task to `in_progress` for a new reviewed source range.
9. Run `agent.sh cleanup <task>` only after the durable record is `done`.

All new commits follow `docs/commit-policy.md`. Subjects use
`type(scope): imperative summary` and describe repository impact without an XT
identifier. Task IDs remain available in the final `Xnn-Task` trailer block.
Run `make commit-message-test` for the focused gate; `make verify`, review
preparation, and CI also enforce governed commit ranges.

The backlog is a reviewed catalogue. `.agents/records/XT-NNN.json` is the
versioned source of truth for lifecycle, verification, document impact,
integration provenance, and acceptance. Git task branches and worktrees provide
single-clone claim isolation. Local branch configuration is only a scheduler
cache and must agree with the tracked record while a task is active.

Only these task states are valid:

```text
ready -> claimed -> in_progress -> review -> integrated -> done
                         \-> blocked
review -> in_progress
blocked -> in_progress
```

New records use schema version 3 task types: `feature`, `bugfix`, `refactor`,
`investigation`, `test`, or `governance`. Bugfix records bind an existing
contract, reproduction commit, trusted regression gate, proof mode, and
`restore`, `preserve`, or `change` disposition. Investigation records define a
bounded question and must resolve to `bugfix`, `feature`, or `no_change` before
review; they do not claim a product fix.

Squash is the standard task integration strategy. `agent.sh integrate` records
the complete ordered source range and proves that its aggregate payload patch
matches one delivery commit. The generated record for the current task is
excluded from that patch comparison and validated as structured provenance.
Integration also proves that the current payload is byte-equivalent to the
payload submitted for review. Any post-review handoff or product change returns
the task to `in_progress` for a fresh review.
`agent.sh accept` records the delivery SHA after verification in a separate
acceptance commit.

An integration owner may explicitly select `--strategy cherry-pick` only when
individual commit topology is a reviewed delivery requirement. A hand-written
squash or cherry-pick is not sufficient evidence.

## Verification policy

- Risk-governed task records declare functionality, security, performance,
  compatibility, concurrency, platform, and persistence risk. New tasks name
  trusted gate IDs from `.agents/manifest.yaml`; the harness resolves their
  commands and rejects task-authored shell. Legacy commands remain valid only
  while they exactly match a registered command. Every task includes the
  repository-level `verify` gate.
- Bugfix regression evidence names a trusted gate ID that is also executed by
  the task. A deterministic bugfix review runs the exact resolved gate at both
  revisions: it must fail at the reproduction commit and pass at the reviewed
  head. The reproduction must be within the task range, and generated proof
  binds the command digest, revisions, and exit codes into the record.
  Unsupported proof modes fail closed. A `change` disposition requires an ADR;
  emergency severity changes scheduling only.
- Commit-governed task records declare the delivery `type`, `scope`, and
  imperative `summary`. Harness lifecycle commits derive meaningful subjects
  from that metadata instead of using the task number as the message.
- Architecture-governed task records declare affected canonical modules,
  superseded paths/symbols/targets, temporary leases, and lease retirements.
  Review rejects declarations that do not match the diff or leave claimed
  obsolete code behind.
- A passing generic repository gate is not evidence for a specialized claim.
  Reviewers must reject security, performance, interoperability, or recovery
  gates that do not exercise the behavior named in the risk rationale.
- Native changes: build and run native tests on the current platform.
- Flutter changes: `flutter analyze` and `flutter test`.
- Protocol changes: update the versioned specification and compatibility
  section; add parser or golden tests with the implementation.
- C ABI changes: preserve struct-size/version negotiation and pass
  `make abi-compat-test`, which compiles the frozen v1 caller, checks layout
  prefixes and signatures, and resolves required exports from the built library.
- Security-sensitive changes: document trust boundaries and negative tests.

If a required SDK is unavailable, report the skipped gate explicitly. A
skipped gate is not equivalent to a passing gate.

## Decision and documentation policy

An ADR is required for a new or changed public cross-workstream contract,
security profile, persisted format, compatibility policy, or architecture
choice that is expensive to reverse. Internal implementation, tests, fixtures,
CI wiring, and interfaces contained within one workstream do not require an ADR
unless they change one of those decisions.

Every task record declares ADR, architecture, and roadmap impact. Each impact
must name updated documents or state `not_required` with a concrete rationale.
The task author proposes the disposition; the integration owner accepts it.
Architecture describes durable current boundaries, while the roadmap tracks
delivery milestones. Neither document is a per-commit changelog.

## Shared-file policy

The following files are integration-owned and should change rarely:

- `AGENTS.md`
- `.agents/manifest.yaml`
- root `CMakeLists.txt`
- `apps/desktop/pubspec.yaml`
- `native/include/xnn_transfer/c_api.h`
- files under `protocol/spec/`

Keep generated files, editor state, secrets, build output, and machine-specific
paths out of commits.
