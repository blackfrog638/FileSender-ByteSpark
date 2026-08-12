#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
validator="$root/tool/harness/commit_message.py"

python3 -B "$root/tool/harness/commit_message_test.py"

base="${XNN_TRANSFER_COMMIT_BASE:-}"
head="${XNN_TRANSFER_COMMIT_HEAD:-HEAD}"
if [[ -z "$base" ]]; then
  branch="$(git -C "$root" branch --show-current)"
  if [[ "$branch" =~ ^work/XT-[0-9]{3,}$ ]]; then
    integration_branch="$(
      python3 -c \
        'import json,sys; print(json.load(open(sys.argv[1]))["integration_branch"])' \
        "$root/.agents/manifest.json"
    )"
    if git -C "$root" rev-parse \
      --verify "refs/heads/$integration_branch" >/dev/null 2>&1; then
      base="$(
        git -C "$root" merge-base "refs/heads/$integration_branch" "$head"
      )"
    fi
  fi
fi
if [[ -z "$base" ]] && git -C "$root" rev-parse HEAD^ >/dev/null 2>&1; then
  base="HEAD^"
fi

if [[ -n "$base" ]]; then
  python3 -B "$validator" range --root "$root" "$base" "$head"
else
  printf 'Commit range check skipped: repository has no parent commit.\n'
fi
