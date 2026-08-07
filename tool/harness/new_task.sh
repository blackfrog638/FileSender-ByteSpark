#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ "$#" -lt 5 ]]; then
  printf '%s\n' \
    "Usage: $0 XT-008 task-slug workstream \\" \
    "  --commit-type feat --commit-scope native \\" \
    "  --commit-summary 'implement concrete outcome' \\" \
    "  --architecture-mode none|add|replace|remove|refactor \\" \
    "  --owned 'path/**' [--owned path] [--depends-on XT-001]" >&2
  exit 2
fi

task_id="$1"
slug="$2"
workstream="$3"
shift 3

owned_paths=()
dependencies=()
commit_type=""
commit_scope=""
commit_summary=""
architecture_mode=""
architecture_modules=()
supersedes_paths=()
supersedes_symbols=()
supersedes_targets=()
retires_leases=()
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
    --commit-type)
      [[ "$#" -ge 2 ]] || {
        printf '%s requires a Conventional Commit type.\n' "$1" >&2
        exit 2
      }
      commit_type="$2"
      shift 2
      ;;
    --commit-scope)
      [[ "$#" -ge 2 ]] || {
        printf '%s requires a lowercase scope.\n' "$1" >&2
        exit 2
      }
      commit_scope="$2"
      shift 2
      ;;
    --commit-summary)
      [[ "$#" -ge 2 ]] || {
        printf '%s requires an imperative summary.\n' "$1" >&2
        exit 2
      }
      commit_summary="$2"
      shift 2
      ;;
    --architecture-mode)
      [[ "$#" -ge 2 ]] || {
        printf '%s requires an architecture change mode.\n' "$1" >&2
        exit 2
      }
      architecture_mode="$2"
      shift 2
      ;;
    --architecture-module)
      [[ "$#" -ge 2 ]] || {
        printf '%s requires a module id.\n' "$1" >&2
        exit 2
      }
      architecture_modules+=("$2")
      shift 2
      ;;
    --supersedes-path)
      [[ "$#" -ge 2 ]] || {
        printf '%s requires a repository path.\n' "$1" >&2
        exit 2
      }
      supersedes_paths+=("$2")
      shift 2
      ;;
    --supersedes-symbol)
      [[ "$#" -ge 2 ]] || {
        printf '%s requires path::symbol.\n' "$1" >&2
        exit 2
      }
      supersedes_symbols+=("$2")
      shift 2
      ;;
    --supersedes-target)
      [[ "$#" -ge 2 ]] || {
        printf '%s requires a CMake target.\n' "$1" >&2
        exit 2
      }
      supersedes_targets+=("$2")
      shift 2
      ;;
    --retires-lease)
      [[ "$#" -ge 2 ]] || {
        printf '%s requires a lease id.\n' "$1" >&2
        exit 2
      }
      retires_leases+=("$2")
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
case "$commit_type" in
  feat | fix | docs | style | refactor | perf | test | build | ci | chore | revert) ;;
  *)
    printf 'Invalid or missing Conventional Commit type.\n' >&2
    exit 2
    ;;
esac
if [[ ! "$commit_scope" =~ ^[a-z0-9][a-z0-9.-]*$ ]]; then
  printf 'Commit scope must be lowercase and URL-safe.\n' >&2
  exit 2
fi
if [[ ! "$commit_summary" =~ ^[a-z] ]] ||
  [[ "${#commit_summary}" -lt 12 ]] ||
  [[ "$commit_summary" =~ [.!?]$ ]] ||
  [[ "$commit_summary" =~ XT-[0-9]{3,} ]]; then
  printf '%s\n' \
    'Commit summary must start lowercase, contain at least 12 characters,' \
    'omit final punctuation, and contain no XT task ID.' >&2
  exit 2
fi
subject_length=$((
  ${#commit_type} + ${#commit_scope} + ${#commit_summary} + 4
))
if [[ "$subject_length" -gt 72 ]]; then
  printf 'Generated commit subject exceeds 72 characters.\n' >&2
  exit 2
fi
case "$architecture_mode" in
  none | add | replace | remove | refactor) ;;
  *)
    printf 'Invalid or missing architecture change mode.\n' >&2
    exit 2
    ;;
esac
if [[ "${#supersedes_symbols[@]}" -gt 0 ]]; then
  for symbol in "${supersedes_symbols[@]}"; do
    if [[ "$symbol" != *::* || "$symbol" == ::* || "$symbol" == *:: ]]; then
      printf 'Superseded symbol must use path::symbol: %s\n' "$symbol" >&2
      exit 2
    fi
  done
