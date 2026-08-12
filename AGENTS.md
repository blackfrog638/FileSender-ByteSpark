# Agent Engineering Contract

This file is the canonical instruction entrypoint for XnnTransfer contributors.
Harness V2 separates static planning contracts, runtime refs, verification
attestations, and product history.

## Non-negotiable rules

1. One active task owns one primary module boundary.
2. Active or queued tasks must not own intersecting paths.
3. Agents may draft plans and tasks; only the configured project owner may
   approve a Delivery Plan.
4. TaskSpec may reference trusted Gate IDs but may not define commands.
5. Review freezes the complete source payload. Any later source, Plan,
   TaskSpec, Gate, or proof change requires a new submission attempt.
6. Queue candidates must be byte-equivalent to their immutable submissions.
7. Only successful exact-candidate CI and acceptance attestation may authorize
   a protected-branch compare-and-swap.
8. Runtime state never enters product commits.
9. Failed attempts are archived. Do not continue delivery on a lineage that
   contains a failed candidate.
10. Public ABI, wire protocol, security profile, persisted format, compatibility
    policy, or expensive architecture changes require an ADR.
11. Do not claim unimplemented networking, security, persistence, or transfer
    behavior.
12. Missing or skipped required tools are failures, not passing evidence.
13. Standard queue tasks may not modify the Harness verification trust root.
    Those changes require a separately approved bootstrap/governance cutover.

## Sources of truth

```text
.agents/manifest.json       Harness version, owner, branch, ref namespaces
.agents/gates.json          Trusted Gate DAG and commands
.agents/risk-routing.json   Minimum risk and Gate routing
.agents/plans/DP-*.json     Requirements and criteria
.agents/tasks/XT-*.json     Dependencies, ownership, risk, and TDD
.agents/migration-v1.json   Read-only V1 acceptance/deferred snapshot
.agents/architecture/       Canonical product module inventory
docs/adr/                   Reviewed architecture decisions
```

There is no backlog/record/handoff mirror. Dashboard and handoff output are
derived views.

## Runtime state

The persistent lifecycle is:

```text
ready -> active -> queued -> done
           ^          |
           +----------+
```

Wait reasons are event metadata, not extra states. Acceptance tasks alone may
use `active -> done` after external evidence
closure; they never create a queue candidate.

Runtime refs are:

```text
approve/DP-NAME/DIGEST
state/XT-NNN
submit/XT-NNN/NNNNNN
queue/TRAIN/NNN-XT-NNN
attest/tdd/XT-NNN/SHA
attest/acceptance/XT-NNN/SHA
archive/...
```

Every state update is append-only and compare-and-swap. Local cache under the
common Git directory is disposable and never sufficient for `done`.

## Task workflow

### Planning

1. Add or update one Delivery Plan under `.agents/plans/`.
2. Give every criterion a stable ID, observable statement, negative
   definitions, and required evidence.
3. Add one TaskSpec per implementation or acceptance owner.
4. Run `tool/harness/agent.sh validate`.
5. The project owner reviews and approves the canonical plan digest.

Approval identity comes from `.agents/manifest.json`, repository Git identity,
and an immutable remote `approve/DP-NAME/DIGEST` ref. A caller cannot pass an
arbitrary approver string. Production ref rules permit only the project owner
to create `approve/**`; `--local` approval is non-authoritative.

Plans and TaskSpecs must be present on the accepted integration base before
claim. A pre-existing governance task may deliver new approved Plans and
TaskSpecs, but may not change active/queued contracts. Manifest, Gate/risk
policy, schemas, module inventory, commit identity, workflows, Harness code,
`AGENTS.md`, and `Makefile` require a separate owner-approved cutover because
a candidate may not redefine the verifier that authorizes itself.

### Claim

```bash
tool/harness/agent.sh claim XT-NNN
```

Claim checks approved planning, accepted dependencies, owned-path conflicts,
the integration base, and unresolved placeholders. It then creates an isolated
`work/XT-NNN` branch/worktree and appends `ready -> active`.

Work only in the returned worktree and only in TaskSpec owned paths.
If claim state was persisted but worktree creation was interrupted, run
`tool/harness/agent.sh claim-recover XT-NNN`.

### TDD

Behavior work uses the mode selected by task type:

