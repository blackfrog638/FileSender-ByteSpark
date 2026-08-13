# Commit message policy

New commits use this form:

```text
type(scope): imperative summary

Optional body explaining why the change is needed and any non-obvious limits.
```

The subject describes repository behavior, not task workflow. It is at most 72
characters, starts with a lowercase imperative verb, has no final punctuation,
and is specific enough to understand without opening the task tracker.

## Types and scopes

Allowed types are `feat`, `fix`, `docs`, `style`, `refactor`, `perf`, `test`,
`build`, `ci`, `chore`, and `revert`. A lowercase scope is mandatory. Prefer
the narrowest stable module name, such as `discovery`, `protocol`, `native`,
`abi`, `desktop`, `harness`, or `docs`.

Examples:

```text
feat(discovery): advertise authenticated peer metadata
fix(protocol): reject manifest paths with dot segments
ci(harness): enforce meaningful commit messages
docs(architecture): define the discovery trust boundary
```

These subjects are rejected because they hide the actual change:

```text
harness: deliver XT-047
chore(harness): update files
fix(native): fix issue
chore(harness): address review comments
```

## Task provenance

An XT identifier is audit metadata, not a description. Keep it out of the
subject and ordinary body text. The merge train creates one product delivery
commit from the reviewed payload and adds a final contiguous trailer block:

```text
Xnn-Task: XT-047
Xnn-Lifecycle: delivery
Xnn-Payload-SHA256: <digest>
```

Source commits may include `Xnn-Task`, but only the temporary exact candidate
adds `Xnn-Lifecycle: delivery`. Runtime status is derived from accepted
delivery commits, attached worktrees, and temporary queue refs.

## Repository identity

`.agents/commit-identity.json` is the machine-readable source of truth for the
repository author and committer:

```text
blackfrog638 <blackfrog638@gmail.com>
```

Both identities must match exactly. A correct author does not excuse an
incorrect committer. `make bootstrap` copies the versioned values into this
repository's local Git config; it does not change the global identity used by
other repositories.

Schema v1 policies remain historical and are evaluated from each commit.
Schema v2 activates the immutable trust root: its first committed content
governs every descendant commit. Modification, deletion, or re-addition after
that activation is rejected. Hooks and bootstrap read the committed `HEAD`
policy once schema v2 is active, so staged content cannot authorize a
replacement identity.

## Enforcement

`make bootstrap` installs the versioned `.githooks/commit-msg` hook through the
repository-local `core.hooksPath` setting and repairs the local identity. The
hook validates the message, the identities reported by `git var`, and staged
attempts to change the immutable policy. It gives immediate feedback but is
not trusted as a gate because `git commit --no-verify` bypasses it.

`make commit-message-test`, `make verify`, local review, and queue CI
independently validate governed commit ranges from commit objects. Message and
schema v1 identity enforcement retain their historical activation boundaries;
immutable enforcement begins with the first schema v2 policy commit. Older
history remains readable and unchanged; it is not retroactively rejected or
rewritten.
