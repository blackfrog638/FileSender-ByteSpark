#!/usr/bin/env bash

set -euo pipefail

export PYTHONDONTWRITEBYTECODE=1

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
python3 -B "$root/tool/harness/trusted_gates_test.py"
python3 -B "$root/tool/harness/defect_proof_test.py"
temporary="$(mktemp -d)"
repository="$temporary/repository"
task_id="XT-999"

cleanup() {
  rm -rf "$temporary"
}
trap cleanup EXIT

git clone --quiet --shared "$root" "$repository"
python3 -B \
  "$repository/tool/harness/commit_message.py" \
  configure \
  --root \
  "$repository"
git -C "$repository" checkout -B harness >/dev/null
inherited_head="$(git -C "$repository" rev-parse HEAD)"

python3 - "$repository" "$task_id" "$inherited_head" <<'PY'
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
task_id, inherited_head = sys.argv[2:]
sys.path.insert(0, str(root / "tool" / "harness"))
from trusted_gates import load_gate_registry

manifest_path = root / ".agents" / "manifest.yaml"
manifest = manifest_path.read_text(encoding="utf-8")
manifest = manifest.replace(
    "  verify: make verify\n",
    "  legacy_verify: make verify\n  verify: true\n",
)
manifest_path.write_text(manifest, encoding="utf-8")
gate_registry = load_gate_registry(manifest_path)
for path in sorted((root / ".agents" / "records").glob("XT-*.json")):
    record = json.loads(path.read_text(encoding="utf-8"))
    verification = record.get("verification", {})
    gates = verification.get("gates")
    if isinstance(gates, list) and "verify" in gates:
        expanded = []
        for gate in gates:
            if gate == "verify":
                expanded.extend(("legacy_verify", "verify"))
            else:
                expanded.append(gate)
        verification["gates"] = expanded
        verification["commands"] = [
            gate_registry[gate] for gate in expanded
        ]
        for risk in record.get("risks", {}).values():
            risk["gates"] = [
                "legacy_verify" if gate == "verify" else gate
                for gate in risk.get("gates", [])
            ]
    if record.get("state") == "integrated":
        integration = record["integration"]
        if (
            integration.get("strategy") == "squash"
            and not integration.get("result")
        ):
            integration["result"] = inherited_head
        integration["verified_sha"] = inherited_head
        record["state"] = "done"
        verification["status"] = "passed"
        verification["reference"] = "test:inherited-integration"
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
        "risk_profile_required": True,
        "commit_policy_required": True,
        "architecture_contract_required": True,
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
    "schema_version": 2,
    "id": task_id,
    "task_type": "test",
    "state": "ready",
    "owner": "unassigned",
    "base_sha": "",
    "head_sha": "",
    "handoff": f".agents/handoffs/{task_id}.md",
    "commit": {
        "type": "test",
        "scope": "harness",
        "summary": "exercise governance lifecycle",
    },
    "architecture_change": {
        "mode": "none",
        "modules": [],
        "supersedes": {"paths": [], "symbols": [], "targets": []},
        "temporary_leases": [],
        "retires_leases": [],
    },
    "risks": {
        "functionality": {
            "level": "low",
            "rationale": "The fixture must exercise one complete governed lifecycle.",
            "gates": ["verify"],
        },
        "security": {
            "level": "none",
            "rationale": "The isolated fixture changes no product security behavior.",
            "gates": [],
        },
        "performance": {
            "level": "none",
            "rationale": "The isolated fixture establishes no performance claim.",
            "gates": [],
        },
        "compatibility": {
            "level": "none",
            "rationale": "The fixture uses only the current harness contract.",
            "gates": [],
        },
        "concurrency": {
            "level": "none",
            "rationale": "The fixture runs one synchronous lifecycle.",
            "gates": [],
        },
        "platform": {
            "level": "none",
            "rationale": "The fixture does not claim product platform support.",
            "gates": [],
        },
        "persistence": {
            "level": "none",
            "rationale": "The fixture creates no product persisted state.",
            "gates": [],
        },
    },
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
        "gates": ["verify"],
        "commands": ["true"],
        "reference": "",
    },
    "acceptance": {"accepted_by": "", "accepted_at": "", "note": ""},
}
record_path = root / ".agents" / "records" / f"{task_id}.json"
record_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
PY

git -C "$repository" add \
  .agents/manifest.yaml \
  .agents/backlog.yaml \
  ".agents/tasks/$task_id-governance-lifecycle.md" \
  .agents/records
