#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
backlog="$root/.agents/backlog.yaml"

usage() {
  cat <<'EOF'
Usage:
  tool/harness/agent.sh list
  tool/harness/agent.sh validate
  tool/harness/agent.sh claim XT-001 owner-slug [worktree-path] [base-ref]
  tool/harness/agent.sh transition XT-001 in_progress|review|done|blocked
  tool/harness/agent.sh prompt XT-001

Claims require a clean, committed base. Each task maps to one task/XT-NNN
branch and one worktree, so competing claims fail atomically.
EOF
}

task_field() {
  local task_id="$1"
  local field="$2"
  python3 - "$backlog" "$task_id" "$field" <<'PY'
import json
import sys

path, task_id, field = sys.argv[1:]
with open(path, encoding="utf-8") as source:
    tasks = json.load(source)["tasks"]
for task in tasks:
    if task["id"] == task_id:
        value = task[field]
        if isinstance(value, list):
            print("\n".join(value))
        else:
            print(value)
        raise SystemExit(0)
raise SystemExit(f"Unknown task: {task_id}")
PY
}

task_branch() {
  printf 'task/%s\n' "$1"
}

task_config() {
  local task_id="$1"
  local field="$2"
  git -C "$root" config --get "branch.$(task_branch "$task_id").$field" ||
    true
}

list_tasks() {
  python3 - "$backlog" "$root" <<'PY'
import json
import subprocess
import sys

path, root = sys.argv[1:]
with open(path, encoding="utf-8") as source:
    tasks = json.load(source)["tasks"]

print(f"{'TASK':<8} {'READINESS':<10} {'RUNTIME':<12} {'OWNER':<18} TITLE")
for task in tasks:
    branch = f"task/{task['id']}"
    exists = subprocess.run(
        ["git", "-C", root, "show-ref", "--verify", "--quiet",
         f"refs/heads/{branch}"],
        check=False,
    ).returncode == 0
    owner = ""
    state = ""
    if exists:
        owner_result = subprocess.run(
            ["git", "-C", root, "config", "--get",
             f"branch.{branch}.xnnOwner"],
            capture_output=True,
            check=False,
            text=True,
        )
        state_result = subprocess.run(
            ["git", "-C", root, "config", "--get",
             f"branch.{branch}.xnnState"],
            capture_output=True,
            check=False,
            text=True,
        )
        owner = owner_result.stdout.strip()
        state = state_result.stdout.strip() or "claimed"
    print(
        f"{task['id']:<8} {task['readiness']:<10} "
        f"{state:<12} {owner:<18} {task['title']}"
    )
PY
}

validate() {
  python3 - "$backlog" <<'PY'
import json
import re
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    document = json.load(source)

assert document["schema_version"] == 1
tasks = document["tasks"]
ids = [task["id"] for task in tasks]
if len(ids) != len(set(ids)):
    raise SystemExit("Duplicate task id")

known = set(ids)
for task in tasks:
    if not re.fullmatch(r"XT-[0-9]{3,}", task["id"]):
        raise SystemExit(f"Invalid task id: {task['id']}")
    if task["readiness"] not in {"ready", "blocked", "done"}:
        raise SystemExit(f"Invalid readiness for {task['id']}")
    unknown = set(task["depends_on"]) - known
    if unknown:
        raise SystemExit(
            f"{task['id']} has unknown dependencies: {sorted(unknown)}"
        )
    if not task["owned_paths"]:
        raise SystemExit(f"{task['id']} has no owned paths")

print(f"Agent backlog validation passed for {len(tasks)} tasks.")
PY

  local branch task_id owner state
  while IFS= read -r branch; do
    task_id="${branch#task/}"
    task_field "$task_id" title >/dev/null
    owner="$(task_config "$task_id" xnnOwner)"
    state="$(task_config "$task_id" xnnState)"
    if [[ -z "$owner" || -z "$state" ]]; then
      printf 'Claim %s is missing owner or runtime state.\n' "$task_id" >&2
      exit 1
    fi
  done < <(
    git -C "$root" for-each-ref \
      --format='%(refname:short)' \
      'refs/heads/task/XT-*'
  )
}

