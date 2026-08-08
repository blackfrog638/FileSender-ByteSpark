# ADR 0013: Attributed deterministic defect proof

- Status: accepted
- Date: 2026-08-08
- Proposed by task: XT-058
- Accepted by task: XT-058

## Context

ADR 0011 requires a deterministic bugfix to fail one trusted gate at its
reproduction commit and pass the same gate at reviewed head. XT-057 proved that
this two-revision rule is insufficient.

A detached reproduction worktree does not contain ignored tool checkouts such
as pinned vcpkg. The trusted gate can therefore fail before compiling the
regression. The proof runner also accepts every executable nonzero exit other
than 126 or 127, so an unrelated compiler, dependency, or infrastructure error
can be recorded as evidence for the reported defect.

The proof must identify the intended failure without making task records
authoritative for executable commands or persisting potentially sensitive
command output.

## Decision

### Three revisions

Deterministic proof executes the same manifest-registered gate at:

1. `base_sha`, which must pass;
2. `defect.reproduction_commit`, which must fail with attribution;
3. reviewed head, which must pass.

The passing base distinguishes a regression introduced by the committed
reproduction from pre-existing repository or environment failure. Base and
reproduction execute in separate detached worktrees, both of which are removed
on every success and error path.

### Failure attribution

An active deterministic bugfix declares one bounded, non-placeholder
`failure_fingerprint`. The exact string must equal one complete line in the
combined reproduction output, without surrounding whitespace. The trusted
command still comes only from
`.agents/manifest.yaml`; the fingerprint identifies expected gate output and
cannot replace or extend that command.

Generated evidence binds:

```text
mode
gate
command_sha256
failure_fingerprint_sha256
base_commit
reproduction_commit
head_commit
base_exit_code
reproduction_exit_code
head_exit_code
reproduction_output_sha256
checked_at
```

Full output is not persisted. The output digest supports audit comparison
without storing secrets, absolute temporary paths, or machine logs.

### Trusted tool environment

Detached gates receive `XNN_TRANSFER_VCPKG_ROOT` derived from the reviewed task
worktree's `out/tools/vcpkg`. A caller-provided value cannot redirect proof to
another checkout. The base-pass requirement rejects a missing, corrupt, or
otherwise unusable tool root before the reproduction result is considered.

Other inherited environment remains subject to the same trusted gate and
three-revision comparison.

### Compatibility

Accepted legacy bugfix records remain valid with the ADR 0011 two-revision
proof shape. Records in active lifecycle states must use the attributed
contract. The task generator emits `failure_fingerprint` for new bugfix
records, so removing it cannot create a valid active task.

## Consequences

- Missing dependencies and pre-existing gate failures cannot masquerade as a
  reproduction.
- A reproduction must emit the reviewed failure identity, not merely exit
  nonzero.
- Review runs the regression gate three times and is therefore slower.
- Fingerprints must be stable across supported hosts and specific enough to
  identify the regression assertion.
- Historical accepted evidence remains auditable without rewriting immutable
  task history.
- Reviewers still inspect whether the fingerprint names the intended behavior;
  no generic output string can replace semantic review.

## Alternatives rejected

- Accept any nonzero result: infrastructure and compilation failures are not
  defect evidence.
- Match only an exit code: build systems collapse unrelated failures into the
  same code.
- Persist full stdout and stderr: logs can contain secrets and unstable
  machine paths.
- Run only reproduction and head with a fingerprint: a pre-existing failure
  could coincidentally contain the same generic text.
- Rewrite all accepted records: historical review evidence is immutable and
  already reflects the policy in force at acceptance.
