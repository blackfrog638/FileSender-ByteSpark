#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
app="$root/apps/desktop/build/macos/Build/Products/Debug/xnn_transfer.app"
library="$app/Contents/Frameworks/libxnn_transfer_core.dylib"

if [[ "$(uname -s)" != "Darwin" ]]; then
  printf 'The macOS bundle test must run on macOS.\n' >&2
  exit 1
fi

"$root/tool/harness/sdk.sh" flutter build macos --debug

if [[ ! -f "$library" ]]; then
  printf 'Native library is missing from the app bundle: %s\n' "$library" >&2
  exit 1
fi

codesign --verify --deep --strict "$app"
nm -gU "$library" | grep -q '_xnn_transfer_abi_version$'

python3 - "$library" <<'PY'
import ctypes
import sys

library = ctypes.CDLL(sys.argv[1])
library.xnn_transfer_abi_version.restype = ctypes.c_uint32
version = library.xnn_transfer_abi_version()
if version != 1:
    raise SystemExit(f"Unexpected native ABI version: {version}")
print(f"Bundled native library loaded successfully; ABI version={version}.")
PY