git -C "$repository" commit \
  -m "test(harness): register governance fixture" >/dev/null

"$repository/tool/harness/agent.sh" validate >/dev/null

python3 - "$repository" "$task_id" <<'PY'
import copy
import json
import subprocess
import sys
from pathlib import Path

root = Path(sys.argv[1])
task_id = sys.argv[2]
record_path = root / ".agents" / "records" / f"{task_id}.json"
valid = json.loads(record_path.read_text(encoding="utf-8"))
legacy = json.loads(
    (root / ".agents" / "records" / "XT-019.json").read_text(encoding="utf-8")
)
assert legacy["schema_version"] == 1


def remove_dimension(record):
    record["risks"].pop("platform")


def invalid_level(record):
    record["risks"]["security"]["level"] = "severe"


def placeholder_rationale(record):
    record["risks"]["security"]["rationale"] = "TODO: assess this risk"


def empty_gates(record):
    record["risks"]["functionality"]["gates"] = []


def duplicate_gates(record):
    record["risks"]["functionality"]["gates"] = ["verify", "verify"]


def unexecuted_gate(record):
    record["risks"]["functionality"]["gates"] = ["unknown"]


def gate_on_none(record):
    record["risks"]["security"]["gates"] = ["verify"]


def unregistered_command(record):
    record["verification"].pop("gates")
    record["verification"]["commands"] = ["false"]
    record["risks"]["functionality"]["gates"] = ["false"]


def trusted_gate_mismatch(record):
    record["verification"]["commands"] = ["false"]


def missing_verify_gate(record):
    record["verification"]["gates"] = ["commit_message_test"]
    record["verification"]["commands"] = ["make commit-message-test"]
    record["risks"]["functionality"]["gates"] = ["commit_message_test"]


def missing_architecture_change(record):
    record.pop("architecture_change")


def invalid_architecture_mode(record):
    record["architecture_change"]["mode"] = "expand"


def none_with_module(record):
    record["architecture_change"]["modules"] = ["tls"]


def unsafe_superseded_path(record):
    record["architecture_change"]["mode"] = "remove"
    record["architecture_change"]["supersedes"]["paths"] = ["../outside.cpp"]


cases = (
    ("missing dimension", remove_dimension, "is missing dimensions"),
    ("invalid level", invalid_level, "level must be one of"),
    ("placeholder rationale", placeholder_rationale, "still contains TODO"),
    ("empty gates", empty_gates, "gates must not be empty"),
    ("duplicate gates", duplicate_gates, "gates contains duplicates"),
    ("unexecuted gate", unexecuted_gate, "unexecuted command"),
    ("gate on none", gate_on_none, "gates must be empty"),
    ("unregistered command", unregistered_command, "unregistered command"),
    (
        "trusted gate mismatch",
        trusted_gate_mismatch,
        "commands must match trusted gates",
    ),
    (
        "missing verify gate",
        missing_verify_gate,
        "gates must include verify",
    ),
    (
        "missing architecture change",
        missing_architecture_change,
        "architecture_change must be an object",
    ),
    (
        "invalid architecture mode",
        invalid_architecture_mode,
        "mode must be one of",
    ),
    (
        "none mode with module",
        none_with_module,
        "mode none cannot declare",
    ),
    (
        "unsafe superseded path",
        unsafe_superseded_path,
        "has unsafe path",
    ),
)

