# XT-010 independent pairing-security review

- Reviewer: `security-review-agent`
- Date: 2026-08-07
- Decision: accept the design
- ADR disposition: change ADR 0002 to `accepted`
- Review cycle: fourth independent review after integrated XT-015
- Acceptance scope: design only; no runtime or production-security claim

## Scope and method

This cumulative review examined ADR 0002, ADR 0004, the complete v1 wire
specification, the XT-001 threat model and negative-test matrix, all prior
XT-010 findings, the integrated XT-015 change, and every security-profile
fixture file. The reviewer is independent from the XT-009, XT-013, XT-014,
and XT-015 authors.

The fourth review did not treat `validate_vectors.py` or the XT-015 handoff as
independent evidence. Its primary evidence was:

1. A separate inline Python implementation of RFC 8032 compressed-point
   decoding, affine Edwards25519 addition, affine double-and-add scalar
   multiplication, and the direct and cofactored Ed25519 verification
   equations. It did not import or invoke the fixture validator.
2. OpenSSL 3.6.3 derivation of RFC 8032 TEST 1, TEST 2, and TEST 3 public keys
   from the published seeds.
3. A structured comparison of the pre-XT-015 and current manifests for every
   positive output and negative error contract.
4. Static call-path review proving that the validator applies the accepted-key
   predicate before pairing, transport, rotation, and device-ID key use.
5. The checked-in validator and repository verification only as supporting
   gates after the independent checks.

## Finding disposition

| Finding | Disposition | Confidence |
| --- | --- | ---: |
| BR-01: authenticated rejection could be confused with confirmation | Closed; typed reject/confirm results and the two-decision trust gate fail closed | 1.00 |
| BR-02: device identifier was not byte-exact | Closed; label, kind-`07` input, digest, text form, and exact-pin limitation are normative | 1.00 |
| BR-03: wrong-key fixture did not prove mismatch between valid keys | Closed; RFC TEST 1 and TEST 2 are independently derived valid subgroup keys | 1.00 |
| BR-04: identity, low-order, and mixed-order keys entered the accepted domain | Closed by the normative non-identity prime-subgroup predicate and pre-use enforcement | 1.00 |
| New release-blocking design finding | None found across byte encoding, roles, state, fallback, and key lifecycle | 0.96 |

### BR-01: closed

ADR 0002 returns typed `authenticated_reject` and `affirmative_confirm`
results and permits trust only when the local and peer decisions are both
exactly `01` for the same live context and role. The fixtures cover both
role-specific reject HMACs, local rejection, authenticated peer rejection,
invalid decisions, and both decision-substitution directions.

| Sender | Confirm HMAC | Reject HMAC | Reject result | Trust on reject |
| ---: | --- | --- | --- | --- |
| Initiator `01` | `84d214ba8f3fbf9bd76c293482463bffd4f597572c561d631e73820bf3668068` | `9a5792fbc1c8d4640887d6539629e954124256350741f21370a48d303b7da1b5` | `authenticated_reject` | false |
| Responder `02` | `b3e50e228ecaa354334ce8e9ca7d340eb9cbeb4dbfdb5a05c5afd215cf2ec660` | `1fcbf0b570cd12766608408628a40ff86b5cfaae75fabf78e0f45f748f652185` | `authenticated_reject` | false |

### BR-02: closed

The device identifier uses the exact 32-byte ASCII label, complete kind-`07`
envelope, raw accepted public-key bytes, SHA-256 digest, 32-byte binary form,
and 64-character lowercase hexadecimal form. It does not override possession
or exact pinning.

```text
device_identifier_input =
584e4e53010700020000004c000100000020
586e6e5472616e7366657220646576696365206964656e746966696572207631
000200000020
d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a

device_identifier =
c503a982b3cc915bd6366c4f6e9e37b08df30a150bf061f4a91693c6f9c44c89
```

### BR-03: closed

OpenSSL 3.6.3 independently derived the checked-in public keys from the
published RFC 8032 seeds:

| Role | RFC source | Public key | Seed derivation |
| --- | --- | --- | --- |
| Identifier/initiator/rotation-old | TEST 1 | `d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a` | exact match |
| Responder/device-ID wrong-key | TEST 2 | `3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c` | exact match |
| Rotation-new | TEST 3 | `fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025` | exact match |

The independent group calculation also proves that all three are distinct,
canonical, non-identity points in the prime-order subgroup. The wrong-key case
therefore compares two valid accepted keys rather than a valid key with an
invalid byte string.

### BR-04: closed

XT-015 adds the exact rule required by the third review: decode canonically,
reject the identity, and require `[L]P = identity` for
`L = 2^252 + 27742317777372353535851937790883648493`. ADR 0002 rejects point
decompression alone, small-order blacklists, and cofactor-only checks.

The independent affine implementation produced:

