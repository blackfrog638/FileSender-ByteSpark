# Harness V2 TaskSpecs

This directory contains one immutable planning contract per active task:

```text
XT-NNN.json
```

TaskSpec owns the plan/criterion mapping, dependencies, owned paths, risk,
TDD mode, and delivery metadata. It does not contain owner, lifecycle state,
source SHA, CI URL, or acceptance evidence.

Runtime state is stored in append-only `state/XT-NNN` refs. Reviewed payloads,
queue candidates, and attestations use their configured independent ref
namespaces.

The active catalogue may be empty. Legacy accepted and deferred tasks are
represented only by `.agents/migration-v1.json` and archived Git history.
