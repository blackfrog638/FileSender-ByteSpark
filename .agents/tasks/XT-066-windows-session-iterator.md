---
id: XT-066
title: Windows session iterator
state: ready
workstream: native_core
owner: unassigned
depends_on:
  - XT-025
owned_paths:
  - native/src/session/wire.cpp
  - native/tests/session/CMakeLists.txt
  - native/tests/session/standard_library_include_test.py
delivery_plan: DP-WINDOWS-ITERATOR-REPAIR
requirement_ids:
  - REQ-WINDOWS-SESSION-COMPILE
delivery_role: implementation_acceptance
contract_changes: []
handoff: .agents/handoffs/XT-066.md
---

## Outcome

Restore supported Windows builds by making the session wire translation unit
directly include the standard-library declaration required by
`std::back_inserter`.

## Defect contract

Resolve every `defect` field in the schema version 3 record. The reproduction
commit may remain empty during development but is required before review.


## Context

XT-025 introduced capability intersection in `native/src/session/wire.cpp`.
GitHub Actions run `31346642027` proves that MSVC 14.44 rejects the source with
`C2039` and `C3861` because `<iterator>` is not directly included. Clang and
libc++ accept the same source through an incidental transitive include, so the
local macOS gate did not expose the defect.

## Constraints

- Add a deterministic native regression gate before the implementation fix and
  record that failing commit as `reproduction_commit`.
- The regression must fail with the exact declared fingerprint when the direct
  include is absent on any host toolchain.
- Fix only include ownership; do not change pairing negotiation behavior,
  protocol bytes, capability ordering, public APIs, or build targets.
- Do not accept the task until both `Native (windows-2022)` and
  `Desktop native bundle (windows-2022)` pass for the integrated delivery SHA.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] The trusted `native_test` gate deterministically rejects removal of the
      direct `<iterator>` include.
- [ ] MSVC compiles `xnn_transfer_session` without `C2039`, `C3861`, or the
      derived `C2672` failure.
- [ ] GitHub Actions run evidence for the integrated delivery SHA reports both
      Windows native and desktop bundle jobs successful.
- [ ] Repository verification passes.

## Verification

```bash
make native-test
make verify
```
