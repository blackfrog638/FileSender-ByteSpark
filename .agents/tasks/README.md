# Harness V2 TaskSpecs

Each `XT-NNN.json` owns one implementation task's Plan/criterion mapping,
dependencies, paths, risk, TDD mode, and delivery metadata.

TaskSpecs contain no commands or runtime fields. Runtime status is derived:

```text
done    accepted delivery commit
queued  temporary remote queue ref
active  attached local worktree
ready   otherwise
```

There are no acceptance-only TaskSpecs.
