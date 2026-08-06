#!/usr/bin/env bash

set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
missing=0

check_tool() {
  local name="$1"
  shift

  if "$@" >/dev/null 2>&1; then
    printf '  [ok]      %s\n' "$name"
  else
    printf '  [missing] %s\n' "$name"
    missing=1
  fi
}

check_clang_format() {
  local candidate
  for candidate in \
    "$(command -v clang-format 2>/dev/null || true)" \
    "$(xcrun --find clang-format 2>/dev/null || true)"; do
    if [[ -n "$candidate" ]] && "$candidate" --version >/dev/null 2>&1; then
      printf '  [ok]      clang-format\n'
      return
    fi
  done

  printf '  [missing] clang-format\n'
  missing=1
}

check_fuzzer_runtime() {
  local runtime
  if command -v brew >/dev/null 2>&1; then
    runtime="$(brew --prefix llvm 2>/dev/null)/lib/clang"
    if find "$runtime" -name 'libclang_rt.fuzzer*' -print -quit \
      2>/dev/null | grep -q .; then
      printf '  [ok]      libFuzzer runtime\n'
      return
    fi
  elif [[ "$(uname -s)" == "Linux" ]] &&
    clang++ --version >/dev/null 2>&1; then
    printf '  [ok]      libFuzzer compiler\n'
    return
  fi

  printf '  [missing] libFuzzer runtime\n'
  missing=1
}

printf 'XnnTransfer toolchain\n'
check_tool git git --version
check_tool c++ c++ --version
check_tool cmake cmake --version
check_tool ninja ninja --version
check_clang_format
check_tool fvm fvm --version
check_tool flutter "$root/tool/harness/sdk.sh" flutter --version
if [[ "$(uname -s)" == "Darwin" ]]; then
  check_tool CocoaPods pod --version
fi
check_fuzzer_runtime

if [[ "$missing" -eq 0 ]]; then
  printf 'All tools required for full verification are available.\n'
  exit 0
fi

printf '%s\n' \
  'Some full-verification tools are unavailable.' \
  'Bootstrap and verify will run the gates supported by this machine.'

if [[ "${STRICT_TOOLS:-0}" == "1" ]]; then
  exit 1
fi
