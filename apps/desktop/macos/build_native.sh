#!/bin/bash

set -euo pipefail

repository_root="$(cd "$PROJECT_DIR/../../.." && pwd)"
configuration="$(printf '%s' "$CONFIGURATION" | tr '[:upper:]' '[:lower:]')"
architectures="${ARCHS:-$(uname -m)}"
architecture_key="$(printf '%s' "$architectures" | tr ' ' '-')"
cmake_architectures="$(printf '%s' "$architectures" | tr ' ' ';')"
build_type="Release"
if [[ "$CONFIGURATION" == "Debug" ]]; then
  build_type="Debug"
fi

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
  -DXNN_TRANSFER_BUILD_TESTS=OFF \
  -DXNN_TRANSFER_BUILD_BENCHMARKS=OFF \
  -DXNN_TRANSFER_BUILD_FUZZERS=OFF
cmake --build "$build_directory" --target xnn_transfer_core

mkdir -p "$frameworks_directory"
cp -f "$source_library" "$destination_library"

signing_identity="${EXPANDED_CODE_SIGN_IDENTITY:--}"
if [[ -z "$signing_identity" ]]; then
  signing_identity="-"
fi
codesign \
  --force \
  --sign "$signing_identity" \
  --timestamp=none \
  "$destination_library"
