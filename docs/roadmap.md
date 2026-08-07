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

## P1 execution plan

Task records are authoritative for runtime state. The roadmap checkboxes above
close only in the two acceptance tasks, after integrated cross-platform
evidence. Tasks in the same wave may run in parallel only when their owned
paths remain disjoint.

| Wave | Task | Workstream | Outcome | Depends on |
| --- | --- | --- | --- | --- |
| 0 | XT-018 | integration | native runtime/dependency ADR and leaf build boundaries | XT-017 |
| 1 | XT-019 | protocol | discovery v1 wire, lifecycle, limits, and vectors | XT-018 |
| 1 | XT-050 | integration | pinned Linux Secret Service dependency | XT-046 |
| 1 | XT-022 | native_core | protected identity and pairing-record storage | XT-018, XT-050 |
| 1 | XT-023 | native_core | TLS 1.3 and security-profile provider | XT-018, XT-015 |
| 1 | XT-027 | native_core | safe one-file storage transaction | XT-018, XT-011 |
| 2 | XT-020 | native_core | multicast discovery, cache, expiry, and interface recovery | XT-019 |
| 2 | XT-024 | protocol | independent identity/TLS runtime security review | XT-022, XT-023 |
| 3 | XT-021 | native_bridge | discovery C ABI and Dart adapter | XT-020 |
| 3 | XT-025 | native_core | authenticated pairing/session state machine | XT-020, XT-024 |
| 4 | XT-026 | native_bridge | pairing C ABI and Dart adapter | XT-021, XT-025 |
| 4 | XT-028 | native_core | authenticated one-file transfer engine | XT-006, XT-025, XT-027 |
| 5 | XT-029 | native_core | cancellation and deterministic cleanup | XT-028 |
| 6 | XT-030 | native_bridge | transfer C ABI and real Dart gateway | XT-026, XT-029 |
| 7 | XT-031 | flutter_desktop | peer, pairing, send, receive, and progress UI | XT-021, XT-026, XT-030 |
| 8 | XT-032 | integration | cross-platform P1 vertical-slice acceptance | XT-031 |
| 9 | XT-033 | native_core | multi-file manifests and directories | XT-032 |
| 10 | XT-034 | native_core | scheduling, backpressure, fairness, and rates | XT-033 |
| 10 | XT-035 | native_core | destination selection and collision policy | XT-033 |
| 11 | XT-036 | native_core | reconnect/process-restart resume state | XT-022, XT-034, XT-035 |
| 12 | XT-037 | native_bridge | production transfer C ABI and Dart adapters | XT-036 |
| 13 | XT-038 | flutter_desktop | production multi-file and resume flow | XT-037 |
| 14 | XT-039 | integration | adverse-platform tests and final P1 acceptance | XT-038 |

Security ordering is fail-closed: XT-022 cannot complete its Linux adapter
before XT-050 proves the selected Secret Service dependency, and XT-025 cannot
start until XT-024 accepts both the protected identity and TLS providers.
Public C ABI work is serialized through XT-021, XT-026, XT-030, and XT-037.
ADR 0008 governs that additive surface. No task before XT-032 may claim the
vertical slice, and no task before XT-039 may claim production P1 behavior.

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
