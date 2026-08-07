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

Squash is the standard task integration strategy. `agent.sh integrate` records
the complete ordered source range and proves that its aggregate payload patch
matches one delivery commit. The generated record for the current task is
excluded from that patch comparison and validated as structured provenance.
`agent.sh accept` records the delivery SHA after verification in a separate
acceptance commit.

An integration owner may explicitly select `--strategy cherry-pick` only when
individual commit topology is a reviewed delivery requirement. A hand-written
squash or cherry-pick is not sufficient evidence.

## Verification policy

- Risk-governed task records declare functionality, security, performance,
  compatibility, concurrency, platform, and persistence risk. Every non-none
  risk names at least one gate that exactly matches an executable
  `verification.commands` entry.
- A passing generic repository gate is not evidence for a specialized claim.
  Reviewers must reject security, performance, interoperability, or recovery
  gates that do not exercise the behavior named in the risk rationale.
- Native changes: build and run native tests on the current platform.
- Flutter changes: `flutter analyze` and `flutter test`.
- Protocol changes: update the versioned specification and compatibility
  section; add parser or golden tests with the implementation.
- C ABI changes: preserve struct-size/version negotiation and add ABI tests.
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
