#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

find_clang_format() {
  local candidate
  for candidate in \
    "$(command -v clang-format 2>/dev/null || true)" \
    "$(xcrun --find clang-format 2>/dev/null || true)"; do
    if [[ -n "$candidate" ]] && "$candidate" --version >/dev/null 2>&1; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

if clang_format="$(find_clang_format)"; then
  find "$root/native" \
    -type f \
    \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
    -print0 |
    xargs -0 "$clang_format" -i
else
  printf '[skip] Native formatting: clang-format unavailable\n'
fi

if "$root/tool/harness/sdk.sh" dart --version >/dev/null 2>&1; then
  (
    cd "$root/apps/desktop"
    "$root/tool/harness/sdk.sh" dart format lib test
  )
else
  printf '[skip] Dart formatting: Dart SDK unavailable\n'
fi
