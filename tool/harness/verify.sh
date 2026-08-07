#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
skipped=0

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

cd "$root"

git diff --check
"$root/tool/harness/check_layout.sh"
"$root/tool/harness/architecture_test.sh"
"$root/tool/harness/agent.sh" validate
"$root/tool/harness/governance_test.sh"

find "$root/tool/harness" -type f -name '*.sh' -exec bash -n {} \;

whitespace="$(
  find "$root" \
    \( -path "$root/.git" -o \
       -path "$root/out" -o \
       -path "$root/apps/desktop/.dart_tool" -o \
       -path "$root/apps/desktop/.fvm" -o \
       -path "$root/apps/desktop/build" -o \
       -path '*/ephemeral' \) -prune -o \
    -type f \
    \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o \
       -name '*.dart' -o -name '*.md' -o -name '*.yaml' -o \
       -name '*.yml' -o -name '*.json' -o -name '*.sh' \) \
    -exec grep -HnE '[[:blank:]]+$' {} + || true
)"
if [[ -n "$whitespace" ]]; then
  printf 'Trailing whitespace detected:\n%s\n' "$whitespace" >&2
  exit 1
fi

if clang_format="$(find_clang_format)"; then
  find "$root/native" \
    -type f \
    \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
    -print0 |
    xargs -0 "$clang_format" --dry-run --Werror
else
  printf '[skip] Native format check: clang-format unavailable\n'
  skipped=1
fi

"$root/tool/harness/native_test.sh"

if "$root/tool/harness/sdk.sh" flutter --version >/dev/null 2>&1; then
  "$root/tool/harness/flutter_test.sh"
else
  printf '[skip] Flutter analyze and tests: SDK unavailable\n'
  skipped=1
fi

if [[ "$skipped" -ne 0 && "${STRICT_TOOLS:-0}" == "1" ]]; then
  printf 'Strict verification rejects skipped gates.\n' >&2
  exit 1
fi

if [[ "$skipped" -eq 0 ]]; then
  printf 'All repository verification gates passed.\n'
else
  printf 'Available verification gates passed; skipped gates are listed above.\n'
fi
