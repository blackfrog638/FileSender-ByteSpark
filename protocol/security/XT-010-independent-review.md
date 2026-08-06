# XT-010 independent pairing-security review

- Reviewer: `security-review-agent`
- Date: 2026-08-06
- Decision: blocked
- ADR disposition: keep ADR 0002 `proposed`

## Scope and method

This review independently examined ADR 0002, ADR 0004, the v1 wire
specification, the threat model, the negative-test matrix, and every file under
`protocol/testdata/security/v1/`.

The reviewer is distinct from the XT-009 owner recorded as
`security-vectors-agent`. In addition to reading the Python fixture oracle, the
reviewer independently reconstructed all six canonical object kinds from the
semantic baseline fixture in Ruby and used OpenSSL's SHA-256 and HMAC-SHA256
implementation. That second implementation matched all 14 positive vector
outputs, including:

- normalized negotiation and pairing/transport/rotation object encodings;
- all context SHA-256 values and exact TLS exporter API tuples;
- HKDF-Expand-SHA256 output `06b5482a0e78d9`;
- SAS indices `53, 1362, 84, 231, 1132` and words
  `allow prevent appear brother mirror`;
- both role-specific confirmation and transport-finished HMACs; and
- both signer-separated rotation proof messages.

This establishes independent agreement for the checked fixture mathematics. It
does not instantiate TLS, prove exporter behavior, execute Ed25519, or satisfy
the future platform and integration gates.

## Release-blocking findings

### BR-01: A valid peer rejection is accepted by the fixture verification contract

**Requirement**

ADR 0002 assigns `00` to reject and `01` to confirm, includes the decision in
`PAIR_CONFIRMATION`, and permits trust only after local confirmation and a
matching peer confirmation. Negative test P-09 requires either endpoint's
rejection to close the attempt without persisting trust. SEC-03 and SEC-18
require confirmation for the same current attempt.

**Evidence**

- `validate_vectors.py:821-832` generates positive confirmation vectors only
  with `DECISION_CONFIRM`.
- `validate_vectors.py:929-947` verifies the role, accepts either decision byte,
  and verifies the HMAC, but has no expected-decision input and returns no typed
  decision to a caller.
- `vectors.json` has role and replay negatives, but no reject output and no case
  requiring a valid authenticated reject to fail an expected-confirm check.

The following direct invocation of the checked-in verifier succeeds:

```text
message = pair_context || 01 || 00
HMAC    = 671e0eb259d4cc05f1678e2f1add3e5ab0663a7b47c9eca87a6136c9413be071
result  = verify_confirmation accepted authenticated reject decision 00
```

There is no production implementation to exploit today. The blocker is that
the normative evidence permits a future implementation to equate "valid
confirmation MAC" with affirmative peer consent, contrary to P-09. ADR
acceptance cannot rely on that incomplete failure-boundary contract.

**Required resolution**

1. Make the confirmation verification/state-machine contract distinguish an
   authenticated reject from an affirmative confirm. Trust may commit only
   when both local and peer decisions are exactly `01` for the same live
   attempt; `00` must terminally reject without a trust write.
2. Add role-specific reject vectors and a negative case in which a valid
   `decision=00` value is presented where `decision=01` is required.
3. Add invalid-decision and confirm/reject substitution cases with stable
   outcomes, mapped to P-09, SEC-03, and SEC-18.
4. Have an independent reviewer recalculate and rerun the amended vectors.

### BR-02: The stable device-identifier derivation is not byte-exact

**Requirement**

ADR 0002 says the stable device identifier is SHA-256 over a domain-separation
label and the canonical 32-byte Ed25519 public key. The threat-model
prerequisite requires exact domain-separation labels and key encodings.

**Evidence**

`docs/adr/0002-pairing-and-transport-security.md:219-222` does not specify the
label octets, concatenation or canonical envelope, or output representation.
No XT-009 operation or vector covers this derivation. Two conforming
implementations can therefore derive different identifiers from the same
pinned key, and there is no golden value that can reject an alternate label or
encoding.

The device identifier does not override the Ed25519 pin, so this is not a claim
of a current authentication bypass. It is nevertheless an incomplete
cross-platform security-profile contract for persisted peer scope and trust
metadata, and conflicts with the ADR's byte-exact objective.

**Required resolution**

1. Define the exact ASCII label bytes, unambiguous input encoding, SHA-256
   output length and representation, and relationship to the canonical public
   key.
2. Add an independent positive vector plus alternate-label, alternate-encoding,
   and wrong-key negative coverage.
3. Retain the rule that device identifiers never override exact public-key
   possession or pin matching.

## Review checklist

