#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ "$(uname -s)" == "Darwin" ]] &&
  command -v brew >/dev/null 2>&1 &&
  [[ "${XNN_TRANSFER_INSTALL_TOOLS:-0}" == "1" ]]; then
  brew bundle --file="$root/Brewfile"
fi

if command -v fvm >/dev/null 2>&1 &&
  [[ -f "$root/apps/desktop/.fvmrc" ]]; then
  (
    cd "$root/apps/desktop"
    fvm install --skip-pub-get
  )
fi

"$root/tool/harness/vcpkg_bootstrap.sh"
"$root/tool/harness/doctor.sh"

if "$root/tool/harness/sdk.sh" flutter --version >/dev/null 2>&1; then
  "$root/tool/harness/sdk.sh" flutter config --enable-linux-desktop \
    --enable-macos-desktop \
    --enable-windows-desktop

  if [[ ! -d "$root/apps/desktop/macos" ||
        ! -d "$root/apps/desktop/windows" ||
        ! -d "$root/apps/desktop/linux" ]]; then
    (
      cd "$root/apps/desktop"
      "$root/tool/harness/sdk.sh" flutter create \
        --platforms=macos,windows,linux \
        --project-name=xnn_transfer \
        .
    )
  fi

  (
    cd "$root/apps/desktop"
    "$root/tool/harness/sdk.sh" flutter pub get
  )
else
  printf '[skip] Flutter dependency and runner generation: SDK unavailable\n'
fi

if cmake --version >/dev/null 2>&1 &&
  ninja --version >/dev/null 2>&1; then
  cmake --preset dev -S "$root"
else
  printf '[skip] CMake configure: CMake or Ninja unavailable\n'
fi

printf 'Bootstrap completed with the available toolchain.\n'
