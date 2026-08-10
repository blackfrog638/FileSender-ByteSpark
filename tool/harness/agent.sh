#!/usr/bin/env bash

set -euo pipefail

script_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
root="$(cd "${XNN_TRANSFER_ROOT:-$script_root}" && pwd)"
backlog="$root/.agents/backlog.yaml"
governance="$root/tool/harness/governance.py"
github_ci="$root/tool/harness/github_ci.py"
integration_branch="${XNN_TRANSFER_INTEGRATION_BRANCH:-harness}"
remote="origin"
claim_lock_ref="refs/xnn-transfer/locks/claim"
claim_lock_token=""

usage() {
  cat <<'EOF'
Usage:
  tool/harness/agent.sh list
  tool/harness/agent.sh validate
  tool/harness/agent.sh claim XT-001 owner-slug [worktree-path] [base-ref]
  tool/harness/agent.sh transition XT-001 in_progress|review|blocked
  tool/harness/agent.sh integrate XT-001 [--strategy squash|cherry-pick]
  tool/harness/agent.sh integrate XT-001 --continue
  tool/harness/agent.sh accept XT-001 reviewer-slug
  tool/harness/agent.sh cleanup XT-001
  tool/harness/agent.sh prompt XT-001

Claims require a clean, committed base. Each task maps to one task/XT-NNN
branch and one worktree, so competing claims fail atomically.

Only integrate and accept can move review -> integrated -> done. Integration
defaults to one squash delivery commit with aggregate patch provenance.
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

task_commit_metadata() {
  local task_id="$1"
  python3 - "$backlog" "$root/.agents/records/$task_id.json" "$task_id" <<'PY'
import json
import sys

backlog_path, record_path, task_id = sys.argv[1:]
with open(backlog_path, encoding="utf-8") as source:
    task = next(
        item for item in json.load(source)["tasks"] if item["id"] == task_id
    )
with open(record_path, encoding="utf-8") as source:
    record = json.load(source)

title = task["title"]
title_summary = title[:1].lower() + title[1:]
commit = record.get("commit")
if not isinstance(commit, dict):
    type_by_workstream = {
        "documentation": "docs",
        "integration": "ci",
    }
    scope_by_workstream = {
        "documentation": "docs",
        "flutter_desktop": "desktop",
        "integration": "harness",
        "native_bridge": "abi",
        "native_core": "native",
        "protocol": "protocol",
    }
    commit = {
        "type": type_by_workstream.get(task["workstream"], "feat"),
        "scope": scope_by_workstream[task["workstream"]],
        "summary": title_summary,
    }
print(
    "\t".join(
        (
            commit["type"],
            commit["scope"],
            commit["summary"],
            title_summary,
        )
    )
)
PY
}

task_delivery_subject() {
  local task_id="$1"
  local commit_type scope summary title
  IFS=$'\t' read -r commit_type scope summary title < <(
    task_commit_metadata "$task_id"
  )
  printf '%s(%s): %s\n' "$commit_type" "$scope" "$summary"
}

task_lifecycle_subject() {
  local task_id="$1"
  local lifecycle="$2"
  local commit_type scope summary title verb suffix
  IFS=$'\t' read -r commit_type scope summary title < <(
    task_commit_metadata "$task_id"
  )
  suffix=""
  case "$lifecycle" in
    claim) verb="claim" ;;
    start) verb="start" ;;
    resume) verb="resume" ;;
    review)
      verb="submit"
      suffix=" for review"
      ;;
    blocked) verb="block" ;;
    integration)
      verb="record"
      suffix=" integration"
      ;;
    acceptance) verb="accept" ;;
    *)
      printf 'Unknown commit lifecycle: %s\n' "$lifecycle" >&2
      exit 2
      ;;
  esac
  printf 'chore(%s): %s %s%s\n' "$scope" "$verb" "$title" "$suffix"
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

