---
id: XT-067
title: Windows test macro isolation
state: ready
workstream: native_core
owner: unassigned
depends_on:
  - XT-029
owned_paths:
  - native/tests/session/test_support.hpp
  - native/tests/transfer/windows_macro_policy_test.py
  - native/tests/transfer/CMakeLists.txt
delivery_plan: DP-WINDOWS-MACRO-REPAIR
requirement_ids:
  - REQ-WINDOWS-TEST-MACRO-ISOLATION
delivery_role: implementation_acceptance
contract_changes: []
handoff: .agents/handoffs/XT-067.md
---

## Outcome

Restore the supported Windows native build by preventing the Windows SDK
`min` and `max` macros from polluting the session and transfer test targets.

## Defect contract

Resolve every `defect` field in the schema version 3 record. The reproduction
commit may remain empty during development but is required before review.


## Context

GitHub Actions run `31350356920` proves that the XT-066 iterator fix allows the
production session library to compile under MSVC 14.44. The native job then
fails in session and transfer tests because Windows headers expose function-like
`min` and `max` macros. These macros rewrite `std::min` and
`std::numeric_limits::max` expressions into invalid tokens.

The desktop native bundle job already passes because its Windows runner target
defines `NOMINMAX`. Shared native test support needs to establish the same
preprocessing contract and remove macros that leaked from earlier includes.

## Constraints

- Add a deterministic native regression gate before the implementation fix and
  record that failing commit as `reproduction_commit`.
- The regression must fail with the exact declared fingerprint when shared test
  support omits `NOMINMAX` or leaves an existing `min` or `max` macro defined,
  independent of the host platform.
- Apply the isolation only to shared native test support. Do not change
  production source, public APIs, protocol bytes, transfer behavior, generated
  Flutter runner files, or global compiler policy.
- Do not accept the task until the complete GitHub Actions matrix passes for
  the integrated delivery SHA.

## Architecture change

The record declares `refactor` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] The trusted `native_test` gate deterministically rejects incomplete
      `NOMINMAX`, `min`, or `max` isolation in shared native test support.
- [ ] MSVC compiles session and transfer tests without Windows `min` or `max`
      macro expansion errors.
- [ ] GitHub Actions reports every job successful for the integrated delivery
      SHA, including `Native (windows-2022)` and
      `Desktop native bundle (windows-2022)`.
- [ ] Repository verification passes.

## Verification

```bash
make native-test
make verify
```
