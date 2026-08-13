# Harness V2

Tracked inputs:

```text
manifest.json       Harness version, integration branch, temporary queue prefix
gates.json          Trusted Gate DAG and commands
risk-routing.json   Path and risk Gate minimums
project/            Project Blueprint and revision-bound Change Sets
plans/              Requirements and criteria
tasks/              Ownership, dependency, risk, TDD, and delivery metadata
schemas/            Machine-readable contract formats
```

Runtime status is not stored. `agent.sh list` derives it from accepted delivery
commits, attached `work/XT-*` worktrees, and temporary remote `queue/**` refs.

```bash
tool/harness/agent.sh validate
tool/harness/agent.sh list
tool/harness/agent.sh claim XT-NNN
tool/harness/agent.sh tdd-red XT-NNN
tool/harness/agent.sh submit XT-NNN --train-id train-001 --red-sha SHA
tool/harness/agent.sh publish XT-NNN QUEUE_REF
```

No `approve/**`, `state/**`, `submit/**`, `attest/**`, or `archive/**`
namespace is part of Harness V2.