release_claim_lock() {
  local current
  if [[ -z "$claim_lock_token" ]]; then
    return
  fi
  current="$(
    git -C "$root" rev-parse --verify "$claim_lock_ref" 2>/dev/null ||
      true
  )"
  if [[ "$current" == "$claim_lock_token" ]]; then
    git -C "$root" update-ref -d "$claim_lock_ref" "$claim_lock_token"
  fi
  claim_lock_token=""
}

acquire_claim_lock() {
  local attempt existing owner zero
  zero="0000000000000000000000000000000000000000"
  claim_lock_token="$(
    printf '%s\n' "$$" |
      git -C "$root" hash-object -w --stdin
  )"
  for attempt in 1 2 3; do
    if git -C "$root" update-ref \
      "$claim_lock_ref" "$claim_lock_token" "$zero" 2>/dev/null; then
      trap release_claim_lock EXIT
      trap 'exit 129' HUP
      trap 'exit 130' INT
      trap 'exit 143' TERM
      return
    fi
    existing="$(
      git -C "$root" rev-parse --verify "$claim_lock_ref" 2>/dev/null ||
        true
    )"
    owner="$(
      git -C "$root" cat-file blob "$existing" 2>/dev/null ||
        true
    )"
    if [[ "$owner" =~ ^[0-9]+$ ]] && kill -0 "$owner" 2>/dev/null; then
      printf 'Another claim is active in this clone (pid %s).\n' \
        "$owner" >&2
      exit 1
    fi
    if [[ -n "$existing" ]]; then
      git -C "$root" update-ref -d "$claim_lock_ref" "$existing" ||
        continue
    fi
  done
  printf 'Cannot acquire the task claim lock.\n' >&2
  exit 1
}

commit_record() {
  local worktree="$1"
  local task_id="$2"
  local lifecycle="$3"
  local subject="$4"
  git -C "$worktree" add ".agents/records/$task_id.json"
  git -C "$worktree" commit \
    -m "$subject" \
    -m "Xnn-Task: $task_id
Xnn-Lifecycle: $lifecycle" >/dev/null
}

run_verification() {
  local worktree="$1"
  local task_id="$2"
  local command commands_file
  commands_file="$(mktemp)"
  if ! "$worktree/tool/harness/governance.py" \
    verification-commands "$task_id" >"$commands_file"; then
    rm -f "$commands_file"
    return 1
  fi
  while IFS= read -r command; do
    [[ -n "$command" ]] || continue
    printf '[verify:%s] %s\n' "$task_id" "$command"
    if ! (
      cd "$worktree"
      bash -lc "$command"
    ); then
      rm -f "$commands_file"
      return 1
    fi
  done <"$commands_file"
  rm -f "$commands_file"
}

commit_patch_id() {
  local commit="$1"
  git -C "$root" show --pretty=format: --binary "$commit" |
    git -C "$root" patch-id --stable |
    awk '{ print $1 }'
}

range_patch_id() {
  local base="$1"
  local head="$2"
  local excluded_path="$3"
  git -C "$root" diff --binary "$base" "$head" -- \
    . ":(exclude)$excluded_path" |
    git -C "$root" patch-id --stable |
    awk '{ print $1 }'
}

