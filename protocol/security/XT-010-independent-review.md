# XT-010 independent pairing-security review

- Reviewer: `security-review-agent`
- Date: 2026-08-06
- Decision: blocked
- ADR disposition: keep ADR 0002 `proposed`
- Review cycle: third independent review after XT-014

## Scope and method

This cumulative review independently examined ADR 0002, ADR 0004, the v1 wire
specification, the threat model, the negative-test matrix, the XT-010 handoff,
the XT-014 task and handoff, and every file under
`protocol/testdata/security/v1/`.

The reviewer is distinct from the XT-009, XT-013, and XT-014 owners recorded
as `security-vectors-agent`, `security-vector-fix-agent`, and
`ed25519-fixture-agent`. The review read the complete integrated XT-014 change
and did not treat its handoff or Python validator as independent evidence.

The evidence paths were:

1. The RFC 8032 text was fetched from the RFC Editor. An inline Ruby parser
   extracted section 7.1 TEST 1, TEST 2, and TEST 3 seeds and public keys.
   OpenSSL 3.6.3 derived each public key again from its published seed.
2. A separate inline Ruby implementation reconstructed all seven canonical
   object kinds, SHA-256, HMAC-SHA256, HKDF Expand, SAS extraction, device
   identifiers, and Edwards25519 group operations. It did not import or invoke
   `validate_vectors.py`.
3. Structured Ruby comparison against the pre-XT-014 manifest enumerated
   positive-output drift and every negative-vector error contract.
4. OpenSSL 3.6.3, Node/OpenSSL, Apple CryptoKit, the RFC 8032 verification
   equation, and the documented libsodium point policy were used to assess
   low-order and subgroup behavior.
5. The checked-in Python validator was run only as a supporting focused gate.

## Prior finding disposition

### BR-01: closed

ADR 0002 returns typed `authenticated_reject` and `affirmative_confirm`
results and permits trust only when the local and peer decisions are both
exactly `01` for the current context and role. The vectors cover both
role-specific reject HMACs, local rejection, authenticated peer rejection,
invalid decisions, and both decision-substitution directions.

The third-round independent reconstruction produced the post-XT-014 values:

| Sender | Confirm HMAC | Reject HMAC | Reject result | Trust on reject |
| ---: | --- | --- | --- | --- |
| Initiator `01` | `84d214ba8f3fbf9bd76c293482463bffd4f597572c561d631e73820bf3668068` | `9a5792fbc1c8d4640887d6539629e954124256350741f21370a48d303b7da1b5` | `authenticated_reject` | false |
| Responder `02` | `b3e50e228ecaa354334ce8e9ca7d340eb9cbeb4dbfdb5a05c5afd215cf2ec660` | `1fcbf0b570cd12766608408628a40ff86b5cfaae75fabf78e0f45f748f652185` | `authenticated_reject` | false |

The independent trust gate permitted confirm/confirm only and rejected a
local `00`, a valid peer `00`, decision `02`, and either decision substitution.
BR-01 remains closed without weakening P-09, SEC-03, or SEC-18.

### BR-02: closed

ADR 0002 defines the exact 32-byte ASCII label, complete kind-`07` envelope,
raw public-key representation, SHA-256 digest, 32-byte binary output, and
64-character lowercase hexadecimal text form. It states that the identifier
does not override possession or exact pinning.

The independent encoder produced:

```text
device_identifier_input =
584e4e53010700020000004c000100000020
586e6e5472616e7366657220646576696365206964656e746966696572207631
000200000020
d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a

device_identifier =
c503a982b3cc915bd6366c4f6e9e37b08df30a150bf061f4a91693c6f9c44c89
```

The label, canonical input, output representation, and alternate-label,
alternate-encoding, and alternate-output failures are byte-exact. BR-02 is
closed.

### BR-03: closed

XT-014 replaced the non-point `60..7f` wrong-key input with RFC 8032 TEST 2
and retained TEST 1 for the identifier:

| Role | RFC source | Public key | Device identifier |
| --- | --- | --- | --- |
| Identifier key | TEST 1 | `d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a` | `c503a982b3cc915bd6366c4f6e9e37b08df30a150bf061f4a91693c6f9c44c89` |
| Presented wrong key | TEST 2 | `3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c` | `b67b9e0fd28553c53f952dfb2c0eb9e219667ec6d8168d43335c0124c3377c89` |

OpenSSL derived both public keys from their published RFC seeds. Independent
RFC 8032 decoding and group arithmetic showed that TEST 1, TEST 2, and TEST 3
are distinct, canonical, non-identity points in the prime-order subgroup. The
wrong-key vector therefore proves the required mismatch between two valid
keys. BR-03 is closed.

## XT-014 vector evidence

The independent canonical implementation matched all 17 positive vectors,
including the unchanged `normalized-negotiation`. Comparison with the parent
of the integrated XT-014 implementation commit found exactly these 16 changed
outputs:

