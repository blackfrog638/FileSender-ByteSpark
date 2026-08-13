# Harness V2

Harness V2 is the repository's Blueprint-driven delivery control plane. ADR
0021 defines the current compatibility-free runtime model.

## Guarantees

- one task owns one explicit boundary;
- active and queued ownership cannot overlap;
- tasks cannot inject Gate commands;
- TDD causality is checked from Git chronology;
- hosted CI binds the exact candidate SHA;
- protected publication uses compare-and-swap;
- no record-only acceptance commit or second CI run;
- no permanent approval, state, submission, attestation, or archive refs;
- temporary work and queue refs are reclaimed promptly.

## Facts

```text
static semantics      .agents/** in accepted Git history
delivered source      protected delivery commits
active work           attached local work/XT-* worktrees
queued candidate      temporary remote queue/**
hosted result         GitHub native workflow run
```

## Documents

- [ADR 0021](../adr/0021-derived-harness-runtime.md)
- [Architecture](architecture.md)
- [Contracts](contracts.md)
- [Delivery flow](delivery-flow.md)
- [Threat model](threat-model.md)
- [Testing strategy](testing-strategy.md)
- [Implementation plan](implementation-plan.md)
- [Operations](pilot-operations.md)
- [Worklog](worklog.md)

The worklog preserves earlier design history. Where it describes durable
evidence refs, ADR 0021 and the current documents supersede it.
