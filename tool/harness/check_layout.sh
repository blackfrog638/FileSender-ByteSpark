#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

required=(
  "AGENTS.md"
  ".agents/manifest.yaml"
  ".agents/tasks/TASK_TEMPLATE.md"
  "docs/architecture.md"
  "docs/roadmap.md"
  "protocol/spec/README.md"
  "native/include/xnn_transfer/c_api.h"
  "apps/desktop/pubspec.yaml"
  "tool/harness/verify.sh"
)

for path in "${required[@]}"; do
  if [[ ! -f "$root/$path" ]]; then
    printf 'Required harness file is missing: %s\n' "$path" >&2
    exit 1
  fi
done

while IFS= read -r path; do
  case "$path" in
    "$root/apps/desktop/lib/core/native/"*) ;;
    *)
      printf 'dart:ffi import outside the native adapter: %s\n' "$path" >&2
      exit 1
      ;;
  esac
done < <(
  grep -RIl \
    --include='*.dart' \
    'dart:ffi' \
    "$root/apps/desktop/lib" || true
)

printf 'Repository layout checks passed.\n'
