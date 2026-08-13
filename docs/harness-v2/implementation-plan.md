# Harness V2 Implementation Plan

## Status

Harness V2 is implemented. ADR 0021 replaces the original durable evidence-ref
model with a compatibility-free derived runtime.

## Target

```text
static contracts
  -> local claim/worktree
  -> TDD and review Gates
  -> temporary exact-candidate queue ref
  -> one hosted CI run
  -> live result validation
  -> protected compare-and-swap
  -> queue deletion
```

Long-term facts are limited to accepted source, Blueprint, Change Set,
Delivery Plan, TaskSpec, ADRs, and delivery commit trailers.

## Work Packages

### HV2-M1 Contract Reduction

- remove Plan and Change Set approval copies;
- remove acceptance ownership and acceptance-only TaskSpecs;
- remove migration/runtime evidence schemas;
- keep exact Plan/outcome revision binding;
- regenerate deterministic project docs.

### HV2-M2 Derived Runtime

- derive `done` from accepted delivery commits;
- derive `queued` from remote `queue/**`;
- derive `active` from attached local `work/XT-*`;
- treat all remaining tasks as `ready`;
- reject ambiguous multiple queue/worktree facts.

### HV2-M3 TDD Chronology

- validate base success and attributed Red failure;
- permit only proof paths before Red;
- replay Red directly from its commit during review;
- freeze proof and oracle paths through Green;
- do not persist TDD attestations.

### HV2-M4 Temporary Queue

- review the clean source worktree;
- validate owned paths and trust-root exclusion;
- apply reviewed payload directly to protected parent;
- create delivery commit with task and payload trailers;
- push creation-only queue ref;
- release local worktree and branch.

### HV2-M5 One-Time Hosted Verification

- run exact candidate once on required platforms;
- bind run, workflow blob, branch, source SHA, jobs, Gates, and criteria;
- reject skips, partial matrices, stale artifacts, unsafe ZIPs, and failures;
- keep normalized result only in process memory.

### HV2-M6 Publication And Cleanup

- require candidate parent equals protected head;
- publish with compare-and-swap;
- verify remote protected SHA;
- delete queue ref by expected SHA;
- recover only interrupted post-publication cleanup;
- reopen or drop failures without archive refs.

### HV2-M7 Governance Cutover

- run Harness, Project Model, architecture, ABI, product, and security Gates;
- publish through temporary `queue/bootstrap/**`;
- delete old remote `approve/**`, `state/**`, `submit/**`, `attest/**`, and
  `archive/**` refs;
- remove obsolete durable-ref ruleset;
- retain only protected branches and active temporary queue refs.

## Acceptance

- static validators reject removed approval/runtime fields;
- no active code writes durable evidence refs;
- runtime tests cover ready, active, queued, and done derivation;
- queue tests cover direct candidate creation, reopen, publish, recovery, and
  CAS deletion;
- CI validation tests cover exact identity, no-skip, platform, Gate, criterion,
  and artifact binding;
- `make verify` passes locally;
- hosted bootstrap matrix passes before protected cutover;
- remote branch inventory is clean after cutover.

## Residual Work

Production-scale P95, larger speculative trains, and least-privilege queue
credentials remain separate improvements. They do not justify recreating a
per-task evidence database.