try:
    for label, mutate, expected in cases:
        tampered = copy.deepcopy(valid)
        mutate(tampered)
        record_path.write_text(
            json.dumps(tampered, indent=2) + "\n",
            encoding="utf-8",
        )
        result = subprocess.run(
            [str(root / "tool" / "harness" / "governance.py"), "validate"],
            cwd=root,
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            raise SystemExit(f"Governance accepted {label}")
        if expected not in result.stderr:
            raise SystemExit(
                f"Governance rejected {label} for the wrong reason:\n"
                + result.stderr
            )
finally:
    record_path.write_text(
        json.dumps(valid, indent=2) + "\n",
        encoding="utf-8",
    )


def validate_candidate(candidate, label, expected=None):
    record_path.write_text(
        json.dumps(candidate, indent=2) + "\n",
        encoding="utf-8",
    )
    result = subprocess.run(
        [str(root / "tool" / "harness" / "governance.py"), "validate"],
        cwd=root,
        check=False,
        capture_output=True,
        text=True,
    )
    if expected is None and result.returncode != 0:
        raise SystemExit(
            f"Governance rejected valid schema v3 {label}:\n"
            + result.stderr
        )
    if expected is not None:
        if result.returncode == 0:
            raise SystemExit(f"Governance accepted invalid schema v3 {label}")
        if expected not in result.stderr:
            raise SystemExit(
                f"Governance rejected schema v3 {label} for the wrong reason:\n"
                + result.stderr
            )


bugfix = copy.deepcopy(valid)
bugfix["schema_version"] = 3
bugfix["task_type"] = "bugfix"
bugfix["defect"] = {
    "severity": "P1",
    "source": "test",
    "symptom": "The parser accepts input after a fatal frame error.",
    "expected_contract": "Fatal protocol errors terminate the parser.",
    "actual_behavior": "A later valid frame is accepted.",
    "trigger": "Feed a truncated frame followed by a valid frame.",
    "affected_since": "initial parser implementation",
    "proof_mode": "deterministic",
    "reproduction_commit": "",
    "regression_gate": "verify",
    "contract_disposition": "restore",
}

try:
    validate_candidate(bugfix, "bugfix")

    invalid = copy.deepcopy(bugfix)
    invalid["task_type"] = "incident"
    validate_candidate(invalid, "task type", "task_type must be one of")

    invalid = copy.deepcopy(bugfix)
    invalid["verification"].pop("gates")
    validate_candidate(
        invalid,
        "missing trusted gates",
        "verification.gates is required by schema version 3",
    )

    invalid = copy.deepcopy(bugfix)
    invalid.pop("defect")
    validate_candidate(invalid, "missing defect", "defect must be an object")

    invalid = copy.deepcopy(bugfix)
    invalid["defect"]["regression_command"] = "true"
    validate_candidate(invalid, "unknown defect field", "has unknown fields")

    invalid = copy.deepcopy(bugfix)
    invalid["defect"]["severity"] = "urgent"
    validate_candidate(invalid, "severity", "severity must be one of")

    invalid = copy.deepcopy(bugfix)
    invalid["defect"]["proof_mode"] = "hope"
    validate_candidate(invalid, "proof mode", "proof_mode must be one of")

    invalid = copy.deepcopy(bugfix)
    invalid["defect"]["regression_gate"] = "unknown"
    validate_candidate(
        invalid,
        "unregistered regression gate",
        "regression_gate is not registered",
    )

    invalid = copy.deepcopy(bugfix)
    invalid["defect"]["regression_gate"] = "commit_message_test"
    validate_candidate(
        invalid,
        "unexecuted regression gate",
        "regression_gate must appear in verification.gates",
    )

    invalid = copy.deepcopy(bugfix)
    invalid["defect"]["contract_disposition"] = "change"
    validate_candidate(
        invalid,
        "contract change without ADR",
        "contract-changing bugfix requires an ADR",
    )

    contract_change = copy.deepcopy(bugfix)
    contract_change["defect"]["contract_disposition"] = "change"
    contract_change["impacts"]["adr"] = {
        "required": True,
        "status": "accepted",
        "references": ["docs/adr/0011-defect-task-governance.md"],
        "rationale": "The reviewed ADR changes the governed contract.",
    }
    validate_candidate(contract_change, "contract change with ADR")

    invalid = copy.deepcopy(bugfix)
    invalid["state"] = "review"
    validate_candidate(
        invalid,
        "review without reproduction",
        "defect.reproduction_commit must be a non-empty string",
    )

    in_progress = copy.deepcopy(bugfix)
    in_progress["state"] = "in_progress"
    in_progress["owner"] = "test-agent"
    in_progress["base_sha"] = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    record_path.write_text(
        json.dumps(in_progress, indent=2) + "\n",
        encoding="utf-8",
    )
    result = subprocess.run(
        [
            str(root / "tool" / "harness" / "governance.py"),
            "mark-review",
            task_id,
            in_progress["base_sha"],
            "test:missing-reproduction",
        ],
        cwd=root,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode == 0:
        raise SystemExit("mark-review accepted a missing reproduction commit")
    if "reproduction commit must be a full lowercase SHA" not in result.stderr:
        raise SystemExit(
            "mark-review rejected a missing reproduction for the wrong reason:\n"
            + result.stderr
        )
    restored = json.loads(record_path.read_text(encoding="utf-8"))
    if restored["state"] != "in_progress":
        raise SystemExit("mark-review did not restore the rejected record")

    feature = copy.deepcopy(bugfix)
    feature["task_type"] = "feature"
    validate_candidate(
        feature,
        "feature with defect",
        "defect is only valid for task_type bugfix",
    )

    investigation = copy.deepcopy(valid)
    investigation["schema_version"] = 3
    investigation["task_type"] = "investigation"
    investigation["investigation"] = {
        "question": "Can the shutdown callback outlive its service?",
        "scope": "Discovery timer and interface monitor callbacks.",
        "evidence_required": "Sanitizer trace or a lifetime proof.",
        "exit_criteria": "Classify the report as a bugfix, feature, or no change.",
        "outcome_disposition": "pending",
    }
    validate_candidate(investigation, "investigation")
    investigation["state"] = "review"
    validate_candidate(
        investigation,
        "pending investigation review",
        "outcome_disposition cannot remain pending at review",
    )
finally:
    record_path.write_text(
        json.dumps(valid, indent=2) + "\n",
        encoding="utf-8",
    )
PY

git -C "$repository" branch task/XT-998
"$repository/tool/harness/agent.sh" validate >/dev/null
git -C "$repository" branch -D task/XT-998 >/dev/null
if [[ -n "$(git -C "$repository" status --short)" ]]; then
  printf 'Dirty governance fixture before claim:\n' >&2
  git -C "$repository" status --short >&2
fi
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
git -C "$task_worktree" commit \
  -m "test(harness): deliver governance fixture" >/dev/null
git -C "$task_worktree" add ".agents/handoffs/$task_id.md"
git -C "$task_worktree" commit \
  -m "docs(harness): document governance fixture handoff" >/dev/null

"$repository/tool/harness/agent.sh" \
  transition "$task_id" review >/dev/null
printf 'unreviewed payload\n' \
  >"$task_worktree/protocol/testdata/governance-fixture.txt"
git -C "$task_worktree" add protocol/testdata/governance-fixture.txt
git -C "$task_worktree" commit \
  -m "test(harness): append unreviewed fixture payload" >/dev/null
post_review_errors="$temporary/post-review-errors.txt"
if "$repository/tool/harness/agent.sh" \
  integrate "$task_id" >/dev/null 2>"$post_review_errors"; then
  printf 'Integration accepted payload added after review.\n' >&2
  exit 1
fi
grep -q 'branch tip is not its immutable review commit' "$post_review_errors"
"$repository/tool/harness/agent.sh" \
  transition "$task_id" in_progress >/dev/null
printf 'governance fixture\n' \
  >"$task_worktree/protocol/testdata/governance-fixture.txt"
git -C "$task_worktree" add protocol/testdata/governance-fixture.txt
git -C "$task_worktree" commit \
  -m "test(harness): restore reviewed fixture payload" >/dev/null
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
)" = "test(harness): exercise governance lifecycle"
python3 - "$repository" "$delivery" "$task_id" <<'PY'
import subprocess
import sys

root, delivery, task_id = sys.argv[1:]
message = subprocess.run(
    ["git", "-C", root, "show", "-s", "--format=%B", delivery],
    check=True,
    capture_output=True,
    text=True,
).stdout
lines = message.rstrip().splitlines()
assert lines[0] == "test(harness): exercise governance lifecycle"
assert f"Xnn-Task: {task_id}" in lines
assert "Xnn-Lifecycle: delivery" in lines
assert lines.count(f"Xnn-Task: {task_id}") == 1
PY

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
test "$(
  git -C "$repository" show -s --format=%s "$acceptance"
)" = "chore(harness): accept exercise governance lifecycle"
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

