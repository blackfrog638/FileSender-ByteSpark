#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

python3 -B "$root/tool/harness/project_model_test.py"
python3 -B "$root/tool/harness/project_model.py" \
  --root "$root" \
  validate \
  --check-docs
