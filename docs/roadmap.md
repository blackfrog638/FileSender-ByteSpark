# Delivery roadmap

Tasks are ordered by dependency. Parallel work is safe only where owned paths
do not overlap.

## P0: engineering baseline

- [x] Repository and multi-agent harness
- [x] Native lifecycle with stable C ABI
- [x] Flutter shell and native adapter boundary
- [x] CI validation on macOS, Windows, and Linux
- [x] Platform packaging of the native dynamic library
- [x] Auditable task review, integration, acceptance, and cleanup

## P0: contracts before networking

- [x] LAN threat model and negative-test matrix
- [x] Accept the pairing and transport security design
- [x] Wire framing, version negotiation, and limits specification
- [x] Native event model and ABI callback contract
- [x] Flutter transfer application state and protocol error taxonomy
- [x] Hostile frame fixtures and bounded native parser
- [x] Hostile path and manifest fixtures

## P1: vertical slice

- [ ] LAN peer discovery with expiry and interface-change handling
- [ ] Authenticated peer pairing
- [ ] One-file offer, explicit acceptance, encrypted transfer, and commit
- [ ] Flutter peer list, send flow, receive prompt, and progress
- [ ] Cancellation and deterministic cleanup

## P1: production transfer behavior

- [ ] Multi-file manifests and directories
- [ ] Chunk scheduling, backpressure, and rate reporting
- [ ] Resume after reconnect or process restart
- [ ] Collision policy and destination selection
- [ ] Large-file, low-space, sleep/wake, and network-change tests

## Current delivery status

This is a reviewed snapshot of task records and active task branches on
2026-08-08. `.agents/records/` and task branches remain authoritative when this
summary and runtime state differ.

- Accepted P1 prerequisites: XT-018 through XT-023 and XT-051. Native
  discovery, its C ABI/Dart adapter, protected identity storage, and the TLS
  provider are delivered independently; this does not yet implement pairing
  or transfer.
- Active: XT-027 is implementing the safe one-file storage transaction.
- Claimable next: XT-024 independently reviews the identity and TLS runtimes.
  Its acceptance unlocks the XT-060 pairing-control contract and independent
  XT-062 Linux backend qualification.
- Critical path: XT-024 -> XT-060 -> XT-061 -> XT-025 -> XT-026, joining
  XT-027 at XT-028 before cancellation, transfer adapters, Flutter UI, and
  XT-032 vertical-slice acceptance. XT-062 independently gates the
  three-platform XT-032 claim.
- Engineering enabling work: XT-040 through XT-058 is accepted, covering
  compiler tooling, pinned runtime dependencies, risk and architecture gates,
  ABI and commit policy, subtractive architecture, trusted review, corrected
  immutable identity, typed defect workflows, conflict detection, the fatal
  parser repair, and attributed defect proof.

`claimable` below means all declared dependencies are accepted.
`dependency-blocked` is a roadmap scheduling view; the durable task record can
remain `ready` until an agent attempts to claim it.

## P1 execution plan

Task records are authoritative for runtime state. The roadmap checkboxes above
close only in the two acceptance tasks, after integrated cross-platform
evidence. Tasks in the same wave may run in parallel only when their owned
paths remain disjoint.

