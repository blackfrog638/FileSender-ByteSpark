#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

required=(
  "AGENTS.md"
  ".agents/manifest.json"
  ".agents/gates.json"
  ".agents/risk-routing.json"
  ".agents/project/blueprint.json"
  ".agents/project/invariants.json"
  ".agents/project/composition-baseline.json"
  ".agents/project/quality-budgets.json"
  ".agents/project/assets.json"
  ".agents/plans/README.md"
  ".agents/tasks/README.md"
  ".agents/schemas/manifest.schema.json"
  ".agents/schemas/bootstrap-evidence.schema.json"
  ".agents/schemas/plan.schema.json"
  ".agents/schemas/task.schema.json"
  ".agents/schemas/gate.schema.json"
  ".agents/schemas/blueprint.schema.json"
  ".agents/schemas/blueprint-change.schema.json"
  ".agents/schemas/composition-baseline.schema.json"
  ".agents/schemas/project-assets.schema.json"
  ".agents/schemas/project-invariants.schema.json"
  ".agents/schemas/quality-budgets.schema.json"
  "docs/architecture.md"
  "docs/roadmap.md"
  "docs/product/capability-map.md"
  "docs/architecture/module-map.md"
  "docs/project/semantic-baseline.md"
  "protocol/spec/README.md"
  "native/include/xnn_transfer/c_api.h"
  "apps/desktop/pubspec.yaml"
  "tool/harness/agent.py"
  "tool/harness/agent_v2.py"
  "tool/harness/agent.sh"
  "tool/harness/ci_validation.py"
  "tool/harness/cleanup.py"
  "tool/harness/delivery.py"
  "tool/harness/model.py"
  "tool/harness/project_model.py"
  "tool/harness/project_model_test.py"
  "tool/harness/project_model_test.sh"
  "tool/harness/runtime.py"
  "tool/harness/gates.py"
  "tool/harness/executor.py"
  "tool/harness/tdd.py"
  "tool/harness/github_evidence.py"
  "tool/harness/verify.sh"
  "tool/harness/diff_check.sh"
  "tool/harness/abi_compat_test.sh"
  "tool/harness/architecture_test.py"
  "tool/harness/architecture_test_test.py"
  "tool/harness/architecture_test.sh"
  "tool/harness/dependency_manifest_test.py"
  "tool/harness/dependency_manifest_test_test.py"
  "tool/harness/dependency_test.sh"
  "tool/harness/vcpkg_bootstrap.sh"
  "vcpkg-configuration.json"
  "vcpkg.json"
)

for path in "${required[@]}"; do
  if [[ ! -f "$root/$path" ]]; then
    printf 'Required harness file is missing: %s\n' "$path" >&2
    exit 1
  fi
done

forbidden=(
  ".agents/backlog.yaml"
  ".agents/manifest.yaml"
  ".agents/records"
  ".agents/handoffs"
  "tool/harness/governance.py"
  "tool/harness/evidence.py"
  "tool/harness/tdd_proof.py"
  "tool/harness/github_ci.py"
  "tool/harness/approval.py"
  "tool/harness/attestation.py"
  "tool/harness/bootstrap.py"
  "tool/harness/branch_reclamation.py"
  "tool/harness/closure.py"
  "tool/harness/merge_queue.py"
  "tool/harness/migrate_v1.py"
  "tool/harness/state.py"
  ".agents/migration-v1.json"
  ".agents/project/approval.json"
  ".agents/schemas/approval.schema.json"
  ".agents/schemas/attestation.schema.json"
  ".agents/schemas/project-approval.schema.json"
)

for path in "${forbidden[@]}"; do
  if [[ -e "$root/$path" ]]; then
    printf 'Legacy Harness V1 path remains active: %s\n' "$path" >&2
    exit 1
  fi
done

while IFS= read -r path; do
  case "$path" in
    "$root/apps/desktop/lib/core/native/"*) ;;
    *)
      printf 'dart:ffi import outside the native adapter: %s\n' "$path" >&2
      exit 1
      ;;
  esac
done < <(
  grep -RIl \
    --include='*.dart' \
    'dart:ffi' \
    "$root/apps/desktop/lib" || true
)

printf 'Repository layout checks passed.\n'