index_patch_id() {
  local base="$1"
  local excluded_path="$2"
  git -C "$root" diff --cached --binary "$base" -- \
    . ":(exclude)$excluded_path" |
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
    if ! task_field "$task_id" title >/dev/null 2>&1; then
      continue
    fi
    owner="$(task_config "$task_id" xnnOwner)"
    state="$(task_config "$task_id" xnnState)"
    if [[ -z "$owner" && -z "$state" ]]; then
      continue
    fi
    if [[ -z "$owner" || -z "$state" ]]; then
      printf 'Local scheduler cache for %s is incomplete.\n' "$task_id" >&2
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
  "$governance" validate-claim "$task_id"

  branch="$(task_branch "$task_id")"
  base_sha="$(git -C "$root" rev-parse --verify "$base_ref^{commit}")"
  acquire_claim_lock
  python3 -B "$root/tool/harness/task_conflicts.py" \
    --root "$root" claim "$task_id"
  python3 -B "$root/tool/harness/task_conflicts.py" \
    --root "$root" stale "$task_id" "$base_sha" "$integration_branch"
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
    claim \
    "$(task_lifecycle_subject "$task_id" claim)"
  release_claim_lock
  trap - EXIT HUP INT TERM

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
  local branch current allowed worktree head reference lifecycle base
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
    base="$(
      "$worktree/tool/harness/governance.py" get "$task_id" base_sha
    )"
    python3 -B "$worktree/tool/harness/task_conflicts.py" \
      --root "$worktree" claim "$task_id"
    python3 -B "$worktree/tool/harness/task_conflicts.py" \
      --root "$worktree" stale "$task_id" "$base" "$integration_branch"
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
  case "$next" in
    in_progress)
      if [[ "$current" == "claimed" ]]; then
        lifecycle="start"
      else
        lifecycle="resume"
      fi
      ;;
    review) lifecycle="review" ;;
    blocked) lifecycle="blocked" ;;
  esac
  commit_record \
    "$worktree" \
    "$task_id" \
    "$lifecycle" \
    "$(task_lifecycle_subject "$task_id" "$lifecycle")"
  git -C "$root" config "branch.$branch.xnnState" "$next"
  printf '%s: %s -> %s\n' "$task_id" "$current" "$next"
}

