---
id: XT-060
title: Pairing control wire contract
state: ready
workstream: protocol
owner: unassigned
depends_on:
  - XT-024
owned_paths:
  - protocol/spec/pairing-v1.md
  - protocol/spec/v1.md
  - protocol/testdata/security/v1/pairing-control-vectors.json
  - protocol/testdata/security/v1/validate_vectors.py
  - protocol/testdata/security/v1/README.md
  - protocol/security/negative-test-matrix.md
  - docs/adr/0002-pairing-and-transport-security.md
delivery_plan: DP-P1-DELIVERY
requirement_ids:
  - REQ-P1-PAIRING
delivery_role: implementation
contract_changes:
  - Register the versioned pairing-control wire profile and production identifiers
  - Define certificate limits, terminal errors, deadlines, and duplicate semantics
handoff: .agents/handoffs/XT-060.md
---

## Outcome

Define the complete versioned pairing-control wire contract that XT-025 can
implement without inventing ALPN bytes, state transitions, resource ceilings,
or replay behavior.


## Context
XT-024 accepted the XT-022 identity and XT-023 TLS providers as lower-level
inputs but recorded XR-024-01: ADR 0002 has no registered production
pairing-control messages, ALPN/profile bytes, certificate ceiling, terminal
errors, timeouts, or duplicate handling. `protocol/spec/v1.md` deliberately
does not select those semantics.

## Constraints
- Specify first-pairing, explicit local decision, authenticated confirmation,
  rejection, timeout, cancellation, and established reconnect transitions.
- Register exact ALPN and profile bytes with one canonical encoding. Unknown,
  duplicate, reordered, missing, trailing, or noncanonical fields fail closed.
- Define pre-auth message, byte, attempt, deadline, and certificate limits
  before implementation allocates or dispatches hostile input.
- Bind roles, endpoint identities, transcript digest, exporter context,
  negotiation result, pairing attempt, and replay handling to ADR 0002.
- Define stable machine-readable terminal errors and duplicate/retransmission
  semantics without revealing trust state to unauthenticated peers.
- Add positive and hostile golden vectors with an executable validator.
- Update the compatibility section and ADR 0002. This task defines a public
  wire contract and therefore requires integration-owner review.
- Do not implement the C++ session state machine, TLS certificate enforcement,
  C ABI, Flutter flow, or transfer behavior.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] `pairing-v1.md` defines every message, state, transition, timeout,
      terminal error, retransmission rule, and hostile-input ceiling.
- [ ] Production ALPN/profile identifiers and certificate limits are unique,
      byte exact, versioned, and referenced from protocol v1 and ADR 0002.
- [ ] Positive vectors cover first pairing, rejection, and paired reconnect;
      hostile vectors cover malformed, duplicate, replayed, role-swapped,
      oversized, unknown-profile, and downgrade input.
- [ ] The trusted security-vector gate validates all new vectors.
- [ ] Compatibility rules reject silent reinterpretation of registered bytes.
- [ ] Repository verification passes.

## Verification

```bash
python3 protocol/testdata/security/v1/validate_vectors.py
make security-test
make verify
```