python3 - \
  "$repository/tool/harness/governance.py" \
  "$repository" \
  "$task_id" <<'PY'
import importlib.util
import sys
from pathlib import Path

module_path, repository, task_id = sys.argv[1:]
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
try:
    module.validate_architecture_review(
        {"id": task_id, "architecture_contract_required": True},
        {
            "architecture_change": {
                "mode": "none",
                "modules": [],
                "supersedes": {"paths": [], "symbols": [], "targets": []},
            }
        },
        Path(repository),
        ["native/src/security/tls/provider.cpp"],
    )
except module.GovernanceError as error:
    assert "undeclared affected modules: tls" in str(error)
else:
    raise AssertionError("Architecture review accepted an undeclared module")
try:
    module.validate_architecture_review(
        {"id": task_id, "architecture_contract_required": True},
        {
            "architecture_change": {
                "mode": "add",
                "modules": [],
                "supersedes": {"paths": [], "symbols": [], "targets": []},
            }
        },
        Path(repository),
        ["native/src/parallel/provider.cpp"],
    )
except module.GovernanceError as error:
    assert "no canonical module" in str(error)
else:
    raise AssertionError("Architecture review accepted an unowned source path")
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

if "$repository/tool/harness/new_task.sh" \
  XT-997 invalid-type-fixture integration \
  --task-type incident \
  --commit-type test \
  --commit-scope harness \
  --commit-summary 'reject invalid generated task type' \
  --architecture-mode none \
  --owned '.agents/handoffs/XT-997.md' >/dev/null 2>&1; then
  printf 'Task generator accepted an invalid task type.\n' >&2
  exit 1
