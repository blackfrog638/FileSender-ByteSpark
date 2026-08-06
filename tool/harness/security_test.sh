#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fuzz_seconds="${XNN_TRANSFER_FUZZ_SECONDS:-10}"
asan_options="halt_on_error=1"
sanitizer_cxx="${XNN_TRANSFER_CLANGXX:-clang++}"
fuzz_cxx="${XNN_TRANSFER_FUZZ_CLANGXX:-$sanitizer_cxx}"

if [[ "$(uname -s)" == "Linux" ]]; then
  asan_options="detect_leaks=1:$asan_options"
elif command -v brew >/dev/null 2>&1; then
  homebrew_clang="$(brew --prefix llvm 2>/dev/null)/bin/clang++"
  if [[ -x "$homebrew_clang" ]]; then
    fuzz_cxx="$homebrew_clang"
  fi
fi

if ! cmake --version >/dev/null 2>&1 ||
  ! ninja --version >/dev/null 2>&1; then
  printf 'CMake and Ninja are required for security tests.\n' >&2
  exit 1
fi
if ! "$sanitizer_cxx" --version >/dev/null 2>&1 ||
  ! "$fuzz_cxx" --version >/dev/null 2>&1; then
  printf 'Clang is required for sanitizer and fuzz tests.\n' >&2
  exit 1
fi

(
  cd "$root"

  cmake --fresh --preset sanitizers \
    -DCMAKE_CXX_COMPILER="$sanitizer_cxx"
  cmake --build --preset sanitizers
  ASAN_OPTIONS="$asan_options" \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    ctest --preset sanitizers --no-tests=error

  cmake --fresh --preset fuzz -DCMAKE_CXX_COMPILER="$fuzz_cxx"
  cmake --build --preset fuzz --target xnn_transfer_c_api_fuzzer
  fuzz_corpus="$root/out/fuzz-corpus/c_api"
  cmake -E rm -rf "$fuzz_corpus"
  cmake -E make_directory "$fuzz_corpus"
  cmake -E copy_directory \
    "$root/native/fuzz/corpus/c_api" \
    "$fuzz_corpus"
  ASAN_OPTIONS="$asan_options" \
    "$root/out/build/fuzz/native/xnn_transfer_c_api_fuzzer" \
    "$fuzz_corpus" \
    -max_total_time="$fuzz_seconds" \
    -max_len=256 \
    -timeout=2 \
    -verbosity=0 \
    -print_final_stats=1
)
