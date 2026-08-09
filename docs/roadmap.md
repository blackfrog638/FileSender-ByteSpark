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

<!-- roadmap-id: RM-P1-DISCOVERY -->
- [ ] LAN peer discovery with expiry and interface-change handling
<!-- roadmap-id: RM-P1-PAIRING -->
- [ ] Authenticated peer pairing
<!-- roadmap-id: RM-P1-ONE-FILE-TRANSFER -->
- [ ] One-file offer, explicit acceptance, encrypted transfer, and commit
<!-- roadmap-id: RM-P1-FLUTTER-FLOW -->
- [ ] Flutter peer list, send flow, receive prompt, and progress
<!-- roadmap-id: RM-P1-CANCELLATION -->
- [ ] Cancellation and deterministic cleanup

## P1: production transfer behavior

<!-- roadmap-id: RM-P1-MULTI-FILE -->
- [ ] Multi-file manifests and directories
<!-- roadmap-id: RM-P1-SCHEDULING -->
- [ ] Chunk scheduling, backpressure, and rate reporting
<!-- roadmap-id: RM-P1-RESUME -->
- [ ] Resume after reconnect or process restart
<!-- roadmap-id: RM-P1-COLLISION -->
- [ ] Collision policy and destination selection
<!-- roadmap-id: RM-P1-ADVERSE-PLATFORM -->
- [ ] Large-file, low-space, sleep/wake, and network-change tests

## Current delivery status

Runtime snapshots are deliberately not checked into this document. Generate
the current requirement and task view from the approved Delivery Plan,
backlog, and durable task records:

```bash
python3 tool/harness/delivery_plan.py status DP-P1-DELIVERY
tool/harness/agent.sh list
```

The generated view reports each requirement as `planned`, `in-progress`,
`partially-delivered`, `acceptance-ready`, or `accepted`. Ready tasks are
reported as `claimable` only when every declared dependency is durably `done`;
otherwise they are `dependency-blocked`. `agent.sh list` additionally resolves
active task branches.

`DP-P1-DELIVERY` is the reviewed requirement-to-task mapping. The backlog is
the only dependency graph. XT-032 uniquely accepts the vertical slice, and
XT-039 uniquely accepts production P1 behavior; only those tasks may close the
corresponding checkboxes above after cross-platform evidence passes.

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
