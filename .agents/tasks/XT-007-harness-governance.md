---
id: XT-007
title: Enforce auditable harness governance
state: ready
workstream: integration
owner: unassigned
depends_on: []
owned_paths:
  - AGENTS.md
  - .agents/**
  - tool/harness/**
  - .github/workflows/**
  - Makefile
  - docs/architecture.md
  - docs/roadmap.md
  - docs/testing.md
  - docs/adr/**
contract_changes:
  - Agent task lifecycle and integration policy
---

## Outcome

Make task review, integration, acceptance, and cleanup auditable from tracked
repository data instead of relying only on local Git configuration.

## Context

The initial harness isolates parallel worktrees and validates build/test
quality, but it does not prove that a reviewed task was integrated, accepted,
documented, or safe to clean up. Existing XT-001 through XT-006 records also
contain stale acceptance and architecture/roadmap state.

## Constraints

- Preserve atomic single-clone task claims and independent task worktrees.
- Support cherry-pick integration, but retain original-to-integrated SHA
  provenance.
- Do not require an ADR for internal implementation-only changes.
- Keep governance validation deterministic and dependency-free.
- Do not weaken existing native, Flutter, security, or packaging gates.

## Acceptance criteria

- [ ] Completed tasks have versioned owner, acceptance, verification, document
      impact, and integration provenance records.
- [ ] Moving to review validates a clean task worktree, owned paths, handoff,
      dependencies, and required evidence.
- [ ] Moving to done cannot bypass integration-owner acceptance.
- [ ] Cherry-pick integration records original and integrated SHAs.
- [ ] Worktree cleanup is refused until the task is accepted and integrated.
- [ ] Roadmap, architecture, testing documentation, and ADR policy reflect the
      implemented baseline.
- [ ] Governance checks run in `make verify` and CI.
- [ ] Repository verification passes.

## Verification

```bash
tool/harness/agent.sh validate
tool/harness/governance_test.sh
make verify
```

## Handoff

Complete `.agents/handoffs/HANDOFF_TEMPLATE.md` before moving to `review`.
