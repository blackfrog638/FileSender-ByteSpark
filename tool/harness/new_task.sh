#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ "$#" -lt 5 ]]; then
  printf '%s\n' \
    "Usage: $0 XT-008 task-slug workstream \\" \
    "  --owned 'path/**' [--owned path] [--depends-on XT-001]" >&2
  exit 2
fi

task_id="$1"
slug="$2"
workstream="$3"
shift 3

owned_paths=()
dependencies=()
while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --owned)
      [[ "$#" -ge 2 ]] || {
        printf '%s requires a path.\n' "$1" >&2
        exit 2
      }
      owned_paths+=("$2")
      shift 2
      ;;
    --depends-on)
      [[ "$#" -ge 2 ]] || {
        printf '%s requires a task id.\n' "$1" >&2
        exit 2
      }
      dependencies+=("$2")
      shift 2
      ;;
    *)
      printf 'Unknown argument: %s\n' "$1" >&2
      exit 2
      ;;
  esac
done

if [[ ! "$task_id" =~ ^XT-[0-9]{3,}$ ]]; then
  printf 'Task id must match XT-001.\n' >&2
  exit 2
fi
if [[ ! "$slug" =~ ^[a-z0-9]+(-[a-z0-9]+)*$ ]]; then
  printf 'Task slug must use lowercase kebab-case.\n' >&2
  exit 2
fi

case "$workstream" in
  integration | native_core | native_bridge | flutter_desktop | protocol | documentation) ;;
  *)
    printf 'Unknown workstream: %s\n' "$workstream" >&2
    exit 2
    ;;
esac

if [[ "${#owned_paths[@]}" -eq 0 ]]; then
  printf 'At least one --owned path is required.\n' >&2
  exit 2
fi
for dependency in "${dependencies[@]}"; do
  if [[ ! "$dependency" =~ ^XT-[0-9]{3,}$ ]]; then
    printf 'Invalid dependency: %s\n' "$dependency" >&2
    exit 2
  fi
done

destination="$root/.agents/tasks/$task_id-$slug.md"
record="$root/.agents/records/$task_id.json"
if [[ -e "$destination" || -e "$record" ]]; then
  printf 'Task already exists: %s\n' "$task_id" >&2
  exit 1
fi

dependencies_text="$(printf '%s\n' "${dependencies[@]:-}")"
owned_paths_text="$(printf '%s\n' "${owned_paths[@]}")"
DEPENDENCIES="$dependencies_text" OWNED_PATHS="$owned_paths_text" \
  python3 - \
    "$root" "$task_id" "$slug" "$workstream" "$destination" "$record" <<'PY'
import json
import os
import sys
from pathlib import Path

root_value, task_id, slug, workstream, spec_value, record_value = sys.argv[1:]
root = Path(root_value)
spec_path = Path(spec_value)
record_path = Path(record_value)
title = slug.replace("-", " ").capitalize()
dependencies = [
    value for value in os.environ["DEPENDENCIES"].splitlines() if value
]
owned_paths = [
    value for value in os.environ["OWNED_PATHS"].splitlines() if value
]

backlog_path = root / ".agents" / "backlog.yaml"
with backlog_path.open(encoding="utf-8") as source:
    backlog = json.load(source)
known = {task["id"] for task in backlog["tasks"]}
if task_id in known:
    raise SystemExit(f"Task is already in backlog: {task_id}")
unknown = sorted(set(dependencies) - known)
if unknown:
    raise SystemExit(f"Unknown dependencies: {unknown}")
backlog["tasks"].append(
    {
        "id": task_id,
        "title": title,
        "readiness": "ready",
        "workstream": workstream,
        "depends_on": dependencies,
        "owned_paths": owned_paths,
    }
)
backlog_path.write_text(
    json.dumps(backlog, indent=2) + "\n",
    encoding="utf-8",
)

def yaml_list(values: list[str]) -> str:
    if not values:
        return " []"
    return "\n" + "\n".join(f"  - {value}" for value in values)

spec_path.write_text(
    f"""---
id: {task_id}
title: {title}
state: ready
workstream: {workstream}
owner: unassigned
depends_on:{yaml_list(dependencies)}
owned_paths:{yaml_list(owned_paths)}
contract_changes: []
handoff: .agents/handoffs/{task_id}.md
---

## Outcome

TODO: describe one observable outcome.

## Context

TODO: link architecture, ADR, protocol, and predecessor tasks.

## Constraints

- TODO: define security, compatibility, platform, and performance constraints.

## Acceptance criteria

- [ ] Functional behavior and negative boundaries are covered.
- [ ] Public contracts and documentation are updated where required.
- [ ] Repository verification passes.

## Verification

```bash
make verify
```
""",
    encoding="utf-8",
)

record = {
    "schema_version": 1,
    "id": task_id,
    "task_type": "implementation",
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
            "rationale": "TODO: decide ADR impact before claim.",
        },
        "architecture": {
            "status": "not_required",
            "references": [],
            "rationale": "TODO: decide architecture impact before claim.",
        },
        "roadmap": {
            "status": "not_required",
            "references": [],
            "rationale": "TODO: decide roadmap impact before claim.",
        },
    },
    "integration": {"strategy": "", "mappings": [], "verified_sha": ""},
    "verification": {
        "status": "pending",
        "commands": ["make verify"],
        "reference": "",
    },
    "acceptance": {"accepted_by": "", "accepted_at": "", "note": ""},
}
record_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
PY

printf '%s\n' \
  "Created task catalogue entry, spec, and record for $task_id." \
  'Resolve every TODO and run agent.sh validate before committing or claiming.'
