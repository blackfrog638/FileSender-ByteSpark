# Agent Engineering Contract

This file is the canonical instruction entrypoint for every agent working in
this repository. Read it together with `.agents/manifest.yaml` and the task
file assigned to you before editing code.

## Non-negotiable rules

1. One agent owns one active task and one primary module boundary.
2. Do not edit another task's owned paths without a written handoff.
3. Public interfaces are contracts. Changes to the C ABI, wire protocol, or
   cross-module types require an ADR and an integration-owner review.
4. Do not claim unimplemented networking, security, or transfer behavior.
5. Generated Flutter runner files are infrastructure, not business-logic
   locations.
6. Every handoff includes commands run, results, residual risks, and changed
   contracts.
7. `make verify` is the repository-level completion gate.

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

The backlog is a reviewed catalogue, not mutable runtime state. Git task
branches and worktrees are authoritative for active claims.

Only these task states are valid:

```text
planned -> ready -> in_progress -> review -> done
                            \-> blocked
```

## Verification policy

- Native changes: build and run native tests on the current platform.
- Flutter changes: `flutter analyze` and `flutter test`.
- Protocol changes: update the versioned specification and compatibility
  section; add parser or golden tests with the implementation.
- C ABI changes: preserve struct-size/version negotiation and add ABI tests.
- Security-sensitive changes: document trust boundaries and negative tests.

If a required SDK is unavailable, report the skipped gate explicitly. A
skipped gate is not equivalent to a passing gate.

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
