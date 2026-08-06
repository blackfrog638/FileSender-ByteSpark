#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ "$#" -lt 1 ]]; then
  printf 'Usage: %s flutter|dart [arguments...]\n' "$0" >&2
  exit 2
fi

tool="$1"
shift
if [[ "$tool" != "flutter" && "$tool" != "dart" ]]; then
  printf 'Unsupported SDK tool: %s\n' "$tool" >&2
  exit 2
fi

cd "$root/apps/desktop"
if command -v fvm >/dev/null 2>&1 && [[ -f .fvmrc ]]; then
  exec fvm "$tool" "$@"
fi
if command -v "$tool" >/dev/null 2>&1; then
  exec "$tool" "$@"
fi

printf '%s is unavailable. Install FVM and run make bootstrap.\n' "$tool" >&2
exit 127
