# Project Semantic Re-baseline

## Purpose

Establish one machine-readable project model above Delivery Plans. The model
compresses product intent, capability decomposition, architectural invariants,
milestone ordering, and code ownership without pretending that it can
regenerate security-sensitive or concurrent implementation details.

Delivery Plans become executable transitions over referenced Blueprint
outcomes. TaskSpecs remain implementation subdivisions of a Plan. Runtime
state remains outside product commits and is derived from immutable acceptance
attestations.

## Scope

The bootstrap cutover will:

1. add strict Blueprint and Blueprint Change Set contracts;
2. register the complete project capability, milestone, invariant, quality,
   and module relationship graph;
3. map every active V2 Plan into one globally composable transition set;
4. classify current product code as `preserve`, `requalify`, `replace`, or
   `remove`;
5. generate deterministic human-readable roadmap, architecture, and
   re-baseline reports from the model;
6. reject orphan outcomes, duplicate transition writers, unsatisfied
   preconditions, invariant violations, stale node revisions, and uncovered
   Blueprint changes.

The cutover does not delete or regenerate product implementation. Existing V1
acceptance evidence remains historical input and does not automatically qualify
new Blueprint outcomes.

## Delivery boundary

The project owner approves the exact bootstrap candidate because the change
extends the Harness verification trust root. Hosted Linux, macOS, Windows,
Harness, sanitizer, and fuzz evidence is required before protected publication.

After cutover, a normal documentation task records the exact Blueprint
revision, generated projections, hosted evidence, and residual product gaps.
An acceptance-owner task closes the criteria without adding product payload.

## Follow-up ordering

Product remediation is generated from the accepted Blueprint gap in dependency
order:

```text
protocol and core contracts
  -> storage and identity
  -> TLS and session
  -> transfer
  -> C ABI bridge
  -> Flutter application and presentation
  -> cross-platform end-to-end qualification
```

Each remediation evolves the canonical module in place. Parallel replacement
providers are forbidden.
