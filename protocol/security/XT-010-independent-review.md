# XT-010 independent pairing-security review

- Reviewer: `security-review-agent`
- Date: 2026-08-06
- Decision: blocked
- ADR disposition: keep ADR 0002 `proposed`
- Review cycle: restored post-XT-013 independent re-review

## Scope and method

This review independently examined ADR 0002, ADR 0004, the v1 wire
specification, the threat model, the negative-test matrix, and every file under
`protocol/testdata/security/v1/`.

The reviewer is distinct from the XT-009 owner recorded as
`security-vectors-agent` and the XT-013 owner recorded as
`security-vector-fix-agent`. The restored review read the complete XT-013
change and did not treat its handoff or Python validator as independent
evidence.

The original review independently reconstructed all six then-defined
canonical object kinds from the semantic baseline fixture in Ruby and used
OpenSSL's SHA-256 and HMAC-SHA256 implementation. That path matched all 14
original positive vectors. The restored review used a separate inline Ruby
encoder over semantic fixture fields, OpenSSL HMAC/SHA-256, direct OpenSSL CLI
calculations, and an RFC 8032 compressed-point decoder. It independently
recalculated the three XT-013 positive additions and exercised the trust gate
without importing or invoking `validate_vectors.py`.

The Python validator still passes all 17 positive and 32 negative cases, and a
structured comparison confirms that XT-013 did not change the 14 pre-existing
positive outputs or 22 pre-existing negative outcomes. Those results are
supporting checks, not the basis for the disposition below.

## Original finding disposition

### BR-01: closed

ADR 0002 now returns typed `authenticated_reject` and `affirmative_confirm`
results and permits trust only when the local and peer decisions are both
exactly `01` for the current context and role. The amended vectors cover both
role-specific reject HMACs, local rejection, authenticated peer rejection,
invalid decisions, and both decision-substitution directions.

The independent reconstruction produced:

| Sender | Confirm HMAC | Reject HMAC | Reject result | Trust on reject |
| ---: | --- | --- | --- | --- |
| Initiator `01` | `f90a5ddbecbbafeb7e9a2c3287404f5c313aef51ac60049506e8c6f67d78bcfd` | `671e0eb259d4cc05f1678e2f1add3e5ab0663a7b47c9eca87a6136c9413be071` | `authenticated_reject` | false |
| Responder `02` | `ae63dc5e1462dd80bcfc931111faef81fbcd560e4de9c88ef8e44a3f6a041a85` | `e6eaf4d5e6a30b6bfd4f25f3f267fb51b951633e741189ff092bffd0b2e4da29` | `authenticated_reject` | false |

The same implementation permitted trust for confirm/confirm only, rejected a
local `00`, rejected a valid peer `00`, rejected decision `02`, and rejected a
decision substitution carrying the old HMAC. BR-01 is closed without weakening
P-09, SEC-03, or SEC-18.

### BR-02: byte-exact contract closed; required evidence remains blocked

ADR 0002 now defines the exact 32-byte ASCII label, complete kind-`07`
canonical envelope, RFC 8032 raw public-key representation, SHA-256 digest,
32-byte binary output, and 64-character lowercase hexadecimal text form. It
also states that the identifier never overrides possession or exact pinning.
The independent encoder produced:

```text
device_identifier_input =
584e4e53010700020000004c000100000020
586e6e5472616e7366657220646576696365206964656e746966696572207631
000200000020
404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f

device_identifier =
c0a74fca4f0e4dd1fc6d7c7ad7e8478b6096d35ec8fab8566234abfd4f5c00f3
```

Direct OpenSSL CLI SHA-256 produced the same digest. The label, canonical
input, output representation, alternate-label, alternate-encoding, and
alternate-output requirements are now byte-exact. The required wrong-key
evidence is not valid, however, because of BR-03.

## Release-blocking findings

### BR-03: The device-ID wrong-key vector is not a valid Ed25519 public key

- Severity: release blocker for ADR acceptance
- Confidence: 0.99

**Requirement**

ADR 0002 requires kind-`07` field 2 to be a canonical 32-octet compressed
Edwards-y public key defined by RFC 8032 and to be the same key used for
private-key-possession verification. BR-02 specifically required a wrong-key
negative, not merely another 32-byte string.

**Evidence**

- `vectors.json:912-927` describes the wrong-key input as a canonical Ed25519
  public key but uses baseline responder bytes `60 61 ... 7f`.
- An independent RFC 8032 decoder accepted the RFC basepoint, the RFC 8032 test
  public key, and the fixture initiator key, but rejected `60 61 ... 7f`
  because its encoded `y` has no Edwards25519 `x` solution.
- `validate_vectors.py:734-748` checks only that the input is 32 bytes before
  hashing it. It therefore reports `DEVICE_IDENTIFIER_MISMATCH` for a value
  that cannot be the proved Ed25519 identity required by the ADR.
- OpenSSL 3.6.3 `pkey -pubcheck` reports this ECX container as valid, but the
  non-FIPS OpenSSL ECX validation path checks key length and presence rather
  than Edwards point decompression. It is not contrary evidence for RFC 8032
  point validity.

