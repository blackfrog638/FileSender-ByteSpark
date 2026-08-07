#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
vcpkg_root="${XNN_TRANSFER_VCPKG_ROOT:-$root/out/tools/vcpkg}"
binary_cache="${VCPKG_DEFAULT_BINARY_CACHE:-$root/out/vcpkg-cache}"

binary_cache_path="$binary_cache"
probe_cache="${binary_cache}.bootstrap-probe.$$"
probe_cache_path="$probe_cache"
case "$(uname -s)" in
  MINGW* | MSYS* | CYGWIN*)
    binary_cache_path="$(cygpath -u "$binary_cache")"
    probe_cache_path="$(cygpath -u "$probe_cache")"
    ;;
esac

rm -rf "$probe_cache_path"
VCPKG_DEFAULT_BINARY_CACHE="$probe_cache" \
  "$root/tool/harness/vcpkg_bootstrap.sh"
if [[ ! -d "$probe_cache_path" ]]; then
  printf 'vcpkg bootstrap did not create a missing binary cache: %s\n' \
    "$probe_cache" >&2
  exit 1
fi
rm -rf "$probe_cache_path"

export VCPKG_DEFAULT_BINARY_CACHE="$binary_cache"
"$root/tool/harness/vcpkg_bootstrap.sh"
if [[ ! -d "$binary_cache_path" ]]; then
  printf 'vcpkg bootstrap did not create the binary cache: %s\n' \
    "$binary_cache" >&2
  exit 1
fi
python3 -B "$root/tool/harness/dependency_manifest_test_test.py"
python3 -B "$root/tool/harness/dependency_manifest_test.py" \
  --root "$root" \
  --vcpkg-root "$vcpkg_root"

export VCPKG_DISABLE_METRICS=1

cmake --fresh --preset dev -S "$root"
cmake --build --preset dev --target xnn_transfer_dependency_probe
ctest \
  --test-dir "$root/out/build/dev" \
  --output-on-failure \
  --no-tests=error \
  -R '^xnn_transfer_dependency_probe$'

triplet="$(
  sed -n \
    's/^VCPKG_TARGET_TRIPLET:STRING=//p' \
    "$root/out/build/dev/CMakeCache.txt"
)"
if [[ -z "$triplet" ]]; then
  printf 'Configured vcpkg triplet is missing from CMakeCache.txt.\n' >&2
  exit 1
fi
installed="$root/out/vcpkg_installed/$triplet"
if [[ ! -d "$installed" ]]; then
  printf 'Pinned dependency installation is missing: %s\n' "$installed" >&2
  exit 1
fi

if find "$installed" -type f \
  \( -name '*.dll' -o -name '*.dylib' -o -name '*.so' -o -name '*.so.*' \) \
  -print -quit | grep -q .; then
  printf 'Pinned dependency graph contains a dynamic library:\n' >&2
  find "$installed" -type f \
    \( -name '*.dll' -o -name '*.dylib' -o -name '*.so' -o -name '*.so.*' \) \
    -print >&2
  exit 1
fi

printf 'Pinned dependency probe passed for %s.\n' "$triplet"
