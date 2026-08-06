#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
backlog="$root/.agents/backlog.yaml"
governance="$root/tool/harness/governance.py"
integration_branch="${XNN_TRANSFER_INTEGRATION_BRANCH:-harness}"

usage() {
  cat <<'EOF'
Usage:
  tool/harness/agent.sh list
  tool/harness/agent.sh validate
  tool/harness/agent.sh claim XT-001 owner-slug [worktree-path] [base-ref]
  tool/harness/agent.sh transition XT-001 in_progress|review|blocked
  tool/harness/agent.sh integrate XT-001
  tool/harness/agent.sh accept XT-001 reviewer-slug [verification-reference]
  tool/harness/agent.sh cleanup XT-001
  tool/harness/agent.sh prompt XT-001

Claims require a clean, committed base. Each task maps to one task/XT-NNN
branch and one worktree, so competing claims fail atomically.

Only integrate and accept can move review -> integrated -> done. Integration
uses cherry-pick -x and records source/result patch provenance.
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

task_worktree() {
  local task_id="$1"
  local branch
  branch="$(task_branch "$task_id")"
  git -C "$root" worktree list --porcelain |
    awk -v target="refs/heads/$branch" '
      $1 == "worktree" { path = $2 }
      $1 == "branch" && $2 == target { print path }
    '
}

ensure_integration_worktree() {
  local current
  current="$(git -C "$root" branch --show-current)"
  if [[ "$current" != "$integration_branch" ]]; then
    printf 'Run this command from the %s integration worktree, not %s.\n' \
      "$integration_branch" "${current:-detached HEAD}" >&2
    exit 1
  fi
}

commit_record() {
  local worktree="$1"
  local task_id="$2"
  local message="$3"
  git -C "$worktree" add ".agents/records/$task_id.json"
  git -C "$worktree" commit -m "$message" >/dev/null
}

run_verification() {
  local worktree="$1"
  local task_id="$2"
  local command
  while IFS= read -r command; do
    [[ -n "$command" ]] || continue
    printf '[verify:%s] %s\n' "$task_id" "$command"
    (
      cd "$worktree"
      bash -lc "$command"
    )
  done < <(
    "$worktree/tool/harness/governance.py" \
      verification-commands "$task_id"
  )
}

commit_patch_id() {
  local commit="$1"
  git -C "$root" show --pretty=format: --binary "$commit" |
    git -C "$root" patch-id --stable |
    awk '{ print $1 }'
}

list_tasks() {
  python3 - "$backlog" "$root" <<'PY'
import json
from pathlib import Path
import subprocess
import sys

path, root = sys.argv[1:]
with open(path, encoding="utf-8") as source:
    tasks = json.load(source)["tasks"]

print(f"{'TASK':<8} {'READINESS':<10} {'RUNTIME':<12} {'OWNER':<18} TITLE")
for task in tasks:
    branch = f"task/{task['id']}"
    record_path = Path(root) / ".agents" / "records" / f"{task['id']}.json"
    durable_record = None
    if record_path.is_file():
        durable_record = json.loads(record_path.read_text(encoding="utf-8"))
    exists = subprocess.run(
        ["git", "-C", root, "show-ref", "--verify", "--quiet",
         f"refs/heads/{branch}"],
        check=False,
    ).returncode == 0
    record = None
    if exists:
        record_result = subprocess.run(
            ["git", "-C", root, "show",
             f"{branch}:.agents/records/{task['id']}.json"],
            capture_output=True,
            check=False,
            text=True,
        )
        if record_result.returncode == 0:
            record = json.loads(record_result.stdout)
    if durable_record and durable_record.get("state") in {"integrated", "done"}:
        record = durable_record
    elif record is None:
        record = durable_record
    owner = record.get("owner", "") if record else ""
    state = record.get("state", "") if record else ""
    print(
        f"{task['id']:<8} {task['readiness']:<10} "
        f"{state:<12} {owner:<18} {task['title']}"
    )
PY
}

validate() {
  "$governance" validate

  local branch task_id owner state record_json record_state
  while IFS= read -r branch; do
    task_id="${branch#task/}"
    task_field "$task_id" title >/dev/null
    owner="$(task_config "$task_id" xnnOwner)"
    state="$(task_config "$task_id" xnnState)"
    if [[ -z "$owner" || -z "$state" ]]; then
      printf 'Claim %s is missing owner or runtime state.\n' "$task_id" >&2
      exit 1
    fi
    record_json="$(
      git -C "$root" show \
        "$branch:.agents/records/$task_id.json" 2>/dev/null ||
        true
    )"
    record_state=""
    if [[ -n "$record_json" ]]; then
      record_state="$(
        python3 -c \
          'import json,sys; print(json.load(sys.stdin)["state"])' \
          <<<"$record_json"
      )"
    fi
    if [[ -n "$record_state" && "$state" != "$record_state" && \
      !( "$record_state" == "review" && \
         ( "$state" == "integrated" || "$state" == "done" ) ) ]]; then
      printf '%s local state %s disagrees with branch record %s.\n' \
        "$task_id" "$state" "$record_state" >&2
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
  local readiness branch base_sha zero record_state

  ensure_integration_worktree
  readiness="$(task_field "$task_id" readiness)"
  if [[ "$readiness" != "ready" ]]; then
    printf '%s is not ready; readiness=%s\n' "$task_id" "$readiness" >&2
    exit 1
  fi
  record_state="$("$governance" get "$task_id" state)"
  if [[ "$record_state" != "ready" ]]; then
    printf '%s cannot be claimed; durable state=%s\n' \
      "$task_id" "$record_state" >&2
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
  "$destination/tool/harness/governance.py" \
    mark-claimed "$task_id" "$owner" "$base_sha"
  commit_record \
    "$destination" \
    "$task_id" \
    "harness: claim $task_id for $owner"

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
  local branch current allowed worktree head reference
  branch="$(task_branch "$task_id")"
  if ! git -C "$root" show-ref --verify --quiet "refs/heads/$branch"; then
    printf '%s is not claimed.\n' "$task_id" >&2
    exit 1
  fi

  worktree="$(task_worktree "$task_id")"
  if [[ -z "$worktree" ]]; then
    printf '%s has no task worktree.\n' "$task_id" >&2
    exit 1
  fi
  current="$(
    "$worktree/tool/harness/governance.py" get "$task_id" state
  )"
  case "$current:$next" in
    claimed:in_progress | in_progress:review | in_progress:blocked | \
      blocked:in_progress | review:in_progress)
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

  if [[ "$next" == "review" ]]; then
    head="$(
      "$worktree/tool/harness/governance.py" \
        prepare-review "$task_id"
    )"
    run_verification "$worktree" "$task_id"
    reference="local:$head:$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    "$worktree/tool/harness/governance.py" \
      mark-review "$task_id" "$head" "$reference"
  else
    if [[ -n "$(git -C "$worktree" status --porcelain)" ]]; then
      printf '%s worktree must be clean before a state transition.\n' \
        "$task_id" >&2
      exit 1
    fi
    "$worktree/tool/harness/governance.py" \
      update-state "$task_id" "$current" "$next"
  fi
  commit_record \
    "$worktree" \
    "$task_id" \
    "harness: move $task_id to $next"
  git -C "$root" config "branch.$branch.xnnState" "$next"
  printf '%s: %s -> %s\n' "$task_id" "$current" "$next"
}