integrate_cherry_pick() {
  local task_id="$1"
  local branch="$2"
  local base="$3"
  local source result source_patch result_patch mapping_rows provenance_json
  mapping_rows="$(mktemp)"
  provenance_json="$(mktemp)"

  while IFS= read -r source; do
    [[ -n "$source" ]] || continue
    if [[ "$(
      git -C "$root" rev-list --parents -n 1 "$source" |
        awk '{ print NF - 1 }'
    )" -ne 1 ]]; then
      printf 'Merge commits are not supported by integration: %s\n' \
        "$source" >&2
      rm -f "$mapping_rows" "$provenance_json"
      exit 1
    fi
    source_patch="$(commit_patch_id "$source")"
    if ! git -C "$root" cherry-pick -x "$source"; then
      printf '%s\n' \
        'Cherry-pick integration stopped on a conflict.' \
        'Restore a clean pre-integration state before retrying.' >&2
      rm -f "$mapping_rows" "$provenance_json"
      exit 1
    fi
    result="$(git -C "$root" rev-parse HEAD)"
    result_patch="$(commit_patch_id "$result")"
    if [[ "$source_patch" != "$result_patch" ]]; then
      printf 'Patch ID changed while integrating %s.\n' "$source" >&2
      rm -f "$mapping_rows" "$provenance_json"
      exit 1
    fi
    printf '%s\t%s\t%s\n' \
      "$source" "$result" "$source_patch" >>"$mapping_rows"
  done < <(git -C "$root" rev-list --reverse "$base..$branch")

  python3 - "$mapping_rows" "$provenance_json" <<'PY'
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
    json.dump(
        {
            "strategy": "cherry-pick",
            "mappings": mappings,
            "verified_sha": "",
        },
        output,
    )
PY
  "$governance" mark-integrated "$task_id" "$provenance_json"
  commit_record \
    "$root" \
    "$task_id" \
    integration \
    "$(task_lifecycle_subject "$task_id" integration)"
  rm -f "$mapping_rows" "$provenance_json"
}

integrate_squash() {
  local task_id="$1"
  local branch="$2"
  local base="$3"
  local continue_integration="$4"
  local record_relative source_head source_patch staged_patch result_patch
  local source_commits_sha256
  local integration_parent result source_commits provenance_json source
  record_relative=".agents/records/$task_id.json"
  source_head="$(git -C "$root" rev-parse "$branch")"
  source_commits="$(mktemp)"
  provenance_json="$(mktemp)"

  if ! git -C "$root" merge-base --is-ancestor "$base" "$source_head"; then
    printf '%s base is not an ancestor of its source head.\n' "$task_id" >&2
    rm -f "$source_commits" "$provenance_json"
    exit 1
  fi
  git -C "$root" rev-list --reverse \
    "$base..$source_head" >"$source_commits"
  if [[ ! -s "$source_commits" ]]; then
    printf '%s has no source commits to integrate.\n' "$task_id" >&2
    rm -f "$source_commits" "$provenance_json"
    exit 1
  fi
  while IFS= read -r source; do
    if [[ "$(
      git -C "$root" rev-list --parents -n 1 "$source" |
        awk '{ print NF - 1 }'
    )" -ne 1 ]]; then
      printf 'Merge commits are not supported by integration: %s\n' \
        "$source" >&2
      rm -f "$source_commits" "$provenance_json"
      exit 1
    fi
  done <"$source_commits"
  source_commits_sha256="$(
    python3 - "$source_commits" <<'PY'
import hashlib
import sys
from pathlib import Path

commits = [
    line.strip()
    for line in Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
    if line.strip()
]
encoded = ("\n".join(commits) + "\n").encode("ascii")
print(hashlib.sha256(encoded).hexdigest())
PY
  )"

  source_patch="$(range_patch_id "$base" "$source_head" "$record_relative")"
  if [[ -z "$source_patch" ]]; then
    printf '%s source range has no payload patch ID.\n' "$task_id" >&2
    rm -f "$source_commits" "$provenance_json"
    exit 1
  fi
  integration_parent="$(git -C "$root" rev-parse HEAD)"
  if ! git -C "$root" merge-base --is-ancestor "$base" "$integration_parent"; then
    printf '%s base is not an ancestor of the integration branch.\n' \
      "$task_id" >&2
    rm -f "$source_commits" "$provenance_json"
    exit 1
  fi

  if [[ "$continue_integration" -eq 0 ]]; then
    if ! git -C "$root" merge --squash --no-commit "$branch"; then
      printf '%s\n' \
        'Squash integration stopped on a conflict.' \
        "Resolve and stage every conflict, then run:" \
        "  tool/harness/agent.sh integrate $task_id --continue" >&2
      rm -f "$source_commits" "$provenance_json"
      exit 1
    fi
  else
    if git -C "$root" diff --cached --quiet; then
      printf 'No staged squash integration is available to continue.\n' >&2
      rm -f "$source_commits" "$provenance_json"
      exit 1
    fi
    if [[ -n "$(git -C "$root" diff --name-only --diff-filter=U)" ]]; then
      printf 'Squash integration still has unresolved conflicts.\n' >&2
      rm -f "$source_commits" "$provenance_json"
      exit 1
    fi
    if ! git -C "$root" diff --quiet; then
      printf 'Stage every squash integration change before continuing.\n' >&2
      rm -f "$source_commits" "$provenance_json"
      exit 1
    fi
    if [[ -n "$(
      git -C "$root" ls-files --others --exclude-standard
    )" ]]; then
      printf 'Remove unrelated untracked files before continuing.\n' >&2
      rm -f "$source_commits" "$provenance_json"
      exit 1
    fi
  fi

  staged_patch="$(
    index_patch_id "$integration_parent" "$record_relative"
  )"
  if [[ "$staged_patch" != "$source_patch" ]]; then
    printf '%s\n' \
      'Squash payload patch ID does not match the reviewed source range.' \
      "source: $source_patch" \
      "staged: ${staged_patch:-empty}" >&2
    rm -f "$source_commits" "$provenance_json"
    exit 1
  fi

  python3 - \
    "$source_commits" \
    "$provenance_json" \
    "$base" \
    "$source_head" \
    "$source_commits_sha256" \
    "$source_patch" <<'PY'
import json
import sys

(
    commits_path,
    output_path,
    source_base,
    source_head,
    commits_sha256,
    patch_id,
) = sys.argv[1:]
with open(commits_path, encoding="utf-8") as source:
    commits = [line.strip() for line in source if line.strip()]
with open(output_path, "w", encoding="utf-8") as output:
    json.dump(
        {
            "strategy": "squash",
            "source_base": source_base,
            "source_head": source_head,
            "source_commits": commits,
            "source_commits_sha256": commits_sha256,
            "source_patch_id": patch_id,
            "result": "",
            "result_patch_id": patch_id,
            "verified_sha": "",
        },
        output,
    )
PY
  "$governance" mark-integrated "$task_id" "$provenance_json"
  git -C "$root" add "$record_relative"
  if ! git -C "$root" diff --quiet; then
    printf 'Integration left unstaged changes after recording provenance.\n' >&2
    rm -f "$source_commits" "$provenance_json"
    exit 1
  fi
  git -C "$root" commit \
    -m "$(task_delivery_subject "$task_id")" \
    -m "Xnn-Task: $task_id
Xnn-Lifecycle: delivery
Xnn-Integration-Strategy: squash
Xnn-Source-Base: $base
Xnn-Source-Head: $source_head
Xnn-Source-Commits-SHA256: $source_commits_sha256
Xnn-Source-Patch-Id: $source_patch" >/dev/null

  result="$(git -C "$root" rev-parse HEAD)"
  result_patch="$(
    range_patch_id "$integration_parent" "$result" "$record_relative"
  )"
  if [[ "$result_patch" != "$source_patch" ]]; then
    printf 'Committed squash payload patch ID changed for %s.\n' \
      "$task_id" >&2
    rm -f "$source_commits" "$provenance_json"
    exit 1
  fi
  "$governance" validate >/dev/null
  rm -f "$source_commits" "$provenance_json"
  printf 'Integrated delivery: %s\n' "$result"
}

