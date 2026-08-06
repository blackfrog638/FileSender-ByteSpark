#!/usr/bin/env bash

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ "$#" -ne 3 ]]; then
  printf 'Usage: %s XT-001 task-slug workstream\n' "$0" >&2
  exit 2
fi

task_id="$1"
slug="$2"
workstream="$3"

if [[ ! "$task_id" =~ ^XT-[0-9]{3,}$ ]]; then
  printf 'Task id must match XT-001.\n' >&2
  exit 2
fi
if [[ ! "$slug" =~ ^[a-z0-9]+(-[a-z0-9]+)*$ ]]; then
  printf 'Task slug must use lowercase kebab-case.\n' >&2
  exit 2
fi

case "$workstream" in
  integration | native_core | native_bridge | flutter_desktop | protocol | documentation) ;;
  *)
    printf 'Unknown workstream: %s\n' "$workstream" >&2
    exit 2
    ;;
esac

destination="$root/.agents/tasks/$task_id-$slug.md"
if [[ -e "$destination" ]]; then
  printf 'Task already exists: %s\n' "$destination" >&2
  exit 1
fi

cp "$root/.agents/tasks/TASK_TEMPLATE.md" "$destination"
TASK_ID="$task_id" TASK_TITLE="${slug//-/ }" TASK_WORKSTREAM="$workstream" \
  perl -pi -e '
    s/^id: XT-000$/id: $ENV{TASK_ID}/;
    s/^title: Replace with a concrete outcome$/title: $ENV{TASK_TITLE}/;
    s/^workstream: integration$/workstream: $ENV{TASK_WORKSTREAM}/;
  ' "$destination"

printf 'Created %s\n' "$destination"
