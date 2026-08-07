---
id: XT-047
title: Commit message governance
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-046
owned_paths:
  - AGENTS.md
  - .agents/**
  - .githooks/**
  - Makefile
  - tool/harness/**
  - .github/workflows/ci.yml
  - docs/commit-policy.md
contract_changes: []
handoff: .agents/handoffs/XT-047.md
---

## Outcome

Reject new commits whose subjects are non-conventional, vague, workflow-only,
or rely on an XT number instead of describing repository impact.

## Context

The harness currently generates subjects such as `harness: deliver XT-046` and
`harness: accept XT-046`. An audit before this task found that only 116 of 274
reachable commits matched Conventional Commits. XT-047 governs future history
without rewriting existing SHAs or weakening task provenance.

## Constraints

- Use Conventional Commits with an explicit lowercase scope and a concise
  imperative summary that names repository impact.
- Keep task IDs out of subjects. Preserve traceability with an `Xnn-Task`
  trailer and lifecycle metadata with an `Xnn-Lifecycle` trailer.
- Reject vague summaries, temporary/fixup subjects, malformed bodies, and
  subjects longer than 72 characters.
- Install a versioned `commit-msg` hook during bootstrap for immediate local
  feedback, but also enforce pushed ranges in CI because hooks are bypassable.
- Validate only commits created after the policy lands; do not rewrite or
  retroactively fail historical commits.
- Generate meaningful plan, lifecycle, delivery, and acceptance subjects from
  task commit metadata, with a documented fallback for pre-policy tasks.

## Risk profile

Resolve every schema version 2 risk dimension in the task record. Every
non-none risk must name commands that also appear in `verification.commands`.

## Acceptance criteria

- [ ] Focused tests cover accepted Conventional Commits and rejection of vague
  subjects, XT-number subjects, malformed scopes/bodies, and overlong lines.
- [ ] The harness generates semantic Conventional Commit subjects and keeps XT
  identifiers in trailers for claim, state, delivery, and acceptance commits.
- [ ] New tasks declare delivery type, scope, and summary in their task record.
- [ ] `prepare-review` rejects non-compliant source commits after the policy
  appears in their tree.
- [ ] Bootstrap installs the versioned `commit-msg` hook, while CI validates
  every newly pushed commit range independently of local hooks.
- [ ] Existing history remains unchanged and exempt.
- [ ] `make commit-message-test` passes.
- [ ] Repository verification passes.

## Verification

```bash
make commit-message-test
make verify
```
