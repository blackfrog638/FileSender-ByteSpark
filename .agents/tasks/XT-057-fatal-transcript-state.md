---
id: XT-057
title: Fatal transcript state
state: ready
workstream: protocol
owner: unassigned
depends_on:
  - XT-056
owned_paths:
  - native/include/xnn_transfer/protocol/v1_parser.hpp
  - native/src/protocol/v1_parser.cpp
  - native/tests/protocol/v1_parser_test.cpp
contract_changes:
  - Restore protocol v1 connection-fatal transcript termination
handoff: .agents/handoffs/XT-057.md
---

## Outcome

After any connection-fatal parser error, the same `TranscriptParser` rejects
all later input without parsing it.

## Defect contract

Resolve every `defect` field in the schema version 3 record. The reproduction
commit may remain empty during development but is required before review.


## Context

Protocol v1 section 14 makes malformed framing, message-ID failures, invalid
shared state, and negotiation failures connection-fatal because parser state is
no longer trustworthy. The audited implementation returns those errors but
does not retain a terminal state. XT-055 and XT-056 now provide the dual-state
proof and scheduling gates needed to fix this as a governed bug rather than a
feature.

## Constraints

- Commit the failing regression test before the implementation fix and record
  that commit as `reproduction_commit`.
- Use the existing trusted `native_test` gate for both reproduction and head.
- Classify fatality from protocol v1 section 14; do not make every
  stream-scoped post-binding error connection-fatal.
- Once terminal, reject later input before envelope parsing or message-ID
  mutation.
- Preserve all valid golden transcripts and directional message-ID behavior.
- Add no allocation, retry, networking, or error-response behavior.

## Architecture change

The record declares `refactor` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] A truncated frame followed by a valid HELLO fails on the second call.
- [ ] A malformed stream-scoped transfer after binding does not poison a later
      valid control frame.
- [ ] The reproduction commit fails `make native-test`.
- [ ] The fixed head passes the same gate and records generated dual-state
      proof evidence.
- [ ] Existing golden transcripts and hostile parser tests pass.
- [ ] Protocol specification remains unchanged because this restores section
      14 rather than changing it.
- [ ] Repository verification passes.

## Verification

```bash
make native-test
make security-test
make verify
```