| Area | ADR requirement | Threat/negative mapping | Wire/vector evidence | Result |
| --- | --- | --- | --- | --- |
| Algorithms | Ed25519 identity, X25519 ECDHE, TLS 1.3, AES-128-GCM-SHA256, optional ChaCha20-Poly1305-SHA256, SHA-256/HKDF/HMAC | SEC-04, SEC-06, SEC-07; P-13, P-16, T-01 through T-05 | Exporter bytes are fixture input; no live TLS or Ed25519 | Design coherent; implementation gate retained |
| Labels | Exact pairing, SAS, confirmation, transport, rotation, and proof labels | SEC-08, SEC-19; P-05, P-19, T-07 | Positive contexts and domain-mismatch negatives | Blocked by BR-02 for device ID |
| Canonical encoding | `XNNS`, version/kind/count/length envelope, strict increasing fields, fixed lengths, no trailing bytes | SEC-08, SEC-14, SEC-19; P-19 | ADR byte-exact section; malformed canonical vectors | Pass for covered objects |
| TLS exporter context | Distinct pairing, confirmation, and transport labels; 32-byte context/output | SEC-03, SEC-05, SEC-06, SEC-08; P-03, T-06 | Exact API tuples and independently matched contexts | Pass as fixture evidence; live TLS gate retained |
| Role separation | Ordered initiator/responder keys, nonces, transcripts, confirmation and finished role bytes | SEC-08, SEC-18; P-05, T-07 | Both role outputs plus swapped-role negatives | Pass |
| SAS | HKDF Expand only, 55 MSB-first bits, fixed 2,048-word BIP39 mapping | SEC-03, SEC-18; P-01, P-11, P-16 | Pinned word-list hash and independent recomputation | Pass |
| Confirmation | Decision-bound HMAC and two-sided explicit confirmation before atomic trust | SEC-03, SEC-18; P-03, P-04, P-08 through P-10 | Confirm outputs, role and replay negatives | Blocked by BR-01 |
| Transport binding | Both identity keys, fresh nonces, profile, normalized and raw negotiation, session ID, both role finished values | SEC-05, SEC-08; T-06 through T-11 | ADR kind 04, v1 sections 2 and 6.2, both finished vectors | Pass as design evidence |
| Rotation/revocation | Authenticated bound transport, old/new proof, monotonic counter, atomic pin replacement, tombstone, full re-pair on loss/compromise | SEC-09, SEC-11, SEC-12; K-03 through K-12, T-16 | Rotation context and both signer inputs; counter/signer negatives | Message design passes; signatures/storage/state tests remain gated |
| Replay/downgrade | Fresh handshakes/nonces/session IDs, exact offers and selection, no weaker fallback, bounded duplicate handling | SEC-07 through SEC-09; P-03, P-04, P-06, T-03 through T-10, T-13 | Highest-common/exact-intersection checks and replay negatives | Pass for covered derivations; runtime duplicate state remains gated |
| Resource limits | Bounded canonical objects and strict pre-authentication concurrency, byte, message, and time ceilings | SEC-14, SEC-15; P-12, P-20, R-04 through R-13 | Canonical 1 MiB/32-field bound; threat model has 64 KiB/16-message pairing ceiling | Explicit implementation gate; pairing wire/TLS limits still absent |
| Vector independence | Independent derivation and cross-platform agreement | Negative matrix Golden level; ADR prerequisite 2 | Ruby/OpenSSL second implementation matched all 14 positives | Pass for current positive fixture outputs; blockers still require amended vectors |

## Non-blocking implementation gates

The following are deliberately not treated as evidence that the current
repository implements security behavior:

- The v1 wire specification does not yet define the pairing-control state
  machine, ALPN/profile registration, certificate cap, or pairing error values.
- The canonical decoder's 1 MiB generic limit does not replace the 64 KiB and
  16-message pre-trust pairing ceiling.
- Live TLS conformance must prove exact pinning, identity-key equality, fresh
  X25519, allowed TLS suites, exporter agreement, disabled 0-RTT/resumption, and
  no fallback on all target platforms.
- Rotation vectors cover signer input bytes, not Ed25519 signatures or atomic
  secure-storage transitions.
- Revocation, stale attempts, duplicate confirmations, storage rollback,
  cleanup races, and resource ceilings still require the negative matrix's
  unit, integration, platform, and fuzz evidence.

These gates are already represented as prerequisites and do not by themselves
change the ADR decision in this review. BR-01 and BR-02 do.

## Decision

ADR 0002 remains `proposed`. XT-010 must remain blocked until both blockers are
resolved without weakening SEC-03, SEC-08, SEC-18, P-09, exact pinning, or
fail-closed behavior. After amended vectors pass an independent recalculation,
XT-010 may return to `in_progress` for a new acceptance review.
