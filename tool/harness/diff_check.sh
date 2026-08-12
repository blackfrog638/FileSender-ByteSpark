#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
head="${XNN_TRANSFER_COMMIT_HEAD:-HEAD}"
base="${XNN_TRANSFER_COMMIT_BASE:-}"

if [[ -z "$base" ]]; then
  if git -C "$root" rev-parse --verify "$head^" >/dev/null 2>&1; then
    base="$(git -C "$root" rev-parse "$head^")"
  else
    base="$(git -C "$root" hash-object -t tree /dev/null)"
  fi
fi

git -C "$root" diff --check "$base..$head"
git -C "$root" diff --check
