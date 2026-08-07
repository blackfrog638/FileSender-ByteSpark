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
inherited_head="$(git -C "$repository" rev-parse HEAD)"

python3 - "$repository" "$task_id" "$inherited_head" <<'PY'
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
task_id, inherited_head = sys.argv[2:]
for path in sorted((root / ".agents" / "records").glob("XT-*.json")):
    record = json.loads(path.read_text(encoding="utf-8"))
    if record.get("state") != "integrated":
        continue
    integration = record["integration"]
    if integration.get("strategy") == "squash" and not integration.get("result"):
        integration["result"] = inherited_head
    integration["verified_sha"] = inherited_head
    record["state"] = "done"
    record["verification"]["status"] = "passed"
    record["verification"]["reference"] = "test:inherited-integration"
    record["acceptance"] = {
        "accepted_by": "harness-test",
        "accepted_at": "2000-01-01T00:00:00+00:00",
        "note": "Closed inherited state in the isolated lifecycle fixture.",
    }
    path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")

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
initial_state: ready
workstream: integration
initial_owner: unassigned
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
  .agents/records
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
git -C "$task_worktree" add protocol/testdata/governance-fixture.txt
git -C "$task_worktree" commit -m "test: deliver governance fixture" >/dev/null
git -C "$task_worktree" add ".agents/handoffs/$task_id.md"
git -C "$task_worktree" commit -m "docs: hand off governance fixture" >/dev/null

"$repository/tool/harness/agent.sh" \
  transition "$task_id" review >/dev/null
integration_base="$(git -C "$repository" rev-parse HEAD)"
source_base="$(
  "$task_worktree/tool/harness/governance.py" get "$task_id" base_sha
)"
source_head="$(git -C "$task_worktree" rev-parse HEAD)"
source_commits="$temporary/source-commits.txt"
git -C "$repository" rev-list --reverse \
  "$source_base..task/$task_id" >"$source_commits"
"$repository/tool/harness/agent.sh" integrate "$task_id" >/dev/null
delivery="$(git -C "$repository" rev-parse HEAD)"
test "$(
  git -C "$repository" rev-list --count "$integration_base..$delivery"
)" = "1"
test "$(
  git -C "$repository" show -s --format=%s "$delivery"
)" = "harness: deliver $task_id"

python3 - \
  "$repository/.agents/records/$task_id.json" \
  "$source_commits" \
  "$source_base" \
  "$source_head" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

record_path, commits_path, source_base, source_head = sys.argv[1:]
record = json.loads(Path(record_path).read_text(encoding="utf-8"))
commits = [
    line.strip()
    for line in Path(commits_path).read_text(encoding="utf-8").splitlines()
    if line.strip()
]
integration = record["integration"]
commits_digest = hashlib.sha256(
    ("\n".join(commits) + "\n").encode("ascii")
).hexdigest()
assert record["state"] == "integrated"
assert integration["strategy"] == "squash"
assert integration["source_base"] == source_base
assert integration["source_head"] == source_head
assert integration["source_commits"] == commits
assert integration["source_commits_sha256"] == commits_digest
assert record["head_sha"] in commits
assert integration["source_patch_id"] == integration["result_patch_id"]
assert integration["result"] == ""
assert integration["verified_sha"] == ""
PY

record="$repository/.agents/records/$task_id.json"
integrated_record="$temporary/$task_id-integrated.json"
cp "$record" "$integrated_record"

expect_tamper_rejected() {
  local field="$1"
  if "$repository/tool/harness/governance.py" validate >/dev/null 2>&1; then
    printf 'Governance validation accepted tampered %s.\n' "$field" >&2
    exit 1
  fi
  cp "$integrated_record" "$record"
}

python3 - "$record" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
record = json.loads(path.read_text(encoding="utf-8"))
record["integration"]["source_head"] = "f" * 40
path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
PY
expect_tamper_rejected source_head

python3 - "$record" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
record = json.loads(path.read_text(encoding="utf-8"))
record["integration"]["source_commits"].pop(0)
path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
PY
expect_tamper_rejected source_commits

python3 - "$record" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
record = json.loads(path.read_text(encoding="utf-8"))
record["integration"]["source_patch_id"] = "0" * 40
path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
PY
expect_tamper_rejected source_patch_id

python3 - "$record" "$integration_base" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
record = json.loads(path.read_text(encoding="utf-8"))
record["integration"]["result"] = sys.argv[2]
path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
PY
expect_tamper_rejected result

python3 - "$record" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
record = json.loads(path.read_text(encoding="utf-8"))
record["integration"]["result_patch_id"] = "0" * 40
path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
PY
expect_tamper_rejected result_patch_id

"$repository/tool/harness/agent.sh" \
  accept "$task_id" integration-owner test:governance >/dev/null
acceptance="$(git -C "$repository" rev-parse HEAD)"
test "$(
  git -C "$repository" rev-list --count "$integration_base..$acceptance"
)" = "2"
python3 - "$record" "$delivery" <<'PY'
import json
import sys
from pathlib import Path

record = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
delivery = sys.argv[2]
assert record["state"] == "done"
assert record["integration"]["result"] == delivery
assert record["integration"]["verified_sha"] == delivery
PY
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

test "$(
  "$repository/tool/harness/governance.py" \
    get XT-016 integration.strategy
)" = "cherry-pick"

archived_repository="$temporary/archived-repository"
git clone --quiet --no-local "$repository" "$archived_repository"
archived_record="$archived_repository/.agents/records/$task_id.json"
archived_source_head="$(
  "$archived_repository/tool/harness/governance.py" \
    get "$task_id" integration.source_head
)"
if git -C "$archived_repository" cat-file \
  -e "$archived_source_head^{commit}" 2>/dev/null; then
  printf 'Fresh clone unexpectedly retained archived task commits.\n' >&2
  exit 1
fi
"$archived_repository/tool/harness/agent.sh" validate >/dev/null
archived_record_backup="$temporary/$task_id-archived-record.json"
cp "$archived_record" "$archived_record_backup"

python3 - "$archived_record" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
record = json.loads(path.read_text(encoding="utf-8"))
record["integration"]["source_commits"][0] = "0" * 40
path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
PY
if "$archived_repository/tool/harness/governance.py" \
  validate >/dev/null 2>&1; then
  printf 'Fresh-clone validation accepted a tampered source list.\n' >&2
  exit 1
fi
cp "$archived_record_backup" "$archived_record"

python3 - "$archived_record" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
record = json.loads(path.read_text(encoding="utf-8"))
record["integration"]["result_patch_id"] = "0" * 40
path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
PY
if "$archived_repository/tool/harness/governance.py" \
  validate >/dev/null 2>&1; then
  printf 'Governance validation accepted a tampered result patch ID.\n' >&2
  exit 1
fi
"$repository/tool/harness/agent.sh" validate >/dev/null

"$repository/tool/harness/new_task.sh" \
  XT-998 no-dependency-fixture integration \
  --owned '.agents/handoffs/XT-998.md' >/dev/null
test -f "$repository/.agents/tasks/XT-998-no-dependency-fixture.md"
test -f "$repository/.agents/records/XT-998.json"

printf 'Governance lifecycle test passed.\n'
