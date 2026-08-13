# Agent Engineering Contract

This file is the canonical instruction entrypoint for XnnTransfer contributors.
Harness V2 keeps normative contracts in accepted Git history and derives
runtime status from Git facts that already exist.

## Non-Negotiable Rules

1. One active task owns one primary module boundary.
2. Active or queued tasks must not own intersecting paths.
3. A Delivery Plan belongs to exactly one Blueprint Change Set and may not
   weaken Blueprint criteria, dependencies, or invariants.
4. TaskSpec may reference trusted Gate IDs but may not define commands.
5. Review validates the complete source payload. Any later source, Plan,
   TaskSpec, Gate, or proof change requires review again.
6. Only the exact temporary queue candidate that passed required hosted CI may
   advance the protected branch.
7. Publication uses compare-and-swap against the candidate parent.
8. Runtime status is derived; do not create approval, state, submission,
   attestation, closure, or archive refs.
9. `work/**` and `queue/**` are temporary and must be reclaimed promptly.
10. Public ABI, wire protocol, security profile, persisted format,
    compatibility policy, or expensive architecture changes require an ADR.
11. Do not claim unimplemented networking, security, persistence, or transfer
    behavior.
12. Missing tools, skips, timeouts, crashes, and incomplete platform matrices
    are failures, not passing evidence.
13. Standard queue tasks may not modify the verification trust root. Harness,
    Gate policy, schemas, workflows, module inventory, `AGENTS.md`, or
    `Makefile` changes require an explicit bootstrap cutover.

## Sources Of Truth

```text
.agents/manifest.json       Harness version, protected branch, queue namespace
.agents/gates.json          Trusted Gate DAG and commands
.agents/risk-routing.json   Minimum risk and Gate routing
.agents/project/            Blueprint, Change Sets, invariants, budgets, assets
.agents/plans/DP-*.json     Requirements and observable criteria
.agents/tasks/XT-*.json     Dependencies, ownership, risk, and TDD
.agents/architecture/       Canonical product module inventory
docs/adr/                   Reviewed architecture decisions
accepted Git history        Delivered source and task provenance
```

There is no backlog, record, handoff, approval, runtime-state, submission, or
acceptance mirror. Generated docs and the dashboard are derived views.

## Derived Runtime

The displayed lifecycle is:

```text
ready -> active -> queued -> done
           ^          |
           +----------+
```

The values are projections, not stored events:

```text
done    accepted protected history contains Xnn-Task + delivery trailers
queued  remote refs/heads/queue/** identifies the task
active  an attached local work/XT-NNN worktree exists
ready   none of the above
```

`done` has priority over stale transient refs. `queue-reopen` reconstructs a
local worktree and deletes the rejected queue ref. There are no acceptance-only
tasks.

## Planning

1. Change the Blueprint only when project goals, capabilities, outcomes,
   dependencies, invariants, budgets, or asset ownership change.
2. Add or update one Change Set under `.agents/project/changes/`.
3. Add or update the Delivery Plan whose criteria exactly match Blueprint
   criteria.
4. Add one TaskSpec per implementation owner.
5. Regenerate projections:

   ```bash
   python3 -B tool/harness/project_model.py generate
   ```

6. Validate:

   ```bash
   tool/harness/agent.sh validate
   ```

Accepted protected history is the fact that these static contracts were
reviewed. The repository does not copy approval identity into another entity.

## Claim And Development

```bash
tool/harness/agent.sh claim XT-NNN
```

Claim checks accepted dependencies, active/queued ownership conflicts, and the
protected base, then creates an isolated `work/XT-NNN` branch and worktree.
Work only in that worktree and within TaskSpec `owned_paths`.

If worktree creation was interrupted after the branch appeared:

```bash
tool/harness/agent.sh claim-recover XT-NNN
```

## TDD

- feature: Red-Green;
- bugfix: deterministic regression;
- refactor: characterization/equivalence;
- test infrastructure: mutation/sentinel;
- governance: adversarial fixture;
- documentation: static validation;
- investigation: bounded evidence that cannot close product criteria.