```text
pair-context
pairing-exporter-input
sas-five-words
confirmation-exporter-input
peer-confirmation-initiator
peer-confirmation-responder
peer-rejection-initiator
peer-rejection-responder
device-identifier-initiator
transport-context
transport-exporter-input
transport-finished-initiator
transport-finished-responder
rotation-context
rotation-proof-old-key
rotation-proof-new-key
```

All 32 pre-existing negative-vector IDs retain the same expected failure code.
The two new point cases raise the claimed codes. The complete 34-outcome
contract, grouped without omission, is:

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
| `INVALID_PUBLIC_KEY` | `device-identifier-invalid-public-key-point` |
| `TRANSPORT_FINISHED_MISMATCH` | `transport-finished-replay` |
| `REPLAY_DETECTED` | `rotation-counter-replay` |
| `INVALID_ROTATION` | `rotation-same-old-new-key` |
| `OUTPUT_MISMATCH` | `expected-output-mismatch` |

The focused fixture validator confirms the same 17 accepts and 34 rejections.
This closes XT-014's stated BR-03 remediation, but it does not resolve BR-04.

## Release-blocking findings

### BR-04: Point decoding admits keys with no unique private-key possessor

- Severity: release blocker for ADR acceptance
- Confidence: 1.00

**Requirement**

ADR 0002 requires mutual proof of identity-key possession, exact Ed25519
pinning, and a byte-exact cross-platform profile
(`docs/adr/0002-pairing-and-transport-security.md:38-51,247-251`).
SEC-04 and P-13 require possession of the exact pinned identity before trust.

**Evidence**

`validate_ed25519_public_key` performs only RFC 8032 section 5.1.3 point
decompression (`validate_vectors.py:166-213`). It does not reject the neutral
element, test membership in the prime-order subgroup, or reject mixed-order
points. The same predicate is applied to pairing, transport, rotation, and
device-ID keys (`validate_vectors.py:635-683`).

Independent Edwards25519 arithmetic produced:

| Input | Canonical point decode | `[L]P = identity` | Non-identity | `[8]P = identity` |
| --- | --- | --- | --- | --- |
| RFC TEST 1/2/3 | yes | yes | yes | no |
| identity `01 00..00` | yes | yes | no | yes |
| order-2 `ec ff..ff 7f` | yes | no | yes | yes |
| TEST 1 plus order-2 `16a567fe7d4ef5482ab4012c369bf8c5f11e8d0c2559dcda50fde59708f8aee5` | yes | no | yes | no |

The checked-in validator accepts all three hostile encodings in the last
three rows. The mixed-order row also proves that a small-order blacklist or
`[8]P != identity` check alone is insufficient.

For the identity public key `A = (0,1)`, choose `S = 1` and `R = B`. The
signature is:

```text
public_key =
0100000000000000000000000000000000000000000000000000000000000000

signature =
5866666666666666666666666666666666666666666666666666666666666666
0100000000000000000000000000000000000000000000000000000000000000
```

For every message, RFC 8032's verification equation reduces to
`[8]B = [8]B + [8]k(0,1)`. No corresponding private key is needed.
OpenSSL 3.6.3 `pkey -pubcheck` labels the identity, order-2, and order-4
encodings valid. OpenSSL `pkeyutl -verify`, Node/OpenSSL, and Apple CryptoKit
all accepted the identity signature above; Node accepted it for three
different messages. By contrast, libsodium documents that its point predicate
rejects small-order and non-main-subgroup points. The current profile
therefore permits both an authentication failure and cross-platform
accept/reject divergence.

The source-to-boundary path is concrete at the design level: a hostile LAN
peer controls its self-signed Ed25519 certificate and pairing key; the
specified canonical-point rule admits identity; affected maintained
cryptographic backends accept a forged certificate or TLS possession
signature; and the pairing flow can persist a pin that has no unique private
owner. Any party can later satisfy the same forged possession check, violating
the exact identity guarantee in SEC-04 and P-13.

This does not describe a current runtime vulnerability because pairing and TLS
are not implemented. It is a profile-definition defect that must be resolved
before the ADR can be accepted.

**Required resolution**

1. Define an accepted Ed25519 identity key as a canonical decoded point that
   is not the identity and is in the prime-order subgroup: `[L]A = identity`,
   where
   `L = 2^252 + 27742317777372353535851937790883648493`.
2. Apply the rule before any key enters TLS identity acceptance, pairing or
   transport contexts, rotation, device-ID derivation, or durable trust.
3. Add vectors for identity, another non-identity low-order point, and a
   mixed-order point. The mixed-order case must not be replaced by only a
   low-order blacklist.
4. Add macOS, Windows, and Linux TLS conformance cases proving that certificate
   import and possession verification reject these keys consistently and that
   the proved key bytes equal the transcript and pin bytes.
5. Return the amended profile and fixtures to an independent reviewer.

## Review checklist