integrate() {
  if [[ "$#" -lt 1 || "$#" -gt 3 ]]; then
    usage
    exit 2
  fi

  local task_id="$1"
  local strategy="squash"
  local continue_integration=0
  local branch worktree state base head pending reviewed_patch current_patch
  local review_tip review_parent review_paths review_lifecycle
  shift
  if [[ "$#" -gt 0 ]]; then
    case "$1" in
      --continue)
        [[ "$#" -eq 1 ]] || {
          usage
          exit 2
        }
        continue_integration=1
        ;;
      --strategy)
        [[ "$#" -eq 2 ]] || {
          usage
          exit 2
        }
        strategy="$2"
        ;;
      *)
        usage
        exit 2
        ;;
    esac
  fi
  if [[ "$strategy" != "squash" && "$strategy" != "cherry-pick" ]]; then
    printf 'Unknown integration strategy: %s\n' "$strategy" >&2
    exit 2
  fi
  if [[ "$continue_integration" -eq 1 && "$strategy" != "squash" ]]; then
    printf '%s\n' '--continue is only valid for squash integration.' >&2
    exit 2
  fi

  branch="$(task_branch "$task_id")"
  ensure_integration_worktree
  if [[ "$continue_integration" -eq 0 && \
    -n "$(git -C "$root" status --porcelain)" ]]; then
    printf 'The integration worktree must be clean before integration.\n' >&2
    exit 1
  fi
  pending="$(
    python3 - "$root/.agents/records" <<'PY'
import json
import sys
from pathlib import Path

records = Path(sys.argv[1])
for path in sorted(records.glob("XT-*.json")):
    with path.open(encoding="utf-8") as source:
        record = json.load(source)
    if record.get("state") == "integrated":
        print(record["id"])
