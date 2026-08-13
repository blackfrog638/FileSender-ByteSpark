# Harness V2 Operations

## Normal Delivery

```bash
tool/harness/agent.sh validate
tool/harness/agent.sh list
tool/harness/agent.sh claim XT-NNN
```

Commit work inside the returned worktree. For Red-based tasks:

```bash
tool/harness/agent.sh tdd-red XT-NNN
```

After Green:

```bash
tool/harness/agent.sh submit XT-NNN \
  --train-id delivery-001 \
  --red-sha RED_SHA
```

Wait for the exact queue branch workflow, then publish:

```bash
tool/harness/agent.sh publish \
  XT-NNN refs/heads/queue/delivery-001/001-XT-NNN
```

Completion means the exact candidate is in protected history and its temporary
queue ref is gone. `agent.sh list` then derives `done`.

## Candidate Failure

Restore a worktree:

```bash
tool/harness/agent.sh queue-reopen \
  XT-NNN QUEUE_REF --reason "exact candidate CI failed"
```

Discard an abandoned candidate:

```bash
tool/harness/agent.sh queue-drop \
  XT-NNN QUEUE_REF --reason "requirement withdrawn"
```

No archive or state ref is created.

## Cleanup

```bash
tool/harness/agent.sh branch-gc
tool/harness/agent.sh branch-gc --execute
```

Use `recover XT-NNN QUEUE_REF` only when protected publication succeeded but
queue deletion failed.

## Expected Remote Branches

At rest:

```text
main
harness
```

During delivery:

```text
queue/**
```

The following namespaces are not Harness V2 and should not exist:

```text
approve/**
state/**
submit/**
attest/**
archive/**
```
