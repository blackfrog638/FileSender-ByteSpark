#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
temporary="$(mktemp -d)"
repository="$temporary/repository"
task_id="XT-999"

cleanup() {
  rm -rf "$temporary"
}
trap cleanup EXIT

git clone --quiet --shared "$root" "$repository"
git -C "$repository" config user.name "Harness Test"
git -C "$repository" config user.email "harness-test@example.invalid"
git -C "$repository" checkout -B harness >/dev/null

python3 - "$repository" "$task_id" <<'PY'
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
task_id = sys.argv[2]
backlog_path = root / ".agents" / "backlog.yaml"
with backlog_path.open(encoding="utf-8") as source:
    backlog = json.load(source)
backlog["tasks"].append(
    {
        "id": task_id,
        "title": "Exercise governance lifecycle",
        "readiness": "ready",
        "workstream": "integration",
        "depends_on": [],
        "owned_paths": ["protocol/testdata/governance-fixture.txt"],
    }
)
backlog_path.write_text(json.dumps(backlog, indent=2) + "\n", encoding="utf-8")

spec = root / ".agents" / "tasks" / f"{task_id}-governance-lifecycle.md"
spec.write_text(
    f"""---
id: {task_id}
title: Exercise governance lifecycle
state: ready
workstream: integration
owner: unassigned
depends_on: []
owned_paths:
  - protocol/testdata/governance-fixture.txt
contract_changes: []
---

## Outcome

Exercise the governance state machine in an isolated clone.
""",
    encoding="utf-8",
)

record = {
    "schema_version": 1,
    "id": task_id,
    "task_type": "test",
    "state": "ready",
    "owner": "unassigned",
    "base_sha": "",
    "head_sha": "",
    "handoff": f".agents/handoffs/{task_id}.md",
    "impacts": {
        "adr": {
            "required": False,
            "status": "not_required",
            "references": [],
            "rationale": "The fixture changes no product contract.",
        },
        "architecture": {
            "status": "not_required",
            "references": [],
            "rationale": "The fixture changes no product architecture.",
        },
        "roadmap": {
            "status": "not_required",
            "references": [],
            "rationale": "The fixture is not a delivery milestone.",
        },
    },
    "integration": {"strategy": "", "mappings": [], "verified_sha": ""},
    "verification": {
        "status": "pending",
        "commands": ["true"],
        "reference": "",
    },
    "acceptance": {"accepted_by": "", "accepted_at": "", "note": ""},
}
record_path = root / ".agents" / "records" / f"{task_id}.json"
record_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
PY

git -C "$repository" add \
  .agents/backlog.yaml \
  ".agents/tasks/$task_id-governance-lifecycle.md" \
  ".agents/records/$task_id.json"
git -C "$repository" commit -m "test: register governance fixture" >/dev/null

"$repository/tool/harness/agent.sh" validate >/dev/null
git -C "$repository" branch task/XT-998
"$repository/tool/harness/agent.sh" validate >/dev/null
git -C "$repository" branch -D task/XT-998 >/dev/null
"$repository/tool/harness/agent.sh" \
  claim "$task_id" test-agent >/dev/null
"$repository/tool/harness/agent.sh" \
  transition "$task_id" in_progress >/dev/null

task_worktree="$temporary/XnnTransfer-$task_id"
python3 - \
  "$task_worktree/.agents/handoffs/$task_id.md" \
  "$task_id" \
  "$task_worktree/protocol/testdata/governance-fixture.txt" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
task_id = sys.argv[2]
fixture = Path(sys.argv[3])
path.write_text(
    f"""# Agent handoff

## Delivered

- Task: {task_id}
- From owner: test-agent
- To owner or reviewer: integration-owner
- Branch: task/{task_id}
- Worktree: isolated test worktree
- Base SHA: recorded task base
- Head SHA: committed task delivery
- Worktree clean: yes
- Owned paths: task handoff
- Observable behavior: exercises the governance lifecycle

## Contracts

- Added or changed: no product contract
- Compatibility impact: none
- ADR or protocol reference: not required

## Verification evidence

- Command: true
- Result: passed
- Skipped gate and reason: none

## Residual risk

- Known limitation: isolated harness fixture only
- Follow-up task: none

## Review focus

- Files or invariants requiring close review: state transitions

## Acceptance

- Accepted by: pending integration
- Accepted at: pending integration
- Follow-up runtime state: review
""",
    encoding="utf-8",
)
fixture.parent.mkdir(parents=True, exist_ok=True)
fixture.write_text("governance fixture\n", encoding="utf-8")
PY
git -C "$task_worktree" add \
  ".agents/handoffs/$task_id.md" \
  protocol/testdata/governance-fixture.txt
git -C "$task_worktree" commit -m "test: deliver governance fixture" >/dev/null

"$repository/tool/harness/agent.sh" \
  transition "$task_id" review >/dev/null
"$repository/tool/harness/agent.sh" integrate "$task_id" >/dev/null
"$repository/tool/harness/agent.sh" \
  accept "$task_id" integration-owner test:governance >/dev/null
"$repository/tool/harness/agent.sh" cleanup "$task_id" >/dev/null

test ! -e "$task_worktree"
if git -C "$repository" show-ref --verify --quiet "refs/heads/task/$task_id"; then
  printf 'Governance cleanup left the task branch behind.\n' >&2
  exit 1
fi
test "$(
  "$repository/tool/harness/governance.py" get "$task_id" state
)" = "done"
"$repository/tool/harness/agent.sh" validate >/dev/null

python3 - "$repository/tool/harness/governance.py" "$task_id" <<'PY'
import importlib.util
import sys

module_path, task_id = sys.argv[1:]
spec = importlib.util.spec_from_file_location("governance", module_path)
if spec is None or spec.loader is None:
    raise SystemExit("Cannot load governance module")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
patterns = ["protocol/testdata/governance-fixture.txt"]
assert module.path_allowed(
    f".agents/handoffs/{task_id}.md", patterns, task_id
)
assert not module.path_allowed(
    ".agents/handoffs/XT-998.md", patterns, task_id
)
assert not module.path_allowed(
    ".agents/records/XT-998.json", patterns, task_id
)
PY

record="$repository/.agents/records/$task_id.json"
record_backup="$temporary/$task_id-record.json"
cp "$record" "$record_backup"
python3 - "$record" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
record = json.loads(path.read_text(encoding="utf-8"))
archived_source = "f" * 40
record["head_sha"] = archived_source
record["integration"]["mappings"][-1]["source"] = archived_source
path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
PY
"$repository/tool/harness/agent.sh" validate >/dev/null

python3 - "$record" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
record = json.loads(path.read_text(encoding="utf-8"))
record["integration"]["mappings"][-1]["patch_id"] = "0" * 40
path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
PY
if "$repository/tool/harness/governance.py" validate >/dev/null 2>&1; then
  printf 'Governance validation accepted a tampered result patch ID.\n' >&2
  exit 1
fi
cp "$record_backup" "$record"
"$repository/tool/harness/agent.sh" validate >/dev/null

"$repository/tool/harness/new_task.sh" \
  XT-998 no-dependency-fixture integration \
  --owned '.agents/handoffs/XT-998.md' >/dev/null
test -f "$repository/.agents/tasks/XT-998-no-dependency-fixture.md"
test -f "$repository/.agents/records/XT-998.json"

printf 'Governance lifecycle test passed.\n'