PY
  )"
  if [[ -n "$pending" ]]; then
    printf 'Accept the pending integrated task before another integration: %s\n' \
      "$pending" >&2
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
  python3 -B "$worktree/tool/harness/task_conflicts.py" \
    --root "$worktree" claim "$task_id"
  python3 -B "$worktree/tool/harness/task_conflicts.py" \
    --root "$worktree" stale "$task_id" "$base" "$integration_branch"
  head="$("$worktree/tool/harness/governance.py" get "$task_id" head_sha)"
  review_tip="$(git -C "$root" rev-parse "$branch")"
  review_parent="$(git -C "$root" rev-parse "$review_tip^")"
  review_paths="$(
    git -C "$root" diff-tree \
      --no-commit-id \
      --name-only \
      -r \
      "$review_tip"
  )"
  review_lifecycle="$(
    git -C "$root" show -s --format=%B "$review_tip" |
      git interpret-trailers --parse |
      awk -F': ' '$1 == "Xnn-Lifecycle" { print $2 }'
  )"
  if [[ "$review_parent" != "$head" ||
    "$review_paths" != ".agents/records/$task_id.json" ||
    "$review_lifecycle" != "review" ]]; then
    printf '%s\n' \
      "$task_id branch tip is not its immutable review commit." \
      'Return the task to in_progress and repeat review.' >&2
    exit 1
  fi
  reviewed_patch="$(
    range_patch_id \
      "$base" \
      "$head" \
      ".agents/records/$task_id.json"
  )"
  current_patch="$(
    range_patch_id \
      "$base" \
      "$branch" \
      ".agents/records/$task_id.json"
  )"
  if [[ -z "$reviewed_patch" || "$current_patch" != "$reviewed_patch" ]]; then
    printf '%s\n' \
      "$task_id payload changed after review." \
      "reviewed: ${reviewed_patch:-empty}" \
      "current: ${current_patch:-empty}" \
      'Return the task to in_progress and repeat review.' >&2
    exit 1
  fi
  if [[ -f "$worktree/tool/harness/commit_message.py" ]]; then
    python3 -B \
      "$worktree/tool/harness/commit_message.py" \
      range \
      --root "$worktree" \
      "$base" \
      "$branch"
  fi

  if [[ "$strategy" == "squash" ]]; then
    integrate_squash \
      "$task_id" "$branch" "$base" "$continue_integration"
  else
    integrate_cherry_pick "$task_id" "$branch" "$base"
  fi
  git -C "$root" config "branch.$branch.xnnState" integrated
  printf '%s: review -> integrated (%s)\n' "$task_id" "$strategy"
}

remote_ref_sha() {
  local ref="$1"
  git -C "$root" ls-remote --heads "$remote" "$ref" |
    awk 'NR == 1 { print $1 }'
}

require_fast_forward() {
  local remote_head="$1"
  local candidate="$2"
  if [[ -z "$remote_head" ]]; then
    printf 'Remote integration branch %s does not exist.\n' \
      "$integration_branch" >&2
    return 1
  fi
  if ! git -C "$root" cat-file -e "$remote_head^{commit}" 2>/dev/null; then
    git -C "$root" fetch --quiet "$remote" \
      "refs/heads/$integration_branch"
  fi
  if ! git -C "$root" merge-base --is-ancestor \
    "$remote_head" "$candidate"; then
    printf '%s\n' \
      "Remote $integration_branch at $remote_head is not an ancestor of" \
      "candidate $candidate. Rebase and repeat review." >&2
    return 1
  fi
}

stage_candidate_ci() {
  local candidate_branch="$1"
  local candidate_ref="$2"
  local candidate_sha="$3"
  local current
  current="$(remote_ref_sha "$candidate_ref")"
  if [[ "$current" != "$candidate_sha" ]]; then
    if ! git -C "$root" push \
      --force-with-lease="$candidate_ref:$current" \
      "$remote" \
      "$candidate_sha:$candidate_ref" >&2; then
      printf 'Cannot stage %s on %s.\n' \
        "$candidate_sha" "$candidate_branch" >&2
      return 1
    fi
  fi
  python3 -B "$github_ci" wait \
    --root "$root" \
    --remote "$remote" \
    --branch "$candidate_branch" \
    --sha "$candidate_sha"
}

