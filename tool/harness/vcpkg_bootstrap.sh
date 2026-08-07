#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
vcpkg_commit="17f35ad2418007a895ced8a4cece4ab34068a58d"
vcpkg_repository="https://github.com/microsoft/vcpkg.git"
vcpkg_root="${XNN_TRANSFER_VCPKG_ROOT:-$root/out/tools/vcpkg}"

export VCPKG_DISABLE_METRICS=1

if [[ -n "${VCPKG_DEFAULT_BINARY_CACHE:-}" ]]; then
  binary_cache="$VCPKG_DEFAULT_BINARY_CACHE"
  case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*)
      binary_cache="$(cygpath -u "$binary_cache")"
      ;;
  esac
  mkdir -p "$binary_cache"
  if [[ ! -d "$binary_cache" ]]; then
    printf 'Cannot create vcpkg binary cache: %s\n' \
      "$VCPKG_DEFAULT_BINARY_CACHE" >&2
    exit 1
  fi
fi

if [[ ! -d "$vcpkg_root/.git" ]]; then
  rm -rf "$vcpkg_root"
  mkdir -p "$vcpkg_root"
  git -C "$vcpkg_root" init
  git -C "$vcpkg_root" remote add origin "$vcpkg_repository"
  git -C "$vcpkg_root" fetch --depth=1 origin "$vcpkg_commit"
  git -C "$vcpkg_root" checkout --detach "$vcpkg_commit"
fi

origin="$(git -C "$vcpkg_root" remote get-url origin 2>/dev/null || true)"
if [[ "$origin" != "$vcpkg_repository" &&
      "$origin" != "https://github.com/microsoft/vcpkg" ]]; then
  printf 'Unexpected vcpkg origin at %s: %s\n' "$vcpkg_root" "$origin" >&2
  exit 1
fi

if ! git -C "$vcpkg_root" cat-file -e "$vcpkg_commit^{commit}" 2>/dev/null; then
  git -C "$vcpkg_root" fetch --depth=1 origin "$vcpkg_commit"
fi

actual_commit="$(git -C "$vcpkg_root" rev-parse HEAD 2>/dev/null || true)"
if [[ "$actual_commit" != "$vcpkg_commit" ]]; then
  if [[ -n "$(git -C "$vcpkg_root" status --porcelain)" ]]; then
    printf 'Refusing to replace a modified vcpkg checkout: %s\n' \
      "$vcpkg_root" >&2
    exit 1
  fi
  git -C "$vcpkg_root" checkout --detach "$vcpkg_commit"
fi

bootstrap="$vcpkg_root/bootstrap-vcpkg.sh"
executable="$vcpkg_root/vcpkg"
case "$(uname -s)" in
  MINGW* | MSYS* | CYGWIN*)
    bootstrap="$vcpkg_root/bootstrap-vcpkg.bat"
    executable="$vcpkg_root/vcpkg.exe"
    ;;
esac

if [[ ! -x "$executable" ]]; then
  "$bootstrap" -disableMetrics
fi

if [[ "$(git -C "$vcpkg_root" rev-parse HEAD)" != "$vcpkg_commit" ]]; then
  printf 'Pinned vcpkg checkout changed during bootstrap.\n' >&2
  exit 1
fi

printf 'Pinned vcpkg is ready at %s (%s).\n' \
  "$vcpkg_root" "$vcpkg_commit"