fi
title_summary="${slug//-/ }"
review_subject_length=$((
  ${#commit_scope} + ${#title_summary} + 27
))
if [[ "$review_subject_length" -gt 72 ]]; then
  printf 'Task title produces a lifecycle subject over 72 characters.\n' >&2
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
if [[ "${#dependencies[@]}" -gt 0 ]]; then
  for dependency in "${dependencies[@]}"; do
    if [[ ! "$dependency" =~ ^XT-[0-9]{3,}$ ]]; then
      printf 'Invalid dependency: %s\n' "$dependency" >&2
      exit 2
    fi
  done
fi

destination="$root/.agents/tasks/$task_id-$slug.md"
record="$root/.agents/records/$task_id.json"
if [[ -e "$destination" || -e "$record" ]]; then
  printf 'Task already exists: %s\n' "$task_id" >&2
  exit 1
fi

dependencies_text=""
if [[ "${#dependencies[@]}" -gt 0 ]]; then
  dependencies_text="$(printf '%s\n' "${dependencies[@]}")"
fi
owned_paths_text="$(printf '%s\n' "${owned_paths[@]}")"
architecture_modules_text=""
supersedes_paths_text=""
supersedes_symbols_text=""
supersedes_targets_text=""
retires_leases_text=""
if [[ "${#architecture_modules[@]}" -gt 0 ]]; then
  architecture_modules_text="$(printf '%s\n' "${architecture_modules[@]}")"
fi
if [[ "${#supersedes_paths[@]}" -gt 0 ]]; then
  supersedes_paths_text="$(printf '%s\n' "${supersedes_paths[@]}")"
fi
if [[ "${#supersedes_symbols[@]}" -gt 0 ]]; then
  supersedes_symbols_text="$(printf '%s\n' "${supersedes_symbols[@]}")"
fi
if [[ "${#supersedes_targets[@]}" -gt 0 ]]; then
  supersedes_targets_text="$(printf '%s\n' "${supersedes_targets[@]}")"
fi
if [[ "${#retires_leases[@]}" -gt 0 ]]; then
  retires_leases_text="$(printf '%s\n' "${retires_leases[@]}")"
fi
DEPENDENCIES="$dependencies_text" OWNED_PATHS="$owned_paths_text" \
  COMMIT_TYPE="$commit_type" COMMIT_SCOPE="$commit_scope" \
  COMMIT_SUMMARY="$commit_summary" ARCHITECTURE_MODE="$architecture_mode" \
  ARCHITECTURE_MODULES="$architecture_modules_text" \
  SUPERSEDES_PATHS="$supersedes_paths_text" \
  SUPERSEDES_SYMBOLS="$supersedes_symbols_text" \
  SUPERSEDES_TARGETS="$supersedes_targets_text" \
  RETIRES_LEASES="$retires_leases_text" \
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
commit = {
    "type": os.environ["COMMIT_TYPE"],
    "scope": os.environ["COMMIT_SCOPE"],
    "summary": os.environ["COMMIT_SUMMARY"],
}
architecture_change = {
    "mode": os.environ["ARCHITECTURE_MODE"],
    "modules": [
        value
        for value in os.environ["ARCHITECTURE_MODULES"].splitlines()
        if value
    ],
    "supersedes": {
        "paths": [
            value
            for value in os.environ["SUPERSEDES_PATHS"].splitlines()
            if value
        ],
        "symbols": [
            {"path": value.split("::", 1)[0], "name": value.split("::", 1)[1]}
            for value in os.environ["SUPERSEDES_SYMBOLS"].splitlines()
            if value
        ],
        "targets": [
            value
            for value in os.environ["SUPERSEDES_TARGETS"].splitlines()
            if value
        ],
    },
    "temporary_leases": [],
    "retires_leases": [
        value
        for value in os.environ["RETIRES_LEASES"].splitlines()
        if value
    ],
}

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
        "risk_profile_required": True,
        "commit_policy_required": True,
        "architecture_contract_required": True,
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

## Architecture change

The record declares `{architecture_change["mode"]}` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 2 risk dimension in the task record. Every
non-none risk must name commands that also appear in `verification.commands`.

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
    "schema_version": 2,
    "id": task_id,
    "task_type": "implementation",
    "state": "ready",
    "owner": "unassigned",
    "base_sha": "",
    "head_sha": "",
    "handoff": f".agents/handoffs/{task_id}.md",
    "commit": commit,
    "architecture_change": architecture_change,
    "risks": {
        "functionality": {
            "level": "medium",
            "rationale": "TODO: describe functional regression risk.",
            "gates": ["make verify"],
        },
        "security": {
            "level": "none",
            "rationale": "TODO: describe security risk or why it is absent.",
            "gates": [],
        },
        "performance": {
            "level": "none",
            "rationale": "TODO: describe performance risk or why it is absent.",
            "gates": [],
        },
        "compatibility": {
            "level": "none",
            "rationale": "TODO: describe compatibility risk or why it is absent.",
            "gates": [],
        },
        "concurrency": {
            "level": "none",
            "rationale": "TODO: describe concurrency risk or why it is absent.",
            "gates": [],
        },
        "platform": {
            "level": "none",
            "rationale": "TODO: describe platform risk or why it is absent.",
            "gates": [],
        },
        "persistence": {
            "level": "none",
            "rationale": "TODO: describe persistence risk or why it is absent.",
            "gates": [],
        },
    },
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
