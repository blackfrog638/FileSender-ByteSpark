# Delivery roadmap

Tasks are ordered by dependency. Parallel work is safe only where owned paths
do not overlap.

## P0: engineering baseline

- [x] Repository and multi-agent harness
- [x] Native lifecycle with stable C ABI
- [x] Flutter shell and native adapter boundary
- [ ] CI validation on macOS, Windows, and Linux
- [ ] Platform packaging of the native dynamic library

## P0: contracts before networking

- [ ] Threat model and pairing ADR
- [ ] Wire framing, version negotiation, and limits specification
- [ ] Native event model and ABI callback contract
- [ ] Transfer state machine and error taxonomy
- [ ] Test fixtures for hostile frames, paths, and manifests

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

- [ ] Protocol fuzzing and corpus regression tests
- [ ] Cross-platform interoperability matrix
- [ ] Signed application packaging and update policy
- [ ] Telemetry and privacy policy
- [ ] Performance baselines and resource budgets

## Suggested first parallel task split

| Task | Workstream | Owned area | Dependency |
| --- | --- | --- | --- |
| XT-001 | protocol | threat model and pairing ADR | harness |
| XT-002 | protocol | framing and limits specification | harness |
| XT-003 | native_bridge | asynchronous event C ABI | XT-002 |
| XT-004 | flutter_desktop | UI state and repository contract | harness |
| XT-005 | integration | native library desktop packaging | harness |

XT-001, XT-002, XT-004, and XT-005 can start in parallel. XT-003 waits for the
wire error model so native and Flutter error semantics use one vocabulary.
