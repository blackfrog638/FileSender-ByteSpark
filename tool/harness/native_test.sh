#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if cmake --version >/dev/null 2>&1 &&
  ninja --version >/dev/null 2>&1; then
  printf 'Running native tests through CMake and CTest.\n'
  (
    cd "$root"
    cmake --preset dev
    cmake --build --preset dev
    ctest --preset dev --no-tests=error
  )
  exit 0
fi

cxx="${CXX:-c++}"
if ! "$cxx" --version >/dev/null 2>&1; then
  printf 'No C++ compiler is available for the native fallback build.\n' >&2
  exit 1
fi

output_dir="$root/out/build/fallback"
mkdir -p "$output_dir"

printf 'Running native fallback build with %s.\n' "$cxx"
"$cxx" \
  -std=c++20 \
  -Wall \
  -Wextra \
  -Wpedantic \
  -pthread \
  -I"$root/native/include" \
  "$root/native/src/bridge/c_api.cpp" \
  "$root/native/src/core/engine.cpp" \
  "$root/native/tests/native_tests.cpp" \
  -o "$output_dir/xnn_transfer_native_tests"

"$output_dir/xnn_transfer_native_tests"

"$cxx" \
  -std=c++20 \
  -Wall \
  -Wextra \
  -Wpedantic \
  -pthread \
  -I"$root/native/include" \
  "$root/native/src/bridge/c_api.cpp" \
  "$root/native/src/core/engine.cpp" \
  "$root/native/tests/bridge/c_api_event_test.cpp" \
  -o "$output_dir/xnn_transfer_bridge_event_tests"

"$output_dir/xnn_transfer_bridge_event_tests"

"$cxx" \
  -std=c++20 \
  -Wall \
  -Wextra \
  -Wpedantic \
  -I"$root/native/src" \
  "$root/native/src/protocol/v1_parser.cpp" \
  "$root/native/tests/protocol/v1_parser_test.cpp" \
  -o "$output_dir/xnn_transfer_protocol_tests"

"$output_dir/xnn_transfer_protocol_tests"
python3 "$root/protocol/testdata/v1/validate_vectors.py"
python3 "$root/protocol/testdata/v1/generate_native_vectors.py" --check
