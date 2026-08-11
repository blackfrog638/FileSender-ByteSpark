---
id: XT-093
title: Transport context wire contract
state: ready
workstream: protocol
owner: unassigned
depends_on:
  - XT-082
owned_paths:
  - .agents/records/XT-093.json
  - .agents/tasks/XT-093-transport-context-wire-contract.md
  - .agents/handoffs/XT-093.md
  - protocol/spec/v1.md
  - docs/adr/0002-pairing-and-transport-security.md
  - native/include/xnn_transfer/protocol/v1_parser.hpp
  - native/src/protocol/**
  - native/tests/protocol/**
  - native/fuzz/protocol/**
  - protocol/testdata/v1/**
delivery_plan: DP-P1-TRANSPORT-COMPOSITION
requirement_ids:
  - REQ-P1-TRANSPORT-COMPOSITION-CONTRACT
delivery_role: implementation
contract_changes:
  - Add required v1 HELLO profile-context fields for fresh session nonces and the initiator transfer session identifier.
  - Clarify ADR 0002 role ordering and the normalized-versus-raw transcript binding boundary.
handoff: .agents/handoffs/XT-093.md
---

## Outcome

Define and implement the missing v1 profile-context exchange so both peers
construct the same fresh ADR 0002 transport context before either endpoint can
dispatch transfer metadata.

## Context

XT-069 supplies an exact-pin provisional TLS byte stream and XT-070 must bind
that stream before composing it with the transfer engine. ADR 0002 requires
role-ordered session nonces and a transfer session identifier, but the current
v1 HELLO schema has no normative source for those inputs.

This task extends the proposed v1 HELLO contract with critical field 11,
`session_nonce`, required in both directions as 32 fresh octets, and critical
field 12, `transfer_session_id`, required only in the initiator HELLO as 16
fresh nonzero octets. The profile-context values remain separate fields in the
ADR 0002 transport object while their exact encodings remain covered by the
raw negotiation transcript. They do not alter the 13-field normalized
negotiation object.

## TDD contract

Before production edits, add only protocol tests or fixtures that fail with
the exact line `FAILED: transport context profile inputs are not enforced`.
Checkpoint that Red revision with `agent.sh checkpoint XT-093 red`.

## Constraints

- Generate profile-context values from the approved OS CSPRNG. Reject missing,
  duplicate, malformed, zero, role-swapped, or reused values.
- Preserve exact unknown noncritical bytes in the raw transcript, but do not
  use undocumented extensions as profile-context inputs.
- Keep parser allocation, frame, field, and transcript bounds unchanged or
  tighter. Fragmented input must not bypass validation or deadlines.
- Reject transfer-scoped frames before the matching negotiation ACK and both
  role-specific TRANSPORT_FINISHED conditions.
- Update `protocol/spec/v1.md` and ADR 0002 together. Do not change the public
  C ABI, TLS suites, ALPN values, persisted state, or transfer framing.

## Architecture change

The record declares `refactor` mode for the canonical `protocol` module. It
extends the existing v1 parser and fixtures in place without adding another
protocol provider or target.

## Risk profile

The schema-v4 record binds critical security and compatibility risk to
`native_test`, `protocol_vectors`, `security_test`, and `verify`.

## Acceptance criteria

- [ ] Initiator and responder HELLO frames carry the required role-specific
      profile-context fields with byte-exact parser and golden coverage.
- [ ] Missing, duplicate, zero, malformed, wrong-role, replayed, and tampered
      profile-context values fail closed before transfer dispatch.
- [ ] Normalized negotiation remains semantic and stable while the exact raw
      transcript binds the new field encodings in role order.
- [ ] Fragmented frames and hostile declared lengths remain bounded.
- [ ] `protocol/spec/v1.md` and ADR 0002 describe one interoperable source and
      exchange for every `BuildTransportContext` input.
- [ ] No C ABI, ALPN, TLS suite, or transfer-message layout changes.
- [ ] `make native-test`, protocol vectors, `make security-test`, and
      `make verify` pass.

## Verification

```bash
make native-test
python3 protocol/testdata/v1/validate_vectors.py
make security-test
make verify
```
