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

The backlog is the immutable task catalogue. Runtime ownership is represented
by a unique `task/XT-NNN` Git branch plus its worktree. Branch creation is
atomic, so two agents cannot claim the same task in one clone.

Claims require a clean, committed base. Give the generated prompt to a new
agent and make sure that agent operates only in the printed worktree.

For a task not yet in the backlog, create and review its specification first:

```bash
tool/harness/new_task.sh XT-007 peer-discovery native_core
```

Parallel tasks should have disjoint owned paths. When two tasks need the same
shared contract, serialize the contract change through the integration owner,
then let both tasks build against the accepted interface.

## Move and finish a task

```bash
tool/harness/agent.sh transition XT-001 in_progress
tool/harness/agent.sh transition XT-001 review
tool/harness/agent.sh transition XT-001 done
```

Run the task's focused acceptance commands and `make verify`. Include the
handoff fields from `handoffs/HANDOFF_TEMPLATE.md` in the task or pull request.
Do not mark a task done when required verification was skipped.

The valid runtime path is:

```text
claimed -> in_progress -> review -> done
                    \-> blocked -> in_progress
review -> in_progress
```