| Area | ADR requirement | Threat/negative mapping | Independent evidence | Result |
| --- | --- | --- | --- | --- |
| Algorithms and identity keys | Ed25519 identity, X25519 ECDHE, TLS 1.3, AES-128-GCM-SHA256, optional ChaCha20-Poly1305-SHA256 | SEC-04, SEC-06, SEC-07; P-13, P-16, T-01 through T-05 | RFC/OpenSSL provenance and prime-subgroup checks for TEST 1/2/3; low-order forgery reproduced | Blocked by BR-04 |
| Labels | Exact pairing, SAS, confirmation, transport, rotation, proof, and device-ID labels | SEC-08, SEC-19; P-05, P-19, T-07 | Independent canonical reconstruction | Pass |
| Canonical encoding | `XNNS`, version/kind/count/length envelope, strict increasing fields, fixed lengths, no trailing bytes | SEC-08, SEC-14, SEC-19; P-19 | Independent reconstruction and malformed vectors | Pass for covered objects |
| TLS exporter context | Distinct pairing, confirmation, and transport labels; 32-byte context/output | SEC-03, SEC-05, SEC-06, SEC-08; P-03, T-06 | Independent contexts and outputs | Pass as fixture evidence; live TLS gate retained |
| Role separation | Ordered initiator/responder keys, nonces, transcripts, confirmation and finished role bytes | SEC-08, SEC-18; P-05, T-07 | Both role outputs and swapped-role negatives | Pass |
| SAS | HKDF Expand only, 55 MSB-first bits, fixed 2,048-word BIP39 mapping | SEC-03, SEC-18; P-01, P-11, P-16 | Independent HKDF and pinned word-list digest | Pass |
| Confirmation | Decision-bound HMAC and two-sided explicit confirmation before atomic trust | SEC-03, SEC-18; P-03, P-04, P-08 through P-10 | Confirm/reject outputs and independent trust gate | Pass; BR-01 closed |
| Device identifier | Kind-`07` over the exact pin; canonical digest and text | SEC-04, SEC-08, SEC-19; T-02 | TEST 1/2 digests and distinct-valid wrong-key reproduced | Encoding passes; accepted-key domain blocked by BR-04 |
| Transport binding | Both identity keys, fresh nonces, profile, normalized and raw negotiation, session ID, both finished values | SEC-05, SEC-08; T-06 through T-11 | Kind-`04` and both role outputs reproduced | Pass as design evidence |
| Rotation/revocation | Bound transport, old/new proof, monotonic counter, atomic pin replacement, tombstone, full re-pair on loss | SEC-09, SEC-11, SEC-12; K-03 through K-12, T-16 | Rotation context and both proof inputs reproduced | Message design passes; key domain blocked by BR-04 |
| Replay/downgrade | Fresh handshakes/nonces/session IDs, exact offers and selection, no weaker fallback | SEC-07 through SEC-09; P-03, P-04, P-06, T-03 through T-10, T-13 | Replay and downgrade negatives retained | Pass for covered derivations |
| Resource limits | Bounded canonical objects and strict pre-authentication ceilings | SEC-14, SEC-15; P-12, P-20, R-04 through R-13 | 1 MiB/32-field canonical bound and threat-model ceilings | Explicit implementation gate |
| Vector independence | Independent derivation and cross-platform agreement | Golden level; ADR prerequisite 2 | 17/17 outputs, 16 exact drifts, 34 rejection contracts, subgroup divergence | Blocked by BR-04 |

## Non-blocking implementation gates

The following remain prerequisites rather than evidence of implemented
security behavior:

- The v1 wire specification does not yet define the pairing-control state
  machine, ALPN/profile registration, certificate cap, or pairing errors.
- The canonical decoder's 1 MiB generic limit does not replace the 64 KiB and
  16-message pre-trust pairing ceiling.
- Live TLS conformance must prove exact pinning, identity-key equality, fresh
  X25519, allowed TLS suites, exporter agreement, disabled 0-RTT/resumption,
  and no fallback on all target platforms.
- Rotation vectors cover signer inputs, not signatures or atomic storage.
- Revocation, stale attempts, duplicate confirmations, storage rollback,
  cleanup races, and resource ceilings still require unit, integration,
  platform, and fuzz evidence.

These gates were already explicit and do not independently block accepting the
design. BR-04 does.

## Decision

BR-01, BR-02, and BR-03 are closed. XT-014's RFC key provenance, point
decoding for the published keys, distinct-valid wrong-key case, 16 positive
drifts, and 34 rejection outcomes are independently confirmed.

BR-04 is release-blocking because the profile's accepted Ed25519 key domain
includes identity, low-order, and mixed-order points. The identity point
admits a universal possession forgery in real maintained backends, while
stricter implementations reject it.

ADR 0002 remains `proposed`. XT-010 must return to `blocked`. Architecture and
roadmap synchronization remains an integration-owner action only after a later
independent review accepts a corrected profile.
