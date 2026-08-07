#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

cmake --preset dev -S "$root"
cmake --build --preset dev --target \
  xnn_transfer_core \
  xnn_transfer_abi_current_layout_test \
  xnn_transfer_abi_v1_frozen_layout_test \
  xnn_transfer_abi_v1_legacy_client
ctest \
  --test-dir "$root/out/build/dev" \
  --output-on-failure \
  --no-tests=error \
  -R '^xnn_transfer_abi_'
