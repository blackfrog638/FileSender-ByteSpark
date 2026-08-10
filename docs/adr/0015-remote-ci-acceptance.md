# ADR 0015: Remote CI Acceptance

- Status: accepted
- Date: 2026-08-10
- Decision owners: integration owner

## Context

The task lifecycle currently permits `agent.sh accept` to mark an integrated
task `done` after local verification. The command records a `local:*`
verification reference and does not push the integration branch. GitHub Actions
therefore may never execute for the accepted delivery, even when the task
requires cross-platform evidence.

Protecting `harness` with required status checks creates an ordering constraint:
GitHub cannot accept an unverified commit on the protected branch merely to
start the workflow that would verify it.

## Decision

Acceptance uses an immutable two-stage publication sequence:

1. Run every trusted local verification gate for the integrated delivery.
2. Push that exact delivery SHA to an ephemeral `ci/XT-NNN` branch.
3. Wait for the complete repository CI workflow for that branch and SHA to
   finish successfully.
4. Record the workflow URL and delivery SHA, then create the acceptance commit.
5. Push the exact acceptance SHA to the same ephemeral branch and wait for its
   complete CI workflow to finish successfully.
6. Fast-forward the protected `harness` branch to the acceptance SHA.
7. Delete the ephemeral branch and mark the local scheduler cache `done`.

The GitHub workflow runs on `ci/**` pushes. Workflow evidence must match the
repository, branch, event, and exact candidate SHA. Only a completed
`success` conclusion is accepted. Missing, stale, skipped, cancelled, timed
out, action-required, or failed runs reject publication.

The `harness` branch requires every repository CI job, applies protection to
administrators, and rejects force pushes and deletion. Required checks are
satisfied on the candidate branch for the same Git object before the
fast-forward push.

If delivery CI fails, the task remains `integrated` and no acceptance commit is
created. If acceptance CI fails, the generated acceptance commit and record
mutation are rolled back to the integrated delivery. If final publication is
interrupted after successful CI, rerunning acceptance resumes publication of
the same checked acceptance SHA.

GitHub credentials come from the configured Git credential helper and are
provided to the API client through process input. They are never printed,
persisted in the repository, embedded in URLs, or passed as command-line
arguments.

## Consequences

- `done` means the accepted delivery is present on the remote integration
  branch and has complete cross-platform CI evidence.
- Acceptance takes two workflow durations because the record-only acceptance
  commit must also satisfy protected-branch checks.
- GitHub availability and API rate limits become explicit delivery
  dependencies; local success alone cannot bypass them.
- The ephemeral candidate ref is operational state, not task provenance, and
  is removed after success or failure.

## Alternatives Rejected

- Push unverified commits directly to `harness`: incompatible with required
  status checks and exposes the integration branch to failing commits.
- Accept local verification references: does not prove Windows, Linux, macOS,
  packaging, or hosted runner behavior.
- Exempt administrators or automation from protection: recreates the bypass
  that this decision is intended to remove.
- Record the acceptance commit's own run URL inside that commit: changing the
  record would create a new SHA with no corresponding CI evidence.