fi

"$repository/tool/harness/new_task.sh" \
  XT-998 no-dependency-fixture integration \
  --commit-type test \
  --commit-scope harness \
  --commit-summary 'exercise generated task governance' \
  --architecture-mode none \
  --owned '.agents/handoffs/XT-998.md' >/dev/null
test -f "$repository/.agents/tasks/XT-998-no-dependency-fixture.md"
test -f "$repository/.agents/records/XT-998.json"

"$repository/tool/harness/new_task.sh" \
  XT-997 bugfix-fixture integration \
  --task-type bugfix \
  --commit-type fix \
  --commit-scope harness \
  --commit-summary 'exercise generated bugfix governance' \
  --architecture-mode none \
  --owned '.agents/handoffs/XT-997.md' >/dev/null
test -f "$repository/.agents/tasks/XT-997-bugfix-fixture.md"
test -f "$repository/.agents/records/XT-997.json"

python3 - "$repository" <<'PY'
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
with (root / ".agents" / "backlog.yaml").open(encoding="utf-8") as source:
    backlog = json.load(source)
task = next(item for item in backlog["tasks"] if item["id"] == "XT-998")
assert task["risk_profile_required"] is True
assert task["commit_policy_required"] is True
assert task["architecture_contract_required"] is True

record = json.loads(
    (root / ".agents" / "records" / "XT-998.json").read_text(encoding="utf-8")
)
assert record["schema_version"] == 3
assert record["task_type"] == "feature"
assert record["commit"] == {
    "type": "test",
    "scope": "harness",
    "summary": "exercise generated task governance",
}
assert record["architecture_change"] == {
    "mode": "none",
    "modules": [],
    "supersedes": {"paths": [], "symbols": [], "targets": []},
    "temporary_leases": [],
    "retires_leases": [],
}
assert set(record["risks"]) == {
    "functionality",
    "security",
    "performance",
    "compatibility",
    "concurrency",
    "platform",
    "persistence",
}
assert record["risks"]["functionality"]["gates"] == ["verify"]
assert record["verification"]["gates"] == ["verify"]
assert record["verification"]["commands"] == ["true"]
assert all(
    "TODO" in risk["rationale"] for risk in record["risks"].values()
)
spec = (
    root / ".agents" / "tasks" / "XT-998-no-dependency-fixture.md"
).read_text(encoding="utf-8")
assert "## Risk profile" in spec
assert "## Architecture change" in spec

bugfix = json.loads(
    (root / ".agents" / "records" / "XT-997.json").read_text(encoding="utf-8")
)
assert bugfix["schema_version"] == 3
assert bugfix["task_type"] == "bugfix"
assert set(bugfix["defect"]) == {
    "severity",
    "source",
    "symptom",
    "expected_contract",
    "actual_behavior",
    "trigger",
    "affected_since",
    "proof_mode",
    "reproduction_commit",
    "regression_gate",
    "contract_disposition",
}
assert bugfix["defect"]["reproduction_commit"] == ""
assert "TODO" in bugfix["defect"]["severity"]
bugfix_spec = (
    root / ".agents" / "tasks" / "XT-997-bugfix-fixture.md"
).read_text(encoding="utf-8")
assert "## Defect contract" in bugfix_spec
PY

generated_errors="$temporary/generated-task-errors.txt"
if "$repository/tool/harness/agent.sh" \
  validate >/dev/null 2>"$generated_errors"; then
  printf 'Generated task passed before risk placeholders were resolved.\n' >&2
  exit 1
fi
grep -q \
  'XT-998 task spec still contains TODO' \
  "$generated_errors"
grep -q \
  'XT-998.risks.functionality.rationale still contains TODO' \
  "$generated_errors"

printf 'Governance lifecycle test passed.\n'
