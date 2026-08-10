---
id: XT-080
title: Criterion evidence closure
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-079
owned_paths:
  - .agents/records/XT-080.json
  - .agents/tasks/XT-080-criterion-evidence-closure.md
  - .agents/handoffs/XT-080.md
  - .agents/manifest.yaml
  - .github/workflows/ci.yml
  - tool/harness/agent.sh
  - tool/harness/evidence.py
  - tool/harness/evidence_test.py
  - tool/harness/github_ci.py
  - tool/harness/github_ci_test.py
  - tool/harness/governance.py
  - tool/harness/governance_test.sh
  - Makefile
delivery_plan: DP-FUTURE-TDD
requirement_ids:
  - REQ-FUTURE-TDD-EVIDENCE
  - REQ-FUTURE-TDD-ADOPTION
delivery_role: implementation
contract_changes:
  - Add the structured criterion evidence bundle and remote closure contract.
handoff: .agents/handoffs/XT-080.md
---

## Outcome

Require structured criterion evidence from the exact integrated candidate,
including required remote jobs, matrices, packaged binaries, and artifacts,
before a future acceptance task can close a requirement.

## Context

XT-068 binds task acceptance to successful GitHub Actions runs for the delivery
and acceptance commits, but workflow success alone does not identify which
criterion, scenario, job, platform matrix, binary, or artifact was proved.
XT-078 defines evidence contracts and XT-079 proves test chronology. This task
closes the remaining requirement-to-CI evidence boundary.

## Constraints

- Extend, rather than replace, XT-068 remote-CI acceptance and publication.
- Evidence must bind criterion and scenario IDs, source SHA, gate and workflow
  digests, packaged binary digests, platform and role matrix, result, run URL,
  and artifact digest.
- Validate required jobs and matrices through structured GitHub API responses;
  do not scrape presentation text or trust workflow-level success alone.
- Treat missing jobs, skipped required jobs, partial matrices, stale runs,
  malformed artifacts, and SHA mismatches as failures.
- Rerun acceptance gates against the exact integrated candidate. An upstream
  test task reaching `done` is not current evidence.
- Keep diagnostics bounded and secret-free. Do not store credentials, identity
  material, machine-local paths, or unbounded attacker-controlled output.
- Do not change product behavior, C ABI, wire protocol, or test topology.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] A versioned evidence bundle records every field required by the approved
      criterion contract and rejects unknown, duplicate, missing, or stale data.
- [ ] Remote verification binds the exact source SHA to required workflow jobs,
      job conclusions, platform/role matrices, artifact metadata, and content
      digests before acceptance.
- [ ] Acceptance reruns each required evidence gate on the integrated candidate
      and rejects evidence produced only for an earlier implementation task.
- [ ] Fake gateways, in-memory transports, one-process runs, unauthenticated
      sockets, partial matrices, and generic `make verify` output cannot satisfy
      a criterion that requires packaged E2E evidence.
- [ ] Failed evidence collection leaves the task integrated and the protected
      integration branch unchanged, preserving XT-068 retry semantics.
- [ ] Focused fixtures cover missing and skipped jobs, matrix gaps, stale SHA,
      duplicate scenarios, altered artifacts, binary mismatch, timeout, API
      failure, and secret-bearing diagnostics.
- [ ] `make governance-test` and `make verify` pass.

## Verification

```bash
make governance-test
make verify
```
