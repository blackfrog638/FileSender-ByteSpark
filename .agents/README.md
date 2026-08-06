# Multi-agent harness

The harness makes ownership, contracts, and verification visible to every
contributor. It intentionally uses plain Markdown and YAML so it works with
different agent runtimes.

## List and claim a task

```bash
tool/harness/agent.sh list
tool/harness/agent.sh claim XT-001 alice
tool/harness/agent.sh prompt XT-001
```

The backlog is the reviewed task catalogue. Each task also has:

- a human-readable specification under `.agents/tasks/`;
- a machine-readable durable record under `.agents/records/`;
- a unique `task/XT-NNN` branch and worktree while active.

Branch creation is atomic, so two agents cannot claim the same task in one
clone. The tracked record preserves owner, state, verification, document
impact, integration provenance, and acceptance after cleanup.

Claims require a clean, committed base. Give the generated prompt to a new
agent and make sure that agent operates only in the printed worktree.

For a task not yet in the backlog, create and review its specification first:

```bash
tool/harness/new_task.sh XT-007 peer-discovery native_core
```

Parallel tasks should have disjoint owned paths. When two tasks need the same
shared contract, serialize the contract change through the integration owner,
then let both tasks build against the accepted interface.

## Move, integrate, and finish a task

```bash
tool/harness/agent.sh transition XT-001 in_progress
tool/harness/agent.sh transition XT-001 review
tool/harness/agent.sh integrate XT-001
tool/harness/agent.sh accept XT-001 integration-owner
tool/harness/agent.sh cleanup XT-001
```

Commit implementation and handoff changes before requesting review. The review
transition requires a clean worktree, validates owned paths and handoff fields,
and executes the verification commands in the task record.

`integrate` must run from the configured integration branch. It uses
`git cherry-pick -x` and records stable patch-ID mappings. Shared integration
fixes may be committed after this step. `accept` reruns the recorded commands
and is the only path to `done`. `cleanup` refuses to remove a branch containing
an unmapped commit.

The valid runtime path is:

```text
ready -> claimed -> in_progress -> review -> integrated -> done
                         \-> blocked -> in_progress
review -> in_progress
```

Local Git configuration accelerates single-clone scheduling but is not durable
state. `.agents/records/XT-NNN.json` is authoritative for audit and CI.