| Wave | Task | Status | Workstream | Outcome | Depends on |
| --- | --- | --- | --- | --- | --- |
| 0 | XT-018 | done | integration | native runtime/dependency ADR and leaf build boundaries | XT-017 |
| 1 | XT-019 | done | protocol | discovery v1 wire, lifecycle, limits, and vectors | XT-018 |
| 1 | XT-051 | done | integration | pinned Linux Secret Service dependency | XT-046 |
| 1 | XT-022 | done | native_core | protected identity and pairing-record storage | XT-018, XT-051 |
| 1 | XT-023 | done | native_core | TLS 1.3 and security-profile provider | XT-018, XT-015 |
| 1 | XT-027 | in_progress | native_core | safe one-file storage transaction | XT-018, XT-011 |
| 2 | XT-020 | done | native_core | multicast discovery, cache, expiry, and interface recovery | XT-019 |
| 2 | XT-024 | claimable | protocol | independent identity/TLS runtime security review | XT-022, XT-023 |
| 3 | XT-021 | done | native_bridge | discovery C ABI and Dart adapter | XT-020 |
| 3 | XT-060 | dependency-blocked | protocol | pairing-control wire profile, limits, states, and vectors | XT-024 |
| 3 | XT-062 | dependency-blocked | native_core | qualified Linux Secret Service lifecycle | XT-024, XT-051 |
| 4 | XT-061 | dependency-blocked | native_core | pre-authentication TLS certificate bounds | XT-023, XT-060 |
| 5 | XT-025 | dependency-blocked | native_core | authenticated pairing/session state machine | XT-020, XT-024, XT-060, XT-061 |
| 6 | XT-026 | dependency-blocked | native_bridge | pairing C ABI and Dart adapter | XT-021, XT-025 |
| 6 | XT-028 | dependency-blocked | native_core | authenticated one-file transfer engine | XT-006, XT-025, XT-027 |
| 7 | XT-029 | dependency-blocked | native_core | cancellation and deterministic cleanup | XT-028 |
| 8 | XT-030 | dependency-blocked | native_bridge | transfer C ABI and real Dart gateway | XT-026, XT-029 |
| 9 | XT-031 | dependency-blocked | flutter_desktop | peer, pairing, send, receive, and progress UI | XT-021, XT-026, XT-030 |
| 10 | XT-032 | dependency-blocked | integration | cross-platform P1 vertical-slice acceptance | XT-031, XT-062 |
| 11 | XT-033 | dependency-blocked | native_core | multi-file manifests and directories | XT-032 |
| 12 | XT-034 | dependency-blocked | native_core | scheduling, backpressure, fairness, and rates | XT-033 |
| 12 | XT-035 | dependency-blocked | native_core | destination selection and collision policy | XT-033 |
| 13 | XT-036 | dependency-blocked | native_core | reconnect/process-restart resume state | XT-022, XT-034, XT-035 |
| 14 | XT-037 | dependency-blocked | native_bridge | production transfer C ABI and Dart adapters | XT-036 |
| 15 | XT-038 | dependency-blocked | flutter_desktop | production multi-file and resume flow | XT-037 |
| 16 | XT-039 | dependency-blocked | integration | adverse-platform tests and final P1 acceptance | XT-038 |

Security ordering is fail-closed: XT-022 cannot compile its Linux adapter
before XT-051 proves the selected Secret Service dependency. XT-025 cannot
start until XT-024 accepts both providers, XT-060 registers the complete
pairing-control contract, and XT-061 enforces the registered certificate
ceiling. XT-062 qualifies the concrete Linux protected store independently and
blocks the three-platform XT-032 acceptance claim, not platform-independent
session development. Public C ABI work is serialized through XT-021, XT-026,
XT-030, and XT-037. ADR 0008 governs that additive surface. No task before
XT-032 may claim the vertical slice, and no task before XT-039 may claim
production P1 behavior.

## P2: hardening and release

- [x] C ABI and frame-parser fuzz smoke with corpus regression
- [ ] Session, manifest, storage, and transfer fuzz targets
- [ ] Cross-platform interoperability matrix
- [ ] Signed application packaging and update policy
- [ ] Telemetry and privacy policy
- [ ] Performance baselines and resource budgets

## Completed foundation tasks

| Task | Workstream | Outcome | State |
| --- | --- | --- | --- |
| XT-001 | protocol | threat model and proposed pairing ADR | done |
| XT-002 | protocol | v1 framing, negotiation, and limits | done |
| XT-003 | native_bridge | asynchronous event C ABI | done |
| XT-004 | flutter_desktop | transfer application state | done |
| XT-005 | integration | cross-platform native packaging | done |
| XT-006 | native_core | bounded v1 parser and fuzzing | done |
| XT-007 | integration | auditable harness governance | done |
| XT-008 | integration | remote-safe governance validation | done |
| XT-009 | protocol | proposed security-profile golden vectors | done |
| XT-010 | protocol | independently accepted pairing security design | done |
| XT-011 | protocol | hostile path and manifest contract fixtures | done |
| XT-012 | integration | task-local governance artifact ownership | done |
| XT-013 | protocol | confirmation and device-identifier blocker fixes | done |
| XT-014 | protocol | valid Ed25519 fixture keys | done |
| XT-015 | protocol | prime-subgroup key validation | done |
| XT-016 | protocol | host-independent security word-list validation | done |
| XT-017 | integration | auditable squash task integration | done |

Task state and acceptance evidence live under `.agents/records/`. Roadmap
checkboxes describe product milestones and are not the runtime task state.