The independent hash of the invalid bytes is
`beb8f396d6334fc73d8e35a2dc834dca4984de6d0561e7599c9b36d28e5d3457`,
which agrees with the fixture oracle's byte operation but does not make those
bytes an Ed25519 public key. A strict implementation can reject the input as
an invalid key before device-ID comparison, so the checked vector does not
establish the required cross-platform wrong-valid-key behavior.

This is not a current authentication bypass: no production pairing or
device-ID implementation exists, and exact pinning remains mandatory. It is a
normative golden-evidence defect that blocks accepting the profile.

**Required resolution**

1. Use a second, independently generated and validated Ed25519 public key for
   the wrong-key case without reusing the invalid baseline responder bytes.
2. Assert that fixture values described as canonical Ed25519 public keys pass
   RFC 8032 decoding, and add a separate invalid-point negative if such input
   handling is part of the fixture contract.
3. Recompute the wrong-key device identifier and have an independent reviewer
   verify the amended result.

## Review checklist

| Area | ADR requirement | Threat/negative mapping | Wire/vector evidence | Result |
| --- | --- | --- | --- | --- |
| Algorithms | Ed25519 identity, X25519 ECDHE, TLS 1.3, AES-128-GCM-SHA256, optional ChaCha20-Poly1305-SHA256, SHA-256/HKDF/HMAC | SEC-04, SEC-06, SEC-07; P-13, P-16, T-01 through T-05 | Exporter bytes are fixture input; no live TLS; independent RFC 8032 fixture-key check | Blocked by BR-03 fixture validity; runtime gate retained |
| Labels | Exact pairing, SAS, confirmation, transport, rotation, proof, and device-ID labels | SEC-08, SEC-19; P-05, P-19, T-07 | Positive contexts and domain-mismatch negatives | Pass |
| Canonical encoding | `XNNS`, version/kind/count/length envelope, strict increasing fields, fixed lengths, no trailing bytes | SEC-08, SEC-14, SEC-19; P-19 | ADR byte-exact section; malformed canonical vectors | Pass for covered objects |
| TLS exporter context | Distinct pairing, confirmation, and transport labels; 32-byte context/output | SEC-03, SEC-05, SEC-06, SEC-08; P-03, T-06 | Exact API tuples and independently matched contexts | Pass as fixture evidence; live TLS gate retained |
| Role separation | Ordered initiator/responder keys, nonces, transcripts, confirmation and finished role bytes | SEC-08, SEC-18; P-05, T-07 | Both role outputs plus swapped-role negatives | Pass |
| SAS | HKDF Expand only, 55 MSB-first bits, fixed 2,048-word BIP39 mapping | SEC-03, SEC-18; P-01, P-11, P-16 | Pinned word-list hash and independent recomputation | Pass |
| Confirmation | Decision-bound HMAC and two-sided explicit confirmation before atomic trust | SEC-03, SEC-18; P-03, P-04, P-08 through P-10 | Confirm/reject outputs, trust gate, role/replay/substitution negatives | Pass; BR-01 closed |
| Device identifier | Kind-`07` over the exact RFC 8032 pin; canonical digest and text | SEC-04, SEC-08, SEC-19; T-02 | Independent input/digest match; alternate representations rejected | Blocked by BR-03 wrong-key fixture |
| Transport binding | Both identity keys, fresh nonces, profile, normalized and raw negotiation, session ID, both role finished values | SEC-05, SEC-08; T-06 through T-11 | ADR kind 04, v1 sections 2 and 6.2, both finished vectors | Pass as design evidence |
| Rotation/revocation | Authenticated bound transport, old/new proof, monotonic counter, atomic pin replacement, tombstone, full re-pair on loss/compromise | SEC-09, SEC-11, SEC-12; K-03 through K-12, T-16 | Rotation context and both signer inputs; counter/signer negatives | Message design passes; signatures/storage/state tests remain gated |
| Replay/downgrade | Fresh handshakes/nonces/session IDs, exact offers and selection, no weaker fallback, bounded duplicate handling | SEC-07 through SEC-09; P-03, P-04, P-06, T-03 through T-10, T-13 | Highest-common/exact-intersection checks and replay negatives | Pass for covered derivations; runtime duplicate state remains gated |
| Resource limits | Bounded canonical objects and strict pre-authentication concurrency, byte, message, and time ceilings | SEC-14, SEC-15; P-12, P-20, R-04 through R-13 | Canonical 1 MiB/32-field bound; threat model has 64 KiB/16-message pairing ceiling | Explicit implementation gate; pairing wire/TLS limits still absent |
| Vector independence | Independent derivation and cross-platform agreement | Negative matrix Golden level; ADR prerequisite 2 | Original 14 outputs stable; Ruby/OpenSSL matched all 3 additions; RFC 8032 decoder found invalid wrong-key input | Blocked by BR-03 |

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
change the ADR decision in this review. BR-03 does.

## Decision

BR-01 is closed. The BR-02 byte-exact contract and positive output are closed,
but its required wrong-key evidence is not satisfied because BR-03 uses bytes
that do not decode as an Ed25519 public key. No other new blocker was found.

ADR 0002 remains `proposed`. XT-010 must return to `blocked` until BR-03 is
resolved without weakening SEC-04, SEC-08, SEC-19, exact pinning, or
fail-closed behavior. Architecture and roadmap synchronization remains an
integration-owner action after a later independent acceptance review.
