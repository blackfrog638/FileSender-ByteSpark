#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if ! "$root/tool/harness/sdk.sh" flutter --version >/dev/null 2>&1; then
  printf 'Flutter SDK is required for Flutter verification.\n' >&2
  exit 1
fi

(
  cd "$root/apps/desktop"
  "$root/tool/harness/sdk.sh" flutter pub get
  "$root/tool/harness/sdk.sh" dart \
    format --output=none --set-exit-if-changed lib test
  "$root/tool/harness/sdk.sh" flutter analyze
  "$root/tool/harness/sdk.sh" flutter test
)
