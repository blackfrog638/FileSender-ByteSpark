# Harness V2 Testing Strategy

## Layers

| Layer | Purpose |
| --- | --- |
| Contract tests | Reject unknown fields, graph errors, weakened criteria, and risk gaps |
| Runtime tests | Derive ready/active/queued/done from Git facts |
| TDD tests | Prove base success, attributed Red, frozen proof, and Green |
| Queue tests | Bind reviewed payload, ownership, parent, trailers, and CAS deletion |
| CI validation tests | Bind run identity, jobs, platforms, Gates, criteria, and artifacts |
| Product Gates | Validate architecture, ABI, native, Flutter, security, and platform behavior |

## Adversarial Requirements

Contract fixtures must reject:

- approval, runtime-state, acceptance-owner, and durable namespace fields;
- duplicate IDs and JSON keys;
- Blueprint cycles, stale revisions, duplicate writers, and criterion weakening;
- task-authored commands and risk below routing minimums;
- unordered overlapping ownership.

Runtime fixtures must prove:

- a task is `ready` without work, queue, or accepted delivery;
- an attached `work/XT-*` worktree produces `active`;
- one remote queue ref produces `queued`;
- an accepted delivery trailer produces `done`;
- multiple queue/worktree facts fail closed;
- no durable evidence refs are created.

TDD fixtures must prove:

- production code cannot appear before Red;
- base Gate passes;
- Red is an exact assertion failure, not skip/crash/timeout/tool failure;
- Red is replayed from its commit during submit;
- proof and oracle changes after Red fail;
- Green succeeds on reviewed head.

Queue fixtures must prove:

- changed paths stay inside TaskSpec ownership;
- standard candidates cannot change trust-root paths;
- candidate patch equals reviewed patch;
- candidate parent and payload digest are exact;
- local worktree/branch are removed after queue creation;
- reopen/drop delete queue without archive;
- moved refs fail compare-and-swap.

Publication fixtures must prove:

- wrong repository/workflow/branch/SHA/run is rejected;
- missing, skipped, failed, stale, partial, or unsafe evidence is rejected;
- protected parent races reject publication;
- successful publication advances protected once and deletes queue;
- recovery only deletes an already accepted queue residue;
- remote branch inventory has no evidence namespaces.

## Gate Routing

Review and queue plans are the union of criterion, TDD, path-risk, and phase
minimum Gates. Aggregate nodes expand to unique leaves. Independent resource
groups run concurrently; shared build groups remain serialized.

Cache accepts only successful no-skip results and binds source tree, command,
Gate policy, toolchain, controlled environment, platform, and isolation mode.

## Hosted Cutover

Trust-root changes require:

```text
Harness V2 tests
Project Model tests
commit policy
Linux product Gates
macOS product Gates
Windows product Gates
sanitizer and fuzz security Gate
Candidate accepted
```

All jobs run against one `queue/bootstrap/**` candidate. Publication validates
the live run once, performs protected CAS, and deletes the queue ref.
