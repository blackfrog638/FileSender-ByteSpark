#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
expected_abi_version=1
operating_system="$(uname -s)"

find_python() {
  local candidate
  for candidate in "${PYTHON:-}" python3 python; do
    if [[ -n "$candidate" ]] && command -v "$candidate" >/dev/null 2>&1; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  printf 'Python is required to load the bundled native library.\n' >&2
  return 1
}

find_single_executable() {
  local search_root="$1"
  local path_pattern="$2"
  local candidate
  local -a matches=()

  if [[ ! -d "$search_root" ]]; then
    printf 'Flutter build output is missing: %s\n' "$search_root" >&2
    return 1
  fi

  while IFS= read -r candidate; do
    matches+=("$candidate")
  done < <(find "$search_root" -type f -path "$path_pattern" -print)

  if [[ "${#matches[@]}" -ne 1 ]]; then
    printf 'Expected one bundled executable matching %s, found %d.\n' \
      "$path_pattern" "${#matches[@]}" >&2
    printf '  %s\n' "${matches[@]}" >&2
    return 1
  fi

  executable="${matches[0]}"
}

verify_library_load() {
  local library="$1"
  local python_command="$2"

  if [[ ! -f "$library" ]]; then
    printf 'Native library is missing from the Flutter bundle: %s\n' \
      "$library" >&2
    return 1
  fi

  "$python_command" - "$library" "$expected_abi_version" <<'PY'
import ctypes
import pathlib
import sys

library_path = pathlib.Path(sys.argv[1]).resolve()
expected_version = int(sys.argv[2])
library = ctypes.CDLL(str(library_path))
abi_version = library.xnn_transfer_abi_version
abi_version.argtypes = []
abi_version.restype = ctypes.c_uint32
actual_version = abi_version()
if actual_version != expected_version:
    raise SystemExit(
        f"Unexpected native ABI version: {actual_version}; "
        f"expected {expected_version}"
    )
print(f"Loaded {library_path}; ABI version={actual_version}.")
PY
}

if [[ "$#" -eq 0 ]]; then
  set -- debug release
fi

python_command="$(find_python)"
for mode in "$@"; do
  case "$mode" in
    debug)
      configuration="Debug"
      ;;
    release)
      configuration="Release"
      ;;
    *)
      printf 'Usage: %s [debug|release]...\n' "$0" >&2
      exit 2
      ;;
  esac

  case "$operating_system" in
    Darwin)
      flutter_platform="macos"
      ;;
    Linux)
      flutter_platform="linux"
      ;;
    MINGW* | MSYS* | CYGWIN*)
      flutter_platform="windows"
      ;;
    *)
      printf 'Unsupported desktop build host: %s\n' "$operating_system" >&2
      exit 1
      ;;
  esac

  printf 'Building %s Flutter bundle in %s mode.\n' \
    "$flutter_platform" "$mode"
  "$root/tool/harness/sdk.sh" flutter build "$flutter_platform" "--$mode"

  case "$flutter_platform" in
    macos)
      app="$root/apps/desktop/build/macos/Build/Products/$configuration/xnn_transfer.app"
      executable="$app/Contents/MacOS/xnn_transfer"
      library="$app/Contents/Frameworks/libxnn_transfer_core.dylib"
      if [[ ! -x "$executable" ]]; then
        printf 'Bundled macOS executable is missing: %s\n' "$executable" >&2
        exit 1
      fi
      if [[ ! -f "$library" ]]; then
        printf 'Native library is missing from the Flutter bundle: %s\n' \
          "$library" >&2
        exit 1
      fi
      executable_architectures="$(lipo -archs "$executable")"
      library_architectures="$(lipo -archs "$library")"
      if [[ "$library_architectures" != "$executable_architectures" ]]; then
        printf 'Native library architectures (%s) do not match the app (%s).\n' \
          "$library_architectures" "$executable_architectures" >&2
        exit 1
      fi
      codesign --verify --strict "$library"
      codesign --verify --deep --strict "$app"
      if ! otool -D "$library" |
        grep -q '^@rpath/libxnn_transfer_core\.dylib$'; then
        printf 'Native library has a non-relocatable install name: %s\n' \
          "$library" >&2
        exit 1
      fi
      ;;
    linux)
      find_single_executable \
        "$root/apps/desktop/build/linux" \
        "*/$mode/bundle/xnn_transfer"
      library="$(dirname "$executable")/libxnn_transfer_core.so"
      ;;
    windows)
      find_single_executable \
        "$root/apps/desktop/build/windows" \
        "*/runner/$configuration/xnn_transfer.exe"
      library="$(dirname "$executable")/xnn_transfer_core.dll"
      ;;
  esac

  verify_library_load "$library" "$python_command"
done