- feature: Red-Green;
- bugfix: deterministic regression;
- refactor: characterization/equivalence;
- test infrastructure: mutation/sentinel;
- governance: adversarial fixture;
- documentation: static validation without fabricated Red;
- acceptance: evidence closure without product changes;
- investigation: bounded evidence that cannot close product criteria.

For Red-based work:

```bash
tool/harness/agent.sh tdd-red XT-NNN
```

Only proof paths may appear before Red. Compiler errors, missing tools, timeout,
crash, skip, and unrelated failures do not qualify. The immutable Red
attestation is reused during review when its oracle and governance context are
unchanged; review executes Green, not Red again.

### Submit

```bash
tool/harness/agent.sh submit XT-NNN --red-sha SHA
```

Submit runs the deduplicated review Gate plan, requires independent review for
high/critical work, freezes source and proof digests, writes an immutable
submission ref, appends `active -> queued`, and releases the development
worktree. Owned paths remain reserved.

### Queue and publish

```bash
tool/harness/agent.sh queue-build XT-NNN --train-id train-001
```

The queue builds cumulative exact candidates against the latest protected
base. Non-conflicting candidates may validate concurrently. Publication order
follows the train prefix.

If exact candidate CI fails, preserve and reopen it with:

```bash
tool/harness/agent.sh queue-reopen XT-NNN QUEUE_REF \
  --reason "exact candidate CI failed"
```

The merge-queue workflow emits exact candidate artifacts. The queue worker
collects GitHub evidence, validates repository/workflow/run/SHA/jobs/artifacts,
creates an external acceptance attestation, then advances the protected branch
with one compare-and-swap. There is no acceptance commit and no second complete
CI run.

Acceptance-owner tasks create no product payload. After every implementation
task is durably published, run:

```bash
tool/harness/agent.sh acceptance-close XT-ACCEPT
```

This validates implementation acceptance refs, criterion IDs, and protected
ancestry, then writes an external closure attestation and completes the
acceptance task directly from `active`.

If publication succeeds but the final state event fails, use:

```bash
tool/harness/agent.sh recover XT-NNN QUEUE_REF \
  --required-job "Harness V2" \
  --required-job "Product gates (linux)"
```

## Verification

`make verify` expands the trusted `verify` aggregate into unique leaf Gates.
The executor runs independent resource groups concurrently and caches only
successful no-skip evidence bound to:

- source tree;
- command and Gate policy;
- toolchain and controlled environment;
- platform and isolation mode.

Task review and queue phases use the union of criterion, TDD, path-risk, and
phase minimum Gates. A generic aggregate cannot replace ABI, protocol,
security, persistence, E2E, reliability, or performance evidence.

Common commands:

```bash
make harness-v2-test
make contract-test
make architecture-test
make abi-compat-test
make native-test
make flutter-test
make security-test
make verify
```

## Architecture boundaries

- `native/include/xnn_transfer/c_api.h` is the only Flutter-facing native API.
- C++ implementation details stay under `native/src/`.
- Flutter presentation depends on application/domain abstractions and never
  imports `dart:ffi` outside `lib/core/native/`.
- Wire behavior is specified under `protocol/spec/` before implementation.
- Discovery, peer metadata, paths, sizes, and frames are hostile input.
- Canonical modules evolve in place according to
  `.agents/architecture/modules.json`; do not create parallel providers.

Allowed dependency direction:

```text
Flutter presentation -> Flutter application -> native adapter -> C ABI
                                                        |
                                                        v
C ABI bridge -> C++ application -> C++ domain <- C++ infrastructure
```

## Git and recovery

- Use meaningful Conventional Commit subjects.
- Keep XT IDs in trailers, not subjects.
- Product history contains delivery commits, not lifecycle metadata.
- Never use destructive reset or checkout on unarchived user work.
- Archive failed refs and reconstruct payload from the latest accepted base.
- A published error is corrected by a reviewed revert, not history rewrite.

## Legacy V1

Harness V1 records, plans, handoffs, stopped tasks, and failed diagnostics are
historical only. They remain under `archive/harness-v1/*` and are summarized by
`.agents/migration-v1.json`.

Do not restore V1 write paths, schema-v4 records, `integrated` state, tracked
acceptance records, or two-stage acceptance CI.
