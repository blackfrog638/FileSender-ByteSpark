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
| XT-011 | protocol | hostile path and manifest contract fixtures | done |
| XT-012 | integration | task-local governance artifact ownership | done |
| XT-013 | protocol | confirmation and device-identifier blocker fixes | done |
| XT-014 | protocol | valid Ed25519 fixture keys | done |
| XT-015 | protocol | prime-subgroup key validation | done |

Task state and acceptance evidence live under `.agents/records/`. Roadmap
checkboxes describe product milestones and are not the runtime task state.