claim() {
  if [[ "$#" -lt 2 || "$#" -gt 4 ]]; then
    usage
    exit 2
  fi

  local task_id="$1"
  local owner="$2"
  local destination="${3:-$(dirname "$root")/XnnTransfer-$task_id}"
  local base_ref="${4:-HEAD}"
  local readiness branch base_sha zero

  readiness="$(task_field "$task_id" readiness)"
  if [[ "$readiness" != "ready" ]]; then
    printf '%s is not ready; readiness=%s\n' "$task_id" "$readiness" >&2
    exit 1
  fi
  if [[ ! "$owner" =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]]; then
    printf 'Owner must be a filesystem-safe slug.\n' >&2
    exit 2
  fi
  if [[ -n "$(git -C "$root" status --porcelain)" ]]; then
    printf '%s\n' \
      'The base worktree is not clean.' \
      'Commit the harness baseline before creating agent worktrees.' >&2
    exit 1
  fi
  if [[ -e "$destination" ]]; then
    printf 'Worktree path already exists: %s\n' "$destination" >&2
    exit 1
  fi

  branch="$(task_branch "$task_id")"
  base_sha="$(git -C "$root" rev-parse --verify "$base_ref^{commit}")"
  zero="0000000000000000000000000000000000000000"
  if ! git -C "$root" update-ref \
    "refs/heads/$branch" \
    "$base_sha" \
    "$zero"; then
    printf '%s is already claimed on branch %s.\n' "$task_id" "$branch" >&2
    exit 1
  fi

  git -C "$root" config "branch.$branch.xnnOwner" "$owner"
  git -C "$root" config "branch.$branch.xnnState" claimed
  if ! git -C "$root" worktree add "$destination" "$branch"; then
    git -C "$root" update-ref -d "refs/heads/$branch" "$base_sha"
    git -C "$root" config --remove-section "branch.$branch" || true
    exit 1
  fi

  printf 'Claimed %s for %s\n' "$task_id" "$owner"
  printf 'Worktree: %s\n' "$destination"
  printf 'Next: tool/harness/agent.sh prompt %s\n' "$task_id"
}

transition() {
  if [[ "$#" -ne 2 ]]; then
    usage
    exit 2
  fi

  local task_id="$1"
  local next="$2"
  local branch current allowed
  branch="$(task_branch "$task_id")"
  if ! git -C "$root" show-ref --verify --quiet "refs/heads/$branch"; then
    printf '%s is not claimed.\n' "$task_id" >&2
    exit 1
  fi

  current="$(task_config "$task_id" xnnState)"
  case "$current:$next" in
    claimed:in_progress | in_progress:review | in_progress:blocked | \
      blocked:in_progress | review:in_progress | review:done)
      allowed=1
      ;;
    *)
      allowed=0
      ;;
  esac
  if [[ "$allowed" -ne 1 ]]; then
    printf 'Illegal task transition: %s -> %s\n' "$current" "$next" >&2
    exit 1
  fi

  git -C "$root" config "branch.$branch.xnnState" "$next"
  printf '%s: %s -> %s\n' "$task_id" "$current" "$next"
}

prompt() {
  if [[ "$#" -ne 1 ]]; then
    usage
    exit 2
  fi

  local task_id="$1"
  local branch owner state worktree
  branch="$(task_branch "$task_id")"
  owner="$(task_config "$task_id" xnnOwner)"
  state="$(task_config "$task_id" xnnState)"
  worktree="$(
    git -C "$root" worktree list --porcelain |
      awk -v target="refs/heads/$branch" '
        $1 == "worktree" { path = $2 }
        $1 == "branch" && $2 == target { print path }
      '
  )"
  if [[ -z "$owner" || -z "$worktree" ]]; then
    printf '%s is not claimed in a worktree.\n' "$task_id" >&2
    exit 1
  fi

  printf '你负责 %s：%s。\n' "$task_id" "$(task_field "$task_id" title)"
  printf 'Owner: %s；runtime state: %s。\n' "$owner" "$state"
  printf '只在 worktree %s 中工作。\n' "$worktree"
  printf '先完整阅读 AGENTS.md、.agents/manifest.yaml、docs/architecture.md。\n'
  printf 'Owned paths:\n'
  task_field "$task_id" owned_paths | sed 's/^/- /'
  printf '%s\n' \
    '开始时执行 agent.sh transition <task> in_progress。' \
    '交付前运行聚焦测试和 make verify，填写 handoff，再转 review。'
}

command="${1:-}"
if [[ -z "$command" ]]; then
  usage
  exit 2
fi
shift

case "$command" in
  list) list_tasks "$@" ;;
  validate) validate "$@" ;;
  claim) claim "$@" ;;
  transition) transition "$@" ;;
  prompt) prompt "$@" ;;
  *)
    usage
    exit 2
    ;;
esac