delete_candidate_ref() {
  local candidate_ref="$1"
  local expected_sha="$2"
  local current
  if ! current="$(remote_ref_sha "$candidate_ref")"; then
    printf 'Warning: could not inspect candidate ref %s.\n' \
      "$candidate_ref" >&2
    return
  fi
  if [[ -z "$current" ]]; then
    return
  fi
  if [[ "$current" != "$expected_sha" ]]; then
    printf 'Not deleting changed candidate ref %s at %s.\n' \
      "$candidate_ref" "$current" >&2
    return
  fi
  if ! git -C "$root" push \
    --force-with-lease="$candidate_ref:$expected_sha" \
    "$remote" \
    ":$candidate_ref" >/dev/null; then
    printf 'Warning: could not delete candidate ref %s.\n' \
      "$candidate_ref" >&2
  fi
}

publish_integration() {
  local expected_remote="$1"
  local acceptance_sha="$2"
  git -C "$root" push \
    --force-with-lease="refs/heads/$integration_branch:$expected_remote" \
    "$remote" \
    "$acceptance_sha:refs/heads/$integration_branch"
}

accept() {
  if [[ "$#" -ne 2 ]]; then
    usage
    exit 2
  fi

  local task_id="$1"
  local reviewer="$2"
  local branch state verified_sha acceptance_sha backup reference
  local candidate_branch candidate_ref remote_head task_trailer lifecycle
  branch="$(task_branch "$task_id")"
  candidate_branch="ci/$task_id"
  candidate_ref="refs/heads/$candidate_branch"

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
  if [[ "$state" == "done" ]]; then
    task_trailer="$(
      git -C "$root" show -s --format=%B HEAD |
        git interpret-trailers --parse |
        awk -F': ' '$1 == "Xnn-Task" { print $2 }'
    )"
    lifecycle="$(
      git -C "$root" show -s --format=%B HEAD |
        git interpret-trailers --parse |
        awk -F': ' '$1 == "Xnn-Lifecycle" { print $2 }'
    )"
    if [[ "$task_trailer" != "$task_id" || "$lifecycle" != "acceptance" ]]; then
      printf '%s done state is not at its acceptance commit.\n' \
        "$task_id" >&2
      exit 1
    fi
    "$governance" validate >/dev/null
    acceptance_sha="$(git -C "$root" rev-parse HEAD)"
    remote_head="$(remote_ref_sha "refs/heads/$integration_branch")"
    if [[ "$remote_head" == "$acceptance_sha" ]]; then
      git -C "$root" config "branch.$branch.xnnState" done
      delete_candidate_ref "$candidate_ref" "$acceptance_sha"
      printf '%s: done and published\n' "$task_id"
      return
    fi
    require_fast_forward "$remote_head" "$acceptance_sha"
    if ! stage_candidate_ci \
      "$candidate_branch" "$candidate_ref" "$acceptance_sha" >/dev/null; then
      delete_candidate_ref "$candidate_ref" "$acceptance_sha"
      return 1
    fi
    if ! publish_integration \
      "$remote_head" "$acceptance_sha"; then
      printf '%s\n' \
        'Acceptance CI passed, but publishing harness failed.' \
        "Retry: tool/harness/agent.sh accept $task_id $reviewer" >&2
      return 1
    fi
    delete_candidate_ref "$candidate_ref" "$acceptance_sha"
    git -C "$root" config "branch.$branch.xnnState" done
    printf '%s: resumed publication -> done\n' "$task_id"
    return
  fi
  if [[ "$state" != "integrated" ]]; then
    printf '%s must be integrated before acceptance; state=%s\n' \
      "$task_id" "$state" >&2
    exit 1
  fi

  "$governance" validate >/dev/null
  run_verification "$root" "$task_id"
  verified_sha="$(git -C "$root" rev-parse HEAD)"
  remote_head="$(remote_ref_sha "refs/heads/$integration_branch")"
  require_fast_forward "$remote_head" "$verified_sha"
  if ! reference="$(
    stage_candidate_ci \
      "$candidate_branch" "$candidate_ref" "$verified_sha"
  )"; then
    delete_candidate_ref "$candidate_ref" "$verified_sha"
    return 1
  fi
  backup="$(mktemp)"
  cp "$root/.agents/records/$task_id.json" "$backup"
  if ! "$governance" mark-accepted \
    "$task_id" "$reviewer" "$reference" "$verified_sha"; then
    cp "$backup" "$root/.agents/records/$task_id.json"
    rm -f "$backup"
    delete_candidate_ref "$candidate_ref" "$verified_sha"
    return 1
  fi
  if ! "$governance" validate; then
    cp "$backup" "$root/.agents/records/$task_id.json"
    rm -f "$backup"
    delete_candidate_ref "$candidate_ref" "$verified_sha"
    printf 'Acceptance record failed governance validation.\n' >&2
    exit 1
  fi
  if ! commit_record \
    "$root" \
    "$task_id" \
    acceptance \
    "$(task_lifecycle_subject "$task_id" acceptance)"; then
    git -C "$root" reset --hard "$verified_sha" >/dev/null
    cp "$backup" "$root/.agents/records/$task_id.json"
    rm -f "$backup"
    delete_candidate_ref "$candidate_ref" "$verified_sha"
    return 1
  fi
  acceptance_sha="$(git -C "$root" rev-parse HEAD)"
  if ! stage_candidate_ci \
    "$candidate_branch" "$candidate_ref" "$acceptance_sha" >/dev/null; then
    git -C "$root" reset --hard "$verified_sha" >/dev/null
    git -C "$root" config "branch.$branch.xnnState" integrated
    rm -f "$backup"
    delete_candidate_ref "$candidate_ref" "$acceptance_sha"
    printf '%s remains integrated after acceptance CI failure.\n' \
      "$task_id" >&2
    return 1
  fi
  rm -f "$backup"
  if ! publish_integration "$remote_head" "$acceptance_sha"; then
    printf '%s\n' \
      'Acceptance CI passed, but publishing harness failed.' \
      "Retry: tool/harness/agent.sh accept $task_id $reviewer" >&2
    return 1
  fi
  delete_candidate_ref "$candidate_ref" "$acceptance_sha"
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
  "$governance" validate >/dev/null
  if ! git -C "$root" show-ref --verify --quiet "refs/heads/$branch"; then
    printf '%s has no local task branch; nothing to clean.\n' "$task_id"
    return
  fi
  base="$("$governance" get "$task_id" base_sha)"
  commits_file="$(mktemp)"
  git -C "$root" rev-list --reverse "$base..$branch" >"$commits_file"
  if ! python3 - \
    "$root/.agents/records/$task_id.json" \
    "$commits_file" <<'PY'
import json
import sys

record_path, commits_path = sys.argv[1:]
with open(record_path, encoding="utf-8") as source:
    record = json.load(source)
with open(commits_path, encoding="utf-8") as source:
    commits = [line.strip() for line in source if line.strip()]
integration = record["integration"]
if integration["strategy"] == "squash":
    if commits != integration["source_commits"]:
        raise SystemExit(
            "Task branch does not match squash source provenance."
        )
elif integration["strategy"] in {"cherry-pick", "merge"}:
    mapped = {item["source"] for item in integration["mappings"]}
    missing = sorted(set(commits) - mapped)
    if missing:
        raise SystemExit(
            "Task branch contains commits without integration mappings:\n"
            + "\n".join(missing)
        )
else:
    raise SystemExit("Task has an unsupported integration strategy.")
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
