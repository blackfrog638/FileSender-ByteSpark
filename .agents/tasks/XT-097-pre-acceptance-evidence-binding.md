---
id: XT-097
title: Schema-v4 acceptance bootstrap
state: ready
workstream: integration
owner: unassigned
depends_on: []
owned_paths:
  - .agents/records/XT-097.json
  - .agents/tasks/XT-097-pre-acceptance-evidence-binding.md
  - .agents/handoffs/XT-097.md
  - tool/harness/evidence.py
  - tool/harness/evidence_test.py
  - tool/harness/governance_test.sh
  - tool/harness/lifecycle_fixture.py
  - tool/harness/lifecycle_fixture_test.py
delivery_plan: DP-SCHEMA4-PREACCEPTANCE-EVIDENCE
requirement_ids:
  - REQ-SCHEMA4-PREACCEPTANCE-EVIDENCE
delivery_role: implementation_acceptance
contract_changes: []
handoff: .agents/handoffs/XT-097.md
---

## Outcome

Allow schema-v4 acceptance to verify an integrated delivery and collect its
exact-candidate criterion evidence before the acceptance record binds the
verified SHA.

## Defect contract

Resolve every `defect` field in the schema version 4 record. The reproduction
commit may remain empty during development but is required before review.

## Context

XT-080 requires criterion evidence from the exact integrated candidate. The
current collector also requires that candidate's tracked record to contain
`integration.verified_sha == candidate_sha`. Squash and cherry-pick integration
deliberately leave `verified_sha` empty until `mark-accepted`, which runs only
after collection. A Git commit cannot contain its own SHA, so the first real
schema-v4 acceptance cannot satisfy the collector even when the remote
workflow and criterion artifact both pass.

The isolated governance lifecycle also changes inherited integrated records
to `done` before running its synthetic task. Schema-v4 `done` records require
exact-candidate criterion evidence, so repository verification fails before
delivery CI can start. The standalone XT-096 and XT-097 deliveries proved that
fixing either defect alone leaves the other bootstrap blocker active. Their
archived sources are combined here under one reviewed task and plan.

## TDD contract

Before implementation edits, add only governance and evidence tests that fail
with both exact lines
`FAILED: integrated candidate evidence requires no precommitted verified SHA`
and `FAILED: integrated schema-v4 lifecycle state is not isolated`.
Checkpoint that Red revision with `agent.sh checkpoint XT-097 red`.

## Constraints

- Keep `verified_sha` empty in an integrated pre-acceptance record. Do not
  predict, fabricate, or amend the delivery SHA into its own tracked content.
- Load the record used for collection from the exact candidate commit and
  require the checkout to be clean and at that SHA.
- Continue to bind the GitHub run head, artifact workflow run, bundle source
  SHA, approved plan, trusted gate, required scenarios, and assertions.
- Reject non-integrated, already verified, dirty, mismatched, skipped, stale,
  partial, duplicate, or malformed evidence.
- Preserve post-acceptance record and bundle validation unchanged.
- Restore inherited integrated fixture records byte-for-byte from the delivery
  parent's tracked JSON before running the synthetic lifecycle.
- Preflight every fixture replacement before writing any record. Do not
  synthesize terminal provenance or criterion evidence.
- Keep fixture rewriting inside the temporary lifecycle repository and leave
  the caller worktree and HEAD unchanged.

## Architecture change

The record declares `none` mode. This task repairs an internal evidence
bootstrap invariant and introduces no new runtime module or dependency.

## Risk profile

The schema-v4 record binds critical functionality, security, compatibility,
and persistence risk to `governance_test` and `verify`.

## Acceptance criteria

- [ ] The regression fails at Red with both declared exact fingerprints.
- [ ] An integrated candidate with empty `verified_sha` can collect evidence
      from its successful exact-SHA workflow and artifact.
- [ ] The candidate record is read from the exact source commit in a clean
      checkout rather than trusted from mutable worktree bytes.
- [ ] Dirty or mismatched checkouts and non-integrated or already verified
      records fail closed.
- [ ] Existing stale-run, artifact-SHA, matrix, no-skip, and post-acceptance
      evidence checks remain fail closed.
- [ ] The lifecycle fixture restores inherited integrated records from the
      delivery parent without fabricating evidence or changing the caller.
- [ ] `make governance-test` and `make verify` pass.

## Verification

```bash
make governance-test
make verify
```
