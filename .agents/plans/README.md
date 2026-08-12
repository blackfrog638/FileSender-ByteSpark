# Harness V2 Delivery Plans

This directory contains only active Harness V2 Delivery Plans named
`DP-*.json`.

A plan is static, reviewed input. It owns requirements, observable criteria,
negative definitions, implementation mappings, acceptance ownership, and
required evidence. It never stores runtime task state.

Agents may draft plans. Only the project owner identity configured in
`.agents/manifest.json` may approve one through:

```bash
tool/harness/agent.sh approve-plan .agents/plans/DP-NAME.json \
  --at 2026-08-12T00:00:00Z
```

The active catalogue may be empty. Legacy V1 plans remain available from
`archive/harness-v1/final-local`; they are not silently relabeled as V2 plans.
