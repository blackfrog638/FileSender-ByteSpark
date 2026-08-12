# Harness V2

Harness V2 separates reviewed contracts, runtime state, verification evidence,
and product history.

## Tracked contracts

```text
.agents/manifest.json       Project owner, integration branch, ref namespaces
.agents/gates.json          Trusted Gate DAG and commands
.agents/risk-routing.json   Mechanical path/risk Gate minimums
.agents/plans/DP-*.json     Requirements and criteria
.agents/tasks/XT-*.json     Dependencies, ownership, risk, and TDD
.agents/migration-v1.json   Read-only legacy acceptance/deferred snapshot
.agents/schemas/            Machine-readable formats
```

Plans and TaskSpecs are static. They never contain runtime owner, state, source
SHA, CI URL, or acceptance evidence.

## Runtime refs

```text
approve/DP-NAME/DIGEST         Immutable project-owner Plan approval
state/XT-NNN                  Append-only task state events
submit/XT-NNN/NNNNNN         Immutable reviewed submissions
queue/TRAIN/NNN-XT-NNN       Exact cumulative candidates
attest/tdd/XT-NNN/SHA        Red attestations
attest/acceptance/XT-NNN/SHA Acceptance attestations
archive/...                   Failed and legacy attempts
```

State transitions use compare-and-swap. Deleting local
`.git/xnn-harness/` cache does not remove authoritative state.

## Commands

```bash
tool/harness/agent.sh validate
tool/harness/agent.sh list
tool/harness/agent.sh claim XT-NNN
tool/harness/agent.sh claim-recover XT-NNN
tool/harness/agent.sh tdd-red XT-NNN
tool/harness/agent.sh submit XT-NNN --red-sha SHA
tool/harness/agent.sh queue-build XT-NNN --train-id train-001
tool/harness/agent.sh queue-reopen XT-NNN QUEUE_REF --reason "CI failed"
tool/harness/agent.sh collect-evidence XT-NNN QUEUE_REF --output evidence.json
tool/harness/agent.sh publish XT-NNN QUEUE_REF \
  --evidence evidence.json \
  --required-job "Harness V2" \
  --required-job "Product gates (linux)"
tool/harness/agent.sh acceptance-close XT-ACCEPT
tool/harness/agent.sh bootstrap-accept \
  refs/heads/queue/bootstrap/CUTOVER_REF --at UTC_TIMESTAMP
tool/harness/agent.sh bootstrap-publish \
  refs/heads/queue/bootstrap/CUTOVER_REF
```

The persistent lifecycle is:

```text
ready -> active -> queued -> done
           ^          |
           +----------+
```

`queued -> active` is used only when source repair is required. Wait reasons
such as dependency, review, CI, retry, and publication are event metadata, not
additional authoritative states. Acceptance tasks alone use `active -> done`
after payload-free evidence closure.

Production claim requires the matching remote `approve/**` ref. `--local`
commands are useful for isolated tests but are not authoritative approval or
publication evidence.

## Delivery

Review creates an immutable submission and releases the development worktree.
Owned paths remain reserved. The merge queue builds cumulative candidates from
the current accepted base. Only the final protected-ref compare-and-swap is
serialized.

The exact candidate runs one required CI workflow. Acceptance evidence is
stored outside the candidate, so there is no record-only acceptance commit and
no second complete CI run.

Legacy V1 records, plans, handoffs, and stopped worktrees remain recoverable
from `archive/harness-v1/*`; they are not active V2 state.
