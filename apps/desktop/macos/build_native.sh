#!/bin/bash

set -euo pipefail

: "${PROJECT_DIR:?PROJECT_DIR is required}"
: "${CONFIGURATION:?CONFIGURATION is required}"
: "${TARGET_BUILD_DIR:?TARGET_BUILD_DIR is required}"
: "${FRAMEWORKS_FOLDER_PATH:?FRAMEWORKS_FOLDER_PATH is required}"

repository_root="$(cd "$PROJECT_DIR/../../.." && pwd)"
configuration="$(printf '%s' "$CONFIGURATION" | tr '[:upper:]' '[:lower:]')"
architectures="${ARCHS:-$(uname -m)}"
architecture_key="$(printf '%s' "$architectures" | tr ' ' '-')"
cmake_architectures="$(printf '%s' "$architectures" | tr ' ' ';')"
deployment_target="${MACOSX_DEPLOYMENT_TARGET:-10.14}"

case "$CONFIGURATION" in
  Debug)
    build_type="Debug"
    ;;
  Profile | Release)
    build_type="Release"
    ;;
  *)
    printf 'Unsupported macOS build configuration: %s\n' "$CONFIGURATION" >&2
    exit 1
    ;;
esac

build_directory="$repository_root/out/build/flutter-macos/$configuration-$architecture_key"
library_name="libxnn_transfer_core.dylib"
source_library="$build_directory/native/$library_name"
frameworks_directory="$TARGET_BUILD_DIR/$FRAMEWORKS_FOLDER_PATH"
destination_library="$frameworks_directory/$library_name"

cmake \
  -S "$repository_root" \
  -B "$build_directory" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE="$build_type" \
  -DCMAKE_OSX_ARCHITECTURES="$cmake_architectures" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target" \
  -DXNN_TRANSFER_BUILD_TESTS=OFF \
  -DXNN_TRANSFER_BUILD_BENCHMARKS=OFF \
  -DXNN_TRANSFER_BUILD_FUZZERS=OFF
cmake --build "$build_directory" --target xnn_transfer_core

if [[ ! -f "$source_library" ]]; then
  printf 'Native build did not produce %s\n' "$source_library" >&2
  exit 1
fi

cmake -E make_directory "$frameworks_directory"
cmake -E copy_if_different "$source_library" "$destination_library"

signing_identity="${EXPANDED_CODE_SIGN_IDENTITY:--}"
if [[ -z "$signing_identity" ]]; then
  signing_identity="-"
fi
codesign \
  --force \
  --sign "$signing_identity" \
  --timestamp=none \
  "$destination_library"
codesign --verify --strict "$destination_library"
