---
id: XT-096
title: Schema-v4 lifecycle fixture isolation
state: ready
workstream: integration
owner: unassigned
depends_on: []
owned_paths:
  - .agents/records/XT-096.json
  - .agents/tasks/XT-096-schema4-lifecycle-fixture.md
  - .agents/handoffs/XT-096.md
  - tool/harness/governance_test.sh
  - tool/harness/lifecycle_fixture.py
  - tool/harness/lifecycle_fixture_test.py
delivery_plan: DP-SCHEMA4-LIFECYCLE-FIXTURE
requirement_ids:
  - REQ-SCHEMA4-LIFECYCLE-FIXTURE
delivery_role: implementation_acceptance
contract_changes: []
handoff: .agents/handoffs/XT-096.md
---

## Outcome

Restore repository verification for a locally integrated schema-v4 delivery by
resetting inherited task state only inside the governance lifecycle fixture,
without fabricating criterion evidence or weakening acceptance validation.

## Defect contract

Resolve every `defect` field in the schema version 4 record. The reproduction
commit may remain empty during development but is required before review.


## Context

XT-080 introduced exact-candidate criterion evidence and XT-082 accepted the
schema-v4 workflow. The first real governed delivery, XT-093, exposed an
uncovered fixture path: `governance_test.sh` changes inherited integrated
records to `done` with no `criterion_evidence`, causing `make verify` to fail
before delivery CI can start. The real record is valid; only the isolated
fixture bootstrap is inconsistent.

## TDD contract

Resolve every `test_contract` placeholder before claim. The approved plan
defines exact criteria, negative definitions, evidence ownership, and
acceptance ownership. Use a trusted focused gate, keep every Red path inside
task ownership, and reject skipped evidence.


## Constraints

- Add the deterministic regression before implementation and bind its failing
  commit to the exact declared fingerprint.
- Restore inherited integrated records from the delivery parent's tracked JSON;
  do not synthesize terminal provenance or criterion evidence.
- Keep all rewriting inside the temporary lifecycle repository. Do not modify
  the caller's integration worktree, task branch, accepted records, evidence
  validator, or no-skip policy.
- Fail closed when the delivery parent or tracked record cannot be read.
- Preserve legacy lifecycle fixtures and the existing remote-CI failure,
  acceptance failure, publication retry, cleanup, and stale-evidence cases.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 4 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] A regression fixture containing an inherited schema-v4 integrated record
      fails at the reproduction commit with the exact declared fingerprint.
- [ ] Fixture preparation restores that record byte-for-byte from the delivery
      parent and does not attach `criterion_evidence`.
- [ ] The isolated synthetic lifecycle completes while the caller's integrated
      record and HEAD remain unchanged.
- [ ] Existing lifecycle, criterion-evidence, and remote acceptance negative
      cases remain fail closed.
- [ ] `make governance-test` and `make verify` pass.

## Verification

```bash
make governance-test
make verify
```
