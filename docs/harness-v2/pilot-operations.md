# Harness V2 Pilot Operations

This runbook covers the first governed delivery after the Harness V2
bootstrap. It is intentionally limited to commands that preserve immutable
submissions, hosted evidence, and compare-and-swap publication.

## Preconditions

- Run lifecycle commands from the integration worktree.
- Confirm that the Delivery Plan is `approved` and has an authoritative
  `approve/<plan>/<digest>` ref.
- Confirm that the task is `ready` with `tool/harness/agent.sh list`.
- Keep all edits inside the claimed task worktree and its `owned_paths`.

## Delivery

1. Claim the task:

   ```bash
   tool/harness/agent.sh claim XT-NNN
   ```

2. Commit the reviewed source changes in the generated worktree. For a
   Red-Green task, commit the complete declared proof surface first and record
   it before changing production code:

   ```bash
   tool/harness/agent.sh tdd-red XT-NNN
   ```

3. Create the immutable submission:

   ```bash
   tool/harness/agent.sh submit XT-NNN [--red-sha RED_SHA]
   ```

4. Build a train entry from the protected integration base:

   ```bash
   tool/harness/agent.sh queue-build XT-NNN --train-id pilot
   ```

5. Wait for the exact queue ref to pass the hosted workflow once. Collect the
   workflow artifacts and publish only that verified candidate:

   ```bash
   tool/harness/agent.sh collect-evidence \
     XT-NNN refs/heads/queue/pilot/001-XT-NNN \
     --output /tmp/XT-NNN-evidence.json
   tool/harness/agent.sh publish \
     XT-NNN refs/heads/queue/pilot/001-XT-NNN \
     --evidence /tmp/XT-NNN-evidence.json
   ```

The task is complete only when the protected branch points to the exact
candidate, the task state ref is `done`, and the transient queue ref has been
deleted. Local Gate results do not replace hosted evidence.

## Recovery

- Use `claim-recover` after an interrupted claim.
- Use `queue-reopen` when a queued candidate must return to `active`.
- Use `recover` after evidence exists but publication was interrupted.
- Re-run an idempotent publication command after a transient network error.
- Run `branch-gc` to inspect evidence-bound cleanup residue, then
  `branch-gc --execute` to retry it.

Do not edit or delete state, submission, queue, attestation, or archive refs by
hand. Preserve a failed immutable attempt under the archive namespace and
create a new submission or queue entry when the payload changes.