| Input | Canonical round trip | `P != identity` | `[L]P = identity` | `[8]P = identity` |
| --- | --- | --- | --- | --- |
| RFC TEST 1 | yes | yes | yes | no |
| RFC TEST 2 | yes | yes | yes | no |
| RFC TEST 3 | yes | yes | yes | no |
| identity `01 00..00` | yes | no | yes | yes |
| order-2 `ec ff..ff 7f` | yes | yes | no | yes |
| mixed-order `16a567fe...08f8aee5` | yes | yes | no | no |

The order-2 point satisfies `[2]P = identity`. The mixed-order point is exactly
RFC TEST 1 plus the order-2 point and has `[8]P != identity`; it therefore
proves that neither a low-order blacklist nor only `[8]P != identity` is an
adequate substitute for the `[L]P` membership test.

The identity-forgery bytes were reconstructed exactly:

```text
public_key =
0100000000000000000000000000000000000000000000000000000000000000

signature =
5866666666666666666666666666666666666666666666666666666666666666
0100000000000000000000000000000000000000000000000000000000000000
```

For the empty message, ASCII `XnnTransfer`, and the 32-byte sequence
`00..1f`, both `[S]B = R + [k]A` and
`[8][S]B = [8]R + [8][k]A` evaluated true with `A = identity`, `R = B`, and
`S = 1`. The accepted-key predicate now rejects `A` before a backend can turn
that equation into possession evidence.

## Pre-use enforcement

The validator's control flow applies the same predicate before each covered
key use:

| Use | Enforcement path | Result |
| --- | --- | --- |
| Pairing context | `fixture_public_key` and kind-`02` validation call `validate_ed25519_public_key` before encoding or `canonical_digest` | Pass |
| Transport context | `fixture_public_key` and kind-`04` validation check both identity keys before context hashing | Pass |
| Rotation | `build_rotation_context` validates old/new keys; kind-`05` parsing repeats the checks before the proof context is hashed | Pass |
| Device identifier | `build_device_identifier` validates before encoding and SHA-256; verification validates both compared keys first | Pass |
| TLS identity and durable trust | ADR 0002 requires the predicate before identity acceptance, possession verification, pinning, and trust writes | Design pass; live platform gate retained |

The exact accepted 32 octets remain the bytes used for possession
verification, transcripts, pinning, and device-ID derivation. There is no
alternate key decoding or backend-specific fallback.

## Vector contract

Comparison against the parent of integrated XT-015 commit `2033437` found:

- 17 pre-existing positive vector IDs and outputs are byte-for-byte unchanged.
- All 34 pre-existing negative vector IDs retain their exact error code.
- Exactly three new negative IDs exist, and each rejects as
  `INVALID_PUBLIC_KEY`.

The complete current rejection contract is:

| Failure code | Vector IDs |
| --- | --- |
| `MALFORMED_ENCODING` | `malformed-bad-magic`, `malformed-truncated-header` |
| `TRAILING_DATA` | `malformed-trailing-data` |
| `DUPLICATE_FIELD` | `malformed-duplicate-field` |
| `NON_CANONICAL_ENCODING` | `malformed-out-of-order-field`, `device-identifier-noncanonical-public-key-point`, `device-identifier-alternate-output-encoding` |
| `MISSING_FIELD` | `pair-context-omitted-negotiation`, `transport-context-omitted-raw-transcript` |
| `INVALID_LENGTH` | `pair-context-short-nonce`, `transport-context-short-session-id`, `sas-short-exporter`, `device-identifier-alternate-public-key-encoding` |
| `ROLE_MISMATCH` | `pair-context-swapped-roles`, `confirmation-swapped-role`, `transport-finished-swapped-role`, `rotation-invalid-signer` |
| `CONTEXT_MISMATCH` | `pair-context-swapped-keys` |
| `DOWNGRADE_DETECTED` | `negotiation-downgrade` |
| `MALFORMED_TRANSCRIPT` | `transport-context-malformed-raw-order` |
| `CONFIRMATION_MISMATCH` | `confirmation-replay-fresh-attempt`, `confirmation-substitute-reject-for-confirm`, `confirmation-substitute-confirm-for-reject` |
| `AUTHENTICATED_REJECT` | `confirmation-initiator-reject-where-confirm-required`, `confirmation-responder-reject-where-confirm-required` |
| `LOCAL_REJECT` | `confirmation-local-reject-where-trust-required` |
| `INVALID_DECISION` | `confirmation-invalid-decision` |
| `DOMAIN_MISMATCH` | `device-identifier-alternate-label` |
| `DEVICE_IDENTIFIER_MISMATCH` | `device-identifier-wrong-key` |
| `INVALID_PUBLIC_KEY` | `device-identifier-invalid-public-key-point`, `ed25519-identity-public-key`, `ed25519-order-2-public-key`, `ed25519-mixed-order-public-key` |
| `TRANSPORT_FINISHED_MISMATCH` | `transport-finished-replay` |
| `REPLAY_DETECTED` | `rotation-counter-replay` |
| `INVALID_ROTATION` | `rotation-same-old-new-key` |
| `OUTPUT_MISMATCH` | `expected-output-mismatch` |

