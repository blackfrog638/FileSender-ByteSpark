---
id: XT-058
title: Attributed defect proof
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-057
owned_paths:
  - tool/harness/defect_proof.py
  - tool/harness/defect_proof_test.py
  - tool/harness/governance.py
  - tool/harness/governance_test.sh
  - tool/harness/new_task.sh
  - .agents/records/README.md
  - .agents/tasks/TASK_TEMPLATE.md
  - docs/adr/0013-attributed-defect-proof.md
  - AGENTS.md
contract_changes:
  - Require attributed three-revision evidence for active deterministic bugfixes
handoff: .agents/handoffs/XT-058.md
---

## Outcome

Deterministic defect proof rejects unrelated build, toolchain, and
infrastructure failures instead of treating every nonzero reproduction gate as
evidence for the reported defect.

## Context

XT-055 introduced reproduction-fails/head-passes proof. XT-057's first real
bugfix pilot showed two gaps: a detached reproduction worktree has no ignored
pinned vcpkg checkout, and the runner accepts any executable nonzero result
without proving that the named regression failed.

ADR 0013 defines attributed three-revision proof while preserving accepted
legacy records.

## Constraints

- Resolve commands only through the trusted gate registry.
- Require the task base to pass the same gate before accepting a reproduction
  failure.
- Require an exact, bounded `failure_fingerprint` in reproduction output.
- Derive shared tool roots from the reviewed task worktree; do not trust a
  caller-supplied vcpkg path.
- Run base and reproduction in separate detached worktrees and remove both on
  every success and error path.
- Keep accepted legacy deterministic proofs valid, but require the attributed
  contract for active and future bugfix tasks.
- Do not persist full command output, secrets, absolute temporary paths, or
  environment values.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] A failing task base rejects proof before reproduction is accepted.
- [ ] A nonzero reproduction result without the declared fingerprint rejects
      proof.
- [ ] A base-pass, fingerprinted reproduction-fail, head-pass sequence records
      revision, command, fingerprint, and output digests.
- [ ] Detached gates receive the task worktree's pinned vcpkg root even when
      the caller supplies another value.
- [ ] Forged or structurally incomplete attributed proof is rejected by
      durable governance validation.
- [ ] Accepted legacy bugfix records remain valid; active deterministic
      bugfixes cannot omit `failure_fingerprint`.
- [ ] Generator, task template, record documentation, and ADR agree with the
      executable schema.
- [ ] Repository verification passes.

## Verification

```bash
make governance-test
make verify
```
