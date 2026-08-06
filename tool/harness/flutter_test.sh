#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
native_library=""
dart_native_library=""

if ! "$root/tool/harness/sdk.sh" flutter --version >/dev/null 2>&1; then
  printf 'Flutter SDK is required for Flutter verification.\n' >&2
  exit 1
fi

case "$(uname -s)" in
  Darwin)
    native_library="$root/out/build/dev/native/libxnn_transfer_core.dylib"
    ;;
  Linux)
    native_library="$root/out/build/dev/native/libxnn_transfer_core.so"
    ;;
  MINGW* | MSYS* | CYGWIN*)
    native_library="$root/out/build/dev/native/xnn_transfer_core.dll"
    ;;
esac
dart_native_library="$native_library"
if [[ -f "$native_library" ]] && command -v cygpath >/dev/null 2>&1; then
  dart_native_library="$(cygpath -w "$native_library")"
fi

(
  cd "$root/apps/desktop"
  "$root/tool/harness/sdk.sh" flutter pub get
  "$root/tool/harness/sdk.sh" dart \
    format --output=none --set-exit-if-changed lib test
  "$root/tool/harness/sdk.sh" flutter analyze
  if [[ -n "$native_library" && -f "$native_library" ]]; then
    printf 'Running Flutter tests against %s.\n' "$native_library"
    XNN_TRANSFER_LIBRARY_PATH="$dart_native_library" \
      "$root/tool/harness/sdk.sh" flutter test
  else
    "$root/tool/harness/sdk.sh" flutter test
  fi
)
