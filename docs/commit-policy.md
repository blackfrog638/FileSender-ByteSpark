# Commit message policy

New commits use this form:

```text
type(scope): imperative summary

Optional body explaining why the change is needed and any non-obvious limits.

Xnn-Task: XT-NNN
Xnn-Lifecycle: delivery
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
subject and ordinary body text. Task-related commits use a final contiguous
trailer block:

```text
Xnn-Task: XT-047
Xnn-Lifecycle: review
```

The harness generates trailers for lifecycle, delivery, and acceptance
commits. Squash delivery commits retain source range and patch provenance in
additional `Xnn-*` trailers.

## Enforcement

`make bootstrap` installs the versioned `.githooks/commit-msg` hook through the
repository-local `core.hooksPath` setting. The hook gives immediate feedback
but is not trusted as a gate because `git commit --no-verify` bypasses it.

`make commit-message-test`, `make verify`, task review preparation, and CI
independently validate governed commit ranges. Enforcement begins with the
commit that first adds this policy file. Older history remains readable and
unchanged; it is not retroactively rejected or rewritten.
