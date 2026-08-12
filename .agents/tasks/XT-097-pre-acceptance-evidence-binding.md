---
id: XT-097
title: Pre-acceptance evidence binding
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
delivery_plan: DP-SCHEMA4-PREACCEPTANCE-EVIDENCE
requirement_ids:
  - REQ-SCHEMA4-PREACCEPTANCE-EVIDENCE
delivery_role: implementation_acceptance
contract_changes: []
handoff: .agents/handoffs/XT-097.md
---

## Outcome

Allow schema-v4 acceptance to collect exact-candidate criterion evidence from
an integrated delivery before the acceptance record can bind its verified SHA.

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

## TDD contract

Before implementation edits, add only evidence tests that fail with the exact
line `FAILED: integrated candidate evidence requires no precommitted verified SHA`.
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

## Architecture change

The record declares `none` mode. This task repairs an internal evidence
bootstrap invariant and introduces no new runtime module or dependency.

## Risk profile

The schema-v4 record binds critical functionality, security, compatibility,
and persistence risk to `evidence_test` and `verify`.

## Acceptance criteria

- [ ] The regression fails at Red with the declared exact fingerprint.
- [ ] An integrated candidate with empty `verified_sha` can collect evidence
      from its successful exact-SHA workflow and artifact.
- [ ] The candidate record is read from the exact source commit in a clean
      checkout rather than trusted from mutable worktree bytes.
- [ ] Dirty or mismatched checkouts and non-integrated or already verified
      records fail closed.
- [ ] Existing stale-run, artifact-SHA, matrix, no-skip, and post-acceptance
      evidence checks remain fail closed.
- [ ] `make evidence-test` and `make verify` pass.

## Verification

```bash
make evidence-test
make verify
```