The focused validator confirms the same 17 accepts and 37 rejections as a
supporting gate.

## Review checklist

| Area | ADR/threat mapping | Fourth-review evidence | Result |
| --- | --- | --- | --- |
| Algorithms and accepted key domain | SEC-04, SEC-06, SEC-07; P-13, P-16, T-01 through T-05 | RFC/OpenSSL provenance; independent affine `[L]P`; identity forgery | Pass; BR-04 closed |
| Labels and domain separation | SEC-08, SEC-19; P-05, P-19, T-07 | Exact pairing, SAS, confirmation, transport, rotation, proof, and device-ID labels | Pass |
| Canonical encoding | SEC-08, SEC-14, SEC-19; P-19 | `XNNS` envelope, strict field order/lengths, no alternate decoding | Pass for covered objects |
| TLS exporter inputs | SEC-03, SEC-05, SEC-06, SEC-08; P-03, T-06 | Distinct labels and exact 32-byte contexts/outputs | Design pass; live TLS gate retained |
| Roles and state ordering | SEC-03, SEC-05, SEC-08, SEC-18; P-05, P-08 through P-10, T-07 | Ordered role bytes, both role outputs, typed reject, same-attempt trust gate | Pass; BR-01 closed |
| SAS | SEC-03, SEC-18; P-01, P-11, P-16 | HKDF Expand, 55 MSB-first bits, pinned 2,048-word mapping | Pass |
| Device identifier | SEC-04, SEC-08, SEC-19; T-02 | Exact kind-`07` digest, accepted-key predicate, distinct valid wrong key | Pass; BR-02/03 closed |
| Transport binding | SEC-05, SEC-08; T-06 through T-11 | Both keys/nonces, profile, normalized/raw negotiation, session ID, both finished values | Design pass |
| Rotation/revocation | SEC-09, SEC-11, SEC-12; K-03 through K-12, T-16 | Bound dual-key proof inputs, counter, nonce, key validation, atomic/tombstone rules | Design pass; storage gate retained |
| Replay/downgrade/fallback | SEC-07 through SEC-09; P-03, P-04, P-06, T-03 through T-10, T-13 | Fresh contexts, exact offers/selection, no plaintext/TLS 1.2/TOFU retry | Pass |
| Key lifecycle | SEC-10 through SEC-13; K-01 through K-14 | Protected non-sync storage, no plaintext fallback, explicit reset/revoke/re-pair | Design pass; platform gate retained |
| Resource limits and failure closure | SEC-14, SEC-15, SEC-20; P-12, P-20, R-04 through R-13 | Bounded canonical objects and explicit pre-trust ceilings/cleanup | Design pass; implementation gate retained |
| Vector independence | ADR prerequisite 2 | 17 unchanged outputs, 34 stable errors, 3 new subgroup rejects | Pass |

No new byte-exact, role, state, downgrade/fallback, or key-lifecycle ambiguity
survived this review.

## Remaining implementation gates

Acceptance does not satisfy or weaken these prerequisites:

- The v1 specification still lacks pairing-control messages/state,
  ALPN/profile registration, a concrete certificate cap, pairing error values,
  and complete duplicate handling for the pairing ceremony.
- Manifest/file/chunk/prefix commitments and peer-bound resume authorization
  remain unspecified security-profile contracts; production transfer
  conformance remains blocked until they are defined and reviewed.
- The generic 1 MiB canonical-object limit does not replace the 64 KiB and
  16-message pre-trust pairing ceilings.
- macOS, Windows, and Linux TLS tests must prove subgroup prevalidation, exact
  pin/possession/transcript byte equality, fresh X25519, suite enforcement,
  exporter agreement, disabled 0-RTT/resumption, and no weaker fallback.
- Rotation fixtures cover signer inputs, not signatures or atomic durable
  replacement. Secure-storage atomicity, corruption, rollback, identity-loss,
  non-synchronization, and cleanup races still need platform fault tests.
- Every applicable negative-matrix row still needs unit, integration,
  platform, or fuzz evidence. Discovery, networking, pairing, TLS, secure
  storage, and transfer behavior remain unimplemented.

These are explicit conformance and implementation gates, not unresolved
ambiguities in the accepted pairing and bound-transport design.

## Decision

BR-01, BR-02, BR-03, and BR-04 are closed. The fourth review found no remaining
design blocker, so ADR 0002 is accepted as a security design.

This decision does not claim implemented networking, cryptography, pairing,
TLS, storage, or transfer security. Architecture and roadmap wording still
describe ADR 0002 as proposed; those integration-owned shared documents must
be synchronized by the integration owner when this task is integrated.
