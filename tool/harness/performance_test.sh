#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if ! cmake --version >/dev/null 2>&1 ||
  ! ninja --version >/dev/null 2>&1; then
  printf 'CMake and Ninja are required for performance tests.\n' >&2
  exit 1
fi

(
  cd "$root"
  cmake --preset benchmark
  cmake --build --preset benchmark --target xnn_transfer_engine_benchmark
  "$root/out/build/benchmark/native/xnn_transfer_engine_benchmark"
)

printf '%s\n' \
  'Benchmark results are informational until transfer I/O is implemented.' \
  'Do not interpret native lifecycle throughput as file-transfer throughput.'
