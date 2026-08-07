---
id: XT-049
title: Protocol public headers
state: ready
workstream: native_core
owner: unassigned
depends_on:
  - XT-048
owned_paths:
  - .agents/architecture/modules.json
  - native/include/xnn_transfer/protocol/**
  - native/src/protocol/**
  - native/tests/protocol/**
  - native/fuzz/protocol/**
  - tool/harness/architecture_test.py
  - tool/harness/architecture_test_test.py
contract_changes: []
handoff: .agents/handoffs/XT-049.md
---

## Outcome

Expose the protocol parser through `native/include/xnn_transfer/protocol/`
without placing any `native/src` directory on a target's PUBLIC include path.

## Context

XT-042 introduced the native dependency gate and XT-048 registered canonical
module roots. The protocol target currently exports `native/src` only to make
`protocol/v1_parser.hpp` visible, which also makes every private native header
reachable to session and transfer consumers.

## Constraints

- Preserve every parser type, function signature, namespace, and wire behavior.
- Move the public parser header; do not leave a compatibility forwarding header
  under `native/src`.
- PUBLIC include directories may expose `native/include` only. Module-local
  source directories remain PRIVATE or require no include-directory entry.
- Update production, test, and fuzz consumers to use the canonical
  `xnn_transfer/protocol/v1_parser.hpp` include.
- Add a negative architecture fixture proving that a native source directory
  cannot be exported through PUBLIC or INTERFACE CMake visibility.

## Architecture change

The record declares `refactor` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 2 risk dimension in the task record. Every
non-none risk must name commands that also appear in `verification.commands`.

## Acceptance criteria

- [ ] The protocol header lives under `native/include/xnn_transfer/protocol/`.
- [ ] `xnn_transfer_protocol` no longer exports `native/src`.
- [ ] Parser, test, and fuzz includes use the canonical public include.
- [ ] Architecture tests reject PUBLIC or INTERFACE native source directories.
- [ ] Existing parser and fuzz behavior remains unchanged.
- [ ] Repository verification passes.

## Verification

```bash
make architecture-test
make native-test
make verify
```
