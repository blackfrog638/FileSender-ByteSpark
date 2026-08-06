#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ "$(uname -s)" != "Darwin" ]]; then
  printf 'The macOS bundle test must run on macOS.\n' >&2
  exit 1
fi

"$root/tool/harness/desktop_bundle_test.sh" "$@"
