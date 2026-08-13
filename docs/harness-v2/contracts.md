# Harness V2 Contracts

All normative JSON is strict UTF-8. Duplicate keys, unknown fields, invalid
paths, graph cycles, stale revisions, and skipped required evidence fail
closed.

## Project Model

```text
Goal -> Milestone -> Capability -> Outcome
                                  -> Criterion
                                  -> Invariant
                                  -> Quality budget
                                  -> Implementation unit
```

`.agents/project/changes/BC-*.json` binds exact Plan and outcome revisions,
preconditions, preserved invariants, criteria, and monotonic state transitions:

```text
absent -> specified -> implemented -> qualified
```

Change Sets contain no status or approval block. Their accepted protected
commit is the reviewed fact.

## Delivery Plan

```json
{
  "schema_version": 1,
  "id": "DP-P1-DELIVERY",
  "title": "P1 LAN transfer",
  "source": {
    "kind": "roadmap",
    "path": "docs/roadmap.md"
  },
  "requirements": [{
    "id": "REQ-P1-ONE-FILE",
    "statement": "Transfer one accepted file.",
    "criteria": [{
      "id": "CRIT-P1-EXACT-BYTES",
      "statement": "Receiver atomically commits exact accepted bytes.",
      "negative_definitions": [
        "In-memory transport does not qualify."
      ],
      "evidence": {
        "gates": ["one_file_e2e"],
        "scenarios": ["explicit_accept", "exact_bytes"],
        "topology": "two_process_tls",
        "platforms": ["linux", "macos", "windows"],
        "roles": ["sender", "receiver"],
        "allow_skipped": false
      }
    }],
    "implementation_tasks": ["XT-101"]
  }]
}
```

A Plan has no draft status, approval copy, acceptance owner, runtime state, or
CI identity. Every criterion must be assigned to an implementation TaskSpec.

## TaskSpec

```json
{
  "schema_version": 1,
  "id": "XT-101",
  "title": "Authenticated one-file transport",
  "plan": "DP-P1-DELIVERY",
  "criteria": ["CRIT-P1-EXACT-BYTES"],
  "depends_on": ["XT-100"],
  "owned_paths": [
    "native/src/transfer/**",
    "native/tests/transfer/**"
  ],
  "type": "feature",
  "workstream": "native_core",
  "risk": {
    "functionality": "high",
    "security": "critical",
    "performance": "medium",
    "compatibility": "high",
    "concurrency": "high",
    "platform": "medium",
    "persistence": "high"
  },
  "tdd": {
    "mode": "red_green",
    "gate": "one_file_integration",
    "proof_paths": ["native/tests/transfer/**"],
    "oracle_paths": ["protocol/testdata/v1/**"],
    "failure_fingerprints": [
      "FAILED: exact accepted bytes are not committed"
    ]
  },
  "delivery": {
    "commit_type": "feat",
    "scope": "transfer",
    "summary": "implement authenticated one-file transport",
    "architecture_change": {
      "mode": "none",
      "modules": [],
      "supersedes": {"paths": [], "symbols": [], "targets": []},
      "temporary_leases": [],
      "retires_leases": []
    }
  }
}
```

TaskSpec cannot define commands, acceptance-only tasks, or runtime fields.
Dependencies and owned paths are statically validated.

## Gate Policy

Gate nodes define either an argv command or an aggregate. The graph must be
acyclic. Leaf commands declare timeout, controlled environment, inputs,
resource group, supported platforms, and cache mode.

Task execution selects the union of:

```text
criterion Gates
+ TDD Gate
+ risk-routing Gates
+ changed-path Gates
+ phase minimums
```

The executor expands aggregates once, deduplicates leaves, and schedules
independent resource groups concurrently. Only successful no-skip results may
enter cache.

## Runtime Contract

There is no runtime JSON schema. Status follows this deterministic precedence:

1. accepted first-parent delivery commit -> `done`;
2. one matching remote `queue/**` ref -> `queued`;
3. one attached local `work/XT-*` worktree -> `active`;
4. otherwise -> `ready`.

Multiple queue refs or multiple attached worktrees for one task are invalid.

## Candidate Contract

A standard queue ref points directly to a one-parent delivery commit:

```text
Xnn-Task: XT-NNN
Xnn-Lifecycle: delivery
Xnn-Payload-SHA256: <digest>
```

The changed paths must be owned by the TaskSpec, the payload digest must match,
and the workflow blob must equal the protected parent workflow. Queue refs are
creation-only and deleted after publish, reopen, or explicit drop.

## Hosted Result Contract

Publication reads GitHub once and validates:

- repository, workflow path and workflow blob;
- run ID, attempt, push event, exact head SHA and branch;
- required jobs with `success`, no skipped jobs;
- required platform artifacts bound to the same source SHA;
- expected Gate IDs and criterion digests;
- artifact size, entry count, paths, and digest.

The normalized value lives only in process memory. GitHub retains its native
run according to platform policy; Harness creates no evidence copy.
