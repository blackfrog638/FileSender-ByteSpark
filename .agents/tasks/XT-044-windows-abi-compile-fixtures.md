---
id: XT-044
title: Windows abi compile fixtures
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-043
owned_paths:
  - .agents/**
  - native/tests/abi/**
contract_changes: []
handoff: .agents/handoffs/XT-044.md
---

## Outcome

Make ABI compile fixtures deterministic on Windows by evaluating them during
the parent CMake configure instead of starting recursive CMake builds from a
CTest subprocess.

## Context

XT-043 added the ABI v1 gate required by ADR 0001 and ADR 0003. Its first
three-platform CI run passed Linux and macOS but the Windows ABI step remained
inside the runtime recursive compile fixture for more than ten minutes. The
fixture mechanism must fail promptly without weakening any v1 assertion.

## Constraints

- Do not change the production C ABI, frozen v1 declarations, required export
  set, or legacy-client behavior.
- Preserve positive coverage for additive struct tails and symbols and negative
  coverage for changed values, field types, offsets, and signatures.
- Use the parent build's selected compiler and generator through CMake
  `try_compile`; CTest must not launch nested configure or build processes.
- Give every runtime ABI test an explicit timeout so a platform-specific loader
  or lifecycle failure terminates CI promptly.

## Risk profile

Resolve every schema version 2 risk dimension in the task record. Every
non-none risk must name commands that also appear in `verification.commands`.

## Acceptance criteria

- [ ] Configure-time fixtures accept additive evolution and reject all four
  breaking mutations with the existing shared assertions.
- [ ] No ABI CTest starts Python, CMake, a compiler, or a nested build.
- [ ] Layout, legacy client, and export probes have explicit timeouts.
- [ ] `make abi-compat-test` and the three native CI platforms complete.
- [ ] No production contract changes.
- [ ] Repository verification passes.

## Verification

```bash
make abi-compat-test
make verify
```
