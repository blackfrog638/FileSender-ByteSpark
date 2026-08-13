# Harness V2 Delivery Flow

## 1. Plan

Update Blueprint, Change Set, Delivery Plan, and TaskSpec together. Regenerate
derived docs and validate:

```bash
python3 -B tool/harness/project_model.py generate
tool/harness/agent.sh validate
```

Protected Git history records the reviewed contract revision. No approval ref
or approval block is created.

## 2. Claim

```bash
tool/harness/agent.sh claim XT-101
```

Claim requires:

- TaskSpec is `ready` by derived status;
- dependencies have accepted delivery commits;
- no active or queued task owns overlapping paths;
- the local work branch and target worktree do not exist.

Success creates only local `work/XT-101` and its attached worktree.

## 3. TDD

For Red-based work, commit the declared proof surface before production code:

```bash
tool/harness/agent.sh tdd-red XT-101
```

The command checks base success, Red-only path chronology, exact attributed
failure, and no skip. It returns the Red SHA but stores no attestation.

Commit Green implementation after Red. Submit will replay Red from that SHA,
verify frozen proof/oracle paths, and execute Green.

## 4. Review And Queue

Single task:

```bash
tool/harness/agent.sh submit XT-101 \
  --train-id train-001 \
  --red-sha RED_SHA
```

Multiple tasks:

```bash
tool/harness/agent.sh queue-build XT-101 XT-102 \
  --train-id train-001 \
  --red XT-101=RED_SHA
```

The command:

1. requires clean active worktrees;
2. checks complete payload ownership and trust-root exclusion;
3. validates TDD and review Gates;
4. applies each reviewed patch to the cumulative protected parent;
5. creates deterministic delivery commits;
6. pushes creation-only `queue/**` refs;
7. releases local worktrees and deletes `work/**`.

There is no intermediate submission object.

## 5. Hosted CI

Pushing `queue/**` triggers `.github/workflows/merge-queue.yml`. The workflow
computes the task/platform matrix and runs the exact candidate once. Artifacts
bind source SHA, platform, Gate results, and criteria.

Do not rerun CI on a different payload under the same queue ref.

## 6. Publish

```bash
tool/harness/agent.sh publish \
  XT-101 refs/heads/queue/train-001/001-XT-101
```

Publisher:

1. rereads the exact queue SHA and parent;
2. fetches the current GitHub run and artifacts;
3. validates all required jobs, platforms, Gates, criteria, and no-skip;
4. requires protected head to equal candidate parent;
5. pushes candidate by compare-and-swap;
6. verifies protected head;
7. deletes local and remote queue refs by expected SHA.

The accepted delivery commit makes the task `done`. No state or acceptance
write follows publication.

## 7. Failure And Recovery

Return a failed candidate to development:

```bash
tool/harness/agent.sh queue-reopen XT-101 QUEUE_REF --reason "CI failed"
```

Discard it explicitly:

```bash
tool/harness/agent.sh queue-drop XT-101 QUEUE_REF --reason "abandoned"
```

Both delete the queue ref directly; neither archives it.

If protected CAS succeeded but queue deletion did not:

```bash
tool/harness/agent.sh recover XT-101 QUEUE_REF
```

Recovery proves candidate ancestry in protected history, then retries queue
deletion. It does not reconstruct or persist CI output.

## 8. GC

```bash
tool/harness/agent.sh branch-gc
tool/harness/agent.sh branch-gc --execute
```

Dry-run is the default. GC selects only transient refs already redundant by
protected ancestry and never selects unpublished work based on name or age.

## Trust-Root Cutover

Harness and governance changes use:

```text
refs/heads/queue/bootstrap/CUTOVER
```

After the complete hosted platform and security matrix succeeds:

```bash
tool/harness/agent.sh bootstrap-publish \
  refs/heads/queue/bootstrap/CUTOVER
```

The command validates the live result, performs protected CAS, and deletes the
bootstrap ref in one flow.
