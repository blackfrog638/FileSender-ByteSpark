#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

python3 -B "$root/tool/harness/architecture_test_test.py"
python3 -B "$root/tool/harness/architecture_test.py" --root "$root"
