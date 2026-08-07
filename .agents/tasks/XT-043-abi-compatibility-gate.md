---
id: XT-043
title: Abi compatibility gate
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-042
owned_paths:
  - AGENTS.md
  - .agents/**
  - Makefile
  - tool/harness/**
  - .github/workflows/ci.yml
  - native/CMakeLists.txt
  - native/tests/abi/**
contract_changes: []
handoff: .agents/handoffs/XT-043.md
---

## Outcome

Reject changes that break ABI v1 constants, enum values, struct prefixes,
function signatures, required exports, or an already compiled-style legacy
client on macOS, Windows, or Linux.

## Context

ADR 0001 defines the versioned C ABI boundary, and ADR 0003 requires additive
ABI v1 evolution with feature-detected exports. Existing tests exercise current
callers but compile against the current header, so a coordinated breaking
header and implementation change can still pass. XT-042 makes architecture
direction mechanical; this task makes the public binary contract mechanical.

## Constraints

- Do not change `native/include/xnn_transfer/c_api.h` or create a new ABI
  version in this task.
- Freeze the accepted ABI v1 caller view under tests; production code must not
  include the frozen test header.
- Allow additive symbols and struct tail fields while preserving every v1
  constant, enum value, field offset, function signature, and required export.
- Compile and run a legacy client against the current dynamic library rather
  than relying only on text comparison.
- Resolve required exports from the built library on macOS, Windows, and Linux
  without depending on one platform's `nm` or `dumpbin` output format.
- Keep all probes deterministic, generated-artifact free, and part of
  `make verify` and the native CI matrix.

## Risk profile

Resolve every schema version 2 risk dimension in the task record. Every
non-none risk must name commands that also appear in `verification.commands`.

## Acceptance criteria

- [ ] Compile-time checks preserve ABI v1 macro values, enum values, struct
  alignment and prefix offsets, and public function pointer signatures.
- [ ] A client compiled from the frozen v1 header links to the current library
  and exercises create, callbacks, lifecycle, polling, and destruction.
- [ ] A binary export probe resolves every required ABI v1 symbol from the
  actual shared library.
- [ ] Negative compile fixtures demonstrate that changed values, offsets, and
  signatures fail the compatibility assertions.
- [ ] `make abi-compat-test`, `make verify`, and all three native CI platforms
  execute the ABI tests.
- [ ] Additive symbols and fields remain explicitly permitted.
- [ ] Repository verification passes.

## Verification

```bash
make abi-compat-test
make verify
```