integrate() {
  if [[ "$#" -ne 1 ]]; then
    usage
    exit 2
  fi

  local task_id="$1"
  local branch worktree state base source result source_patch result_patch
  local mapping_rows mapping_json
  branch="$(task_branch "$task_id")"

  ensure_integration_worktree
  if [[ -n "$(git -C "$root" status --porcelain)" ]]; then
    printf 'The integration worktree must be clean before integration.\n' >&2
    exit 1
  fi
  worktree="$(task_worktree "$task_id")"
  if [[ -z "$worktree" ]]; then
    printf '%s has no task worktree.\n' "$task_id" >&2
    exit 1
  fi
  if [[ -n "$(git -C "$worktree" status --porcelain)" ]]; then
    printf '%s worktree must be clean before integration.\n' "$task_id" >&2
    exit 1
  fi
  state="$("$worktree/tool/harness/governance.py" get "$task_id" state)"
  if [[ "$state" != "review" ]]; then
    printf '%s must be in review before integration; state=%s\n' \
      "$task_id" "$state" >&2
    exit 1
  fi
  base="$("$worktree/tool/harness/governance.py" get "$task_id" base_sha)"
  mapping_rows="$(mktemp)"
  mapping_json="$(mktemp)"

  while IFS= read -r source; do
    [[ -n "$source" ]] || continue
    if [[ "$(git -C "$root" rev-list --count "$source^@")" -ne 1 ]]; then
      printf 'Merge commits are not supported by cherry-pick integration: %s\n' \
        "$source" >&2
      rm -f "$mapping_rows" "$mapping_json"
      exit 1
    fi
    source_patch="$(commit_patch_id "$source")"
    if ! git -C "$root" cherry-pick -x "$source"; then
      printf '%s\n' \
        'Integration stopped on a conflict.' \
        'Resolve it in the integration worktree, then restart after restoring' \
        'a clean pre-integration state.' >&2
      rm -f "$mapping_rows" "$mapping_json"
      exit 1
    fi
    result="$(git -C "$root" rev-parse HEAD)"
    result_patch="$(commit_patch_id "$result")"
    if [[ "$source_patch" != "$result_patch" ]]; then
      printf 'Patch ID changed while integrating %s.\n' "$source" >&2
      rm -f "$mapping_rows" "$mapping_json"
      exit 1
    fi
    printf '%s\t%s\t%s\n' \
      "$source" "$result" "$source_patch" >>"$mapping_rows"
  done < <(git -C "$root" rev-list --reverse "$base..$branch")

  python3 - "$mapping_rows" "$mapping_json" <<'PY'
import json
import sys

rows_path, output_path = sys.argv[1:]
mappings = []
with open(rows_path, encoding="utf-8") as rows:
    for line in rows:
        source, result, patch_id = line.rstrip("\n").split("\t")
        mappings.append(
            {"source": source, "result": result, "patch_id": patch_id}
        )
with open(output_path, "w", encoding="utf-8") as output:
    json.dump({"mappings": mappings}, output)
PY
  "$governance" mark-integrated "$task_id" "$mapping_json"
  commit_record \
    "$root" \
    "$task_id" \
    "harness: record $task_id integration"
  git -C "$root" config "branch.$branch.xnnState" integrated
  rm -f "$mapping_rows" "$mapping_json"
  printf '%s: review -> integrated\n' "$task_id"
}

