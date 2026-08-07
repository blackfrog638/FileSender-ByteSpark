# ADR 0008: P1 operation C ABI

- Status: proposed
- Date: 2026-08-07
- Initial owner task: XT-021
- Event model: `docs/adr/0003-asynchronous-native-event-abi.md`

## Context

Discovery, pairing, transfer, destination policy, and resume must cross the
Flutter/native boundary without exposing C++ types, borrowed asynchronous
memory, secrets, or unbounded attacker-controlled values. The existing ABI
only supports engine lifecycle and a bounded wakeup-and-drain event queue.

## Proposed boundary

P1 operations will extend the C ABI additively with struct-size/version
negotiation, opaque operation identifiers, copied command inputs, bounded
events, and snapshot pagination for overflow recovery. Presentation may request
actions but cannot inject identity keys, transcript values, trust decisions
other than explicit confirm/reject, protocol frames, or peer-controlled local
paths.

XT-021 will establish the common operation and discovery shape. XT-026, XT-030,
and XT-037 may extend the accepted pattern only with integration-owner review
and ABI regression tests.

## Acceptance boundary

The ADR remains proposed until the first discovery extension proves:

- existing ABI v1 lifecycle clients remain compatible;
- every asynchronous payload is copied into caller-owned bounded storage;
- short structs, unknown versions, invalid states, overflow, stale IDs, and
  shutdown races fail deterministically;
- Dart retains no native pointer past a call boundary;
- packaged macOS, Windows, and Linux applications load and exercise the API.