For Red-based work:

```bash
tool/harness/agent.sh tdd-red XT-NNN
```

Only declared proof paths may appear before Red. The command validates base
success, exact attributed Red failure, and the failure fingerprint, then prints
the Red commit SHA. It does not persist an attestation. Submit replays Red from
Git chronology and validates Green plus the frozen proof surface.

## Queue

For one task:

```bash
tool/harness/agent.sh submit XT-NNN \
  --train-id train-001 \
  [--red-sha RED_SHA]
```

For a cumulative train:

```bash
tool/harness/agent.sh queue-build XT-101 XT-102 \
  --train-id train-001 \
  [--red XT-101=RED_SHA]
```

Review executes the deduplicated Gate plan, validates ownership and TDD, then
builds the exact candidate directly from the protected parent. The only remote
runtime ref is:

```text
refs/heads/queue/TRAIN/NNN-XT-NNN
```

After queue creation, the local task worktree and `work/**` branch are removed.
Queue refs are creation-only and temporary.

On candidate failure:

```bash
tool/harness/agent.sh queue-reopen XT-NNN QUEUE_REF --reason "CI failed"
tool/harness/agent.sh queue-drop XT-NNN QUEUE_REF --reason "abandoned"
```

Neither command creates an archive ref.

## Hosted Verification And Publication

The queue workflow runs once for the exact candidate. Publication reads the
current GitHub run and artifacts directly:

```bash
tool/harness/agent.sh publish XT-NNN QUEUE_REF
```

Publisher validates repository, workflow blob, run attempt, candidate SHA,
required jobs, platforms, Gate IDs, criterion bindings, and no-skip status. It
then performs one protected-branch CAS and deletes the exact queue ref. It does
not copy CI output into Git.

If publication succeeded but queue deletion failed:

```bash
tool/harness/agent.sh recover XT-NNN QUEUE_REF
```

Trust-root cutovers use `queue/bootstrap/**`, the full hosted matrix, security
Gate, a single live-result check, CAS publication, and immediate queue deletion:

```bash
tool/harness/agent.sh bootstrap-publish refs/heads/queue/bootstrap/CUTOVER
```

## Cleanup

```bash
tool/harness/agent.sh branch-gc
tool/harness/agent.sh branch-gc --execute
```

GC is dry-run by default. It may select only:

- an unattached local work ref already reachable from protected history;
- a queue candidate already reachable from protected history.

GC never uses age and never deletes unpublished user work. Failed or abandoned
queue candidates require explicit `queue-reopen` or `queue-drop`.

## Verification

`make verify` expands the trusted aggregate into unique leaf Gates. Independent
resource groups run concurrently. Cache entries require success and bind the
source tree, Gate policy, toolchain, environment, platform, and isolation mode.

```bash
make harness-v2-test
make project-model-test
make contract-test
make architecture-test
make abi-compat-test
make native-test
make flutter-test
make security-test
make verify
```

## Product Architecture

- `native/include/xnn_transfer/c_api.h` is the only Flutter-facing native API.
- C++ implementation details stay under `native/src/`.
- Flutter presentation depends on application/domain abstractions and never
  imports `dart:ffi` outside `lib/core/native/`.
- Wire behavior is specified under `protocol/spec/` before implementation.
- Discovery, peer metadata, paths, sizes, and frames are hostile input.
- Canonical modules evolve in place according to
  `.agents/architecture/modules.json`; do not create parallel providers.

```text
Flutter presentation -> Flutter application -> native adapter -> C ABI
                                                        |
                                                        v
C ABI bridge -> C++ application -> C++ domain <- C++ infrastructure
```

## Git

- Use meaningful Conventional Commit subjects.
- Keep XT IDs in trailers, not subjects.
- Product history contains delivery commits, not lifecycle metadata.
- Never destructively reset or overwrite unarchived user work.
- A published error is corrected by a reviewed revert, not history rewrite.