accept() {
  if [[ "$#" -lt 2 || "$#" -gt 3 ]]; then
    usage
    exit 2
  fi

  local task_id="$1"
  local reviewer="$2"
  local reference="${3:-}"
  local branch state verified_sha backup
  branch="$(task_branch "$task_id")"

  ensure_integration_worktree
  if [[ ! "$reviewer" =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]]; then
    printf 'Reviewer must be a filesystem-safe slug.\n' >&2
    exit 2
  fi
  if [[ -n "$(git -C "$root" status --porcelain)" ]]; then
    printf 'The integration worktree must be clean before acceptance.\n' >&2
    exit 1
  fi
  state="$("$governance" get "$task_id" state)"
  if [[ "$state" != "integrated" ]]; then
    printf '%s must be integrated before acceptance; state=%s\n' \
      "$task_id" "$state" >&2
    exit 1
  fi

  run_verification "$root" "$task_id"
  verified_sha="$(git -C "$root" rev-parse HEAD)"
  if [[ -z "$reference" ]]; then
    reference="local:$verified_sha:$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  fi
  backup="$(mktemp)"
  cp "$root/.agents/records/$task_id.json" "$backup"
  "$governance" mark-accepted \
    "$task_id" "$reviewer" "$reference" "$verified_sha"
  if ! "$governance" validate; then
    cp "$backup" "$root/.agents/records/$task_id.json"
    rm -f "$backup"
    printf 'Acceptance record failed governance validation.\n' >&2
    exit 1
  fi
  rm -f "$backup"
  commit_record \
    "$root" \
    "$task_id" \
    "harness: accept $task_id"
  git -C "$root" config "branch.$branch.xnnState" done
  printf '%s: integrated -> done\n' "$task_id"
}

cleanup() {
  if [[ "$#" -ne 1 ]]; then
    usage
    exit 2
  fi

  local task_id="$1"
  local branch state base worktree commits_file
  branch="$(task_branch "$task_id")"
  ensure_integration_worktree
  state="$("$governance" get "$task_id" state)"
  if [[ "$state" != "done" ]]; then
    printf '%s cannot be cleaned up; durable state=%s\n' \
      "$task_id" "$state" >&2
    exit 1
  fi
  if ! git -C "$root" show-ref --verify --quiet "refs/heads/$branch"; then
    printf '%s has no local task branch; nothing to clean.\n' "$task_id"
    return
  fi
  base="$("$governance" get "$task_id" base_sha)"
  commits_file="$(mktemp)"
  git -C "$root" rev-list "$base..$branch" >"$commits_file"
  if ! python3 - \
    "$root/.agents/records/$task_id.json" \
    "$commits_file" <<'PY'
import json
import sys

record_path, commits_path = sys.argv[1:]
with open(record_path, encoding="utf-8") as source:
    record = json.load(source)
mapped = {
    item["source"] for item in record["integration"]["mappings"]
}
with open(commits_path, encoding="utf-8") as source:
    commits = {line.strip() for line in source if line.strip()}
missing = sorted(commits - mapped)
if missing:
    raise SystemExit(
        "Task branch contains commits without integration mappings:\n"
        + "\n".join(missing)
    )
PY
  then
    rm -f "$commits_file"
    exit 1
  fi
  rm -f "$commits_file"

  worktree="$(task_worktree "$task_id")"
  if [[ -n "$worktree" ]]; then
    if [[ -n "$(git -C "$worktree" status --porcelain)" ]]; then
      printf '%s worktree is dirty; cleanup refused.\n' "$task_id" >&2
      exit 1
    fi
    git -C "$root" worktree remove "$worktree"
  fi
  git -C "$root" update-ref -d "refs/heads/$branch"
  git -C "$root" config --remove-section "branch.$branch" || true
  git -C "$root" worktree prune
  printf 'Cleaned up %s worktree and local task branch.\n' "$task_id"
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
    '提交代码和 handoff 后转 review；该命令会执行记录中的验证项。' \
    '只有 integration owner 能通过 integrate 和 accept 完成任务。'
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
  integrate) integrate "$@" ;;
  accept) accept "$@" ;;
  cleanup) cleanup "$@" ;;
  prompt) prompt "$@" ;;
  *)
    usage
    exit 2
    ;;
esac
