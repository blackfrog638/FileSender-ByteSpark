# XnnTransfer v1 security-profile golden vectors

This directory contains deterministic test evidence for the security profile
proposed by ADR 0002. It does not implement TLS, pairing, identity storage,
Ed25519 signing, or a production protocol parser.

## Contents

- `vectors.json`: versioned, byte-exact positive and negative vectors.
- `pairing-control-vectors.json`: production ALPN/profile registration,
  canonical `XNNP` frames, pairing state, and hostile-input vectors.
- `validate_vectors.py`: Python standard-library fixture oracle.
- `wordlist.txt`: the 2,048-entry BIP39 English list in index order.

The manifest records the input encoding, expected output or stable failure, and
security invariants for every vector. TLS exporter bytes are explicit fixture
inputs. They are not derived from a live TLS connection. Pairing-control
vectors bind those existing cryptographic fixtures to the wire contract in
`protocol/spec/pairing-v1.md`; they do not instantiate a TLS provider or
protected store.

## Validation

Run from the repository root:

```bash
python3 protocol/testdata/security/v1/validate_vectors.py
```

The command is deterministic, performs no network or filesystem writes, and
does not depend on host byte order, locale, Unicode normalization, or
third-party packages. With no manifest argument it validates both
`vectors.json` and `pairing-control-vectors.json`. Passing an explicit manifest
validates only that manifest.

## Ed25519 fixture keys

The identity and rotation public keys are published Ed25519 keys from
[RFC 8032 section 7.1](https://www.rfc-editor.org/rfc/rfc8032#section-7.1):

- initiator and rotation-old: TEST 1 public key
  `d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a`;
- responder and device-ID wrong-key: TEST 2 public key
  `3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c`;
- rotation-new: TEST 3 public key
  `fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025`.

The validator implements RFC 8032 section 5.1.3 compressed-point decoding
using only Python integer arithmetic. It requires `y < 2^255 - 19`, recovers
`x` from `(y^2 - 1) / (d*y^2 + 1)`, rejects a missing square root, and rejects
the noncanonical `x = 0` encoding with the sign bit set. It then performs
Edwards25519 scalar multiplication in extended coordinates and accepts only
when the decoded point `P` is not the identity and `[L]P` is the identity,
where
`L = 2^252 + 27742317777372353535851937790883648493`. This is a real
prime-order subgroup test, not a small-order blacklist or a cofactor-only
check. An independent reviewer can compare the three keys with the RFC and
apply the same decoding and subgroup rules without importing this fixture
oracle.

## XT-015 subgroup and forgery evidence

The hostile encodings from XT-010 finding BR-04 distinguish the required
prime-subgroup predicate from weaker checks:

| Input | Non-identity | `[L]P = identity` | Expected result |
| --- | --- | --- | --- |
| RFC 8032 TEST 1/2/3 | yes | yes | accept with all existing outputs unchanged |
| identity `01 00..00` | no | yes | `INVALID_PUBLIC_KEY` |
| order-2 `ec ff..ff 7f` | yes | no | `INVALID_PUBLIC_KEY` |
| TEST 1 plus order-2 `16a567fe...08f8aee5` | yes | no | `INVALID_PUBLIC_KEY` |

The mixed-order point has `[8]P != identity`, so it is not stopped by a
small-order blacklist or by requiring only `[8]P != identity`.

BR-04 also recorded a universal identity-key forgery. For public key
`A = (0,1)`, `R = B`, and `S = 1`, the following signature satisfies the
cofactored verification equation for every message without a corresponding
private key:

```text
public_key =
0100000000000000000000000000000000000000000000000000000000000000

signature =
5866666666666666666666666666666666666666666666666666666666666666
0100000000000000000000000000000000000000000000000000000000000000

[8]B = [8]B + [8]k(0,1)
```

The independent review observed the following backend divergence:

| Backend | Recorded behavior |
| --- | --- |
| OpenSSL 3.6.3 | imported identity, order-2, and order-4 keys; accepted the identity signature |
| Node/OpenSSL | accepted the identity signature for three different messages |
| Apple CryptoKit | accepted the identity signature |
| libsodium | rejects small-order and non-main-subgroup points |

`vectors.json` pins these exact bytes and observations under
`ed25519_subgroup_evidence`. They are review evidence motivating a
backend-independent pre-use subgroup check, not live TLS conformance evidence
or a claim that production pairing exists.

## XT-014 golden drift

Replacing the unsourced identity bytes with the published keys necessarily
changes every key-bound context and its downstream derivations. The principal
digest changes are:

| Value | Previous | XT-014 |
| --- | --- | --- |
| Pair context | `14faa5b3b80123935e5627f96afea5ea0cc4f527f727af62b615174c67f70882` | `f8ccd258387e2b0347934c4a909055865db87a95e182b0372de01d0fb1c0fa50` |
| Initiator device ID | `c0a74fca4f0e4dd1fc6d7c7ad7e8478b6096d35ec8fab8566234abfd4f5c00f3` | `c503a982b3cc915bd6366c4f6e9e37b08df30a150bf061f4a91693c6f9c44c89` |
| Responder wrong-key device ID | invalid key input | `b67b9e0fd28553c53f952dfb2c0eb9e219667ec6d8168d43335c0124c3377c89` |
| Transport context | `deaf1569d495b3182b41fa24d874bb64254c9c99b76b1b258510fab69ffa4b43` | `7fcb0313518a55ff34783e3ec2984fb28b1de235f9e7cccb6746c79dee88ed51` |
| Rotation context | `92491f75fa6855de192b5fd110481b54381c351bfe9a0647a9848624466d9ac7` | `28a0cb64af46756fe2828eebf3859edfca0585abfd352a7cebb62f754ad03acb` |

`normalized-negotiation` is the only positive output that remains byte-exact.
The other 16 positive outputs were recomputed, including pair/SAS/confirmation,
device-ID, transport/finished, and rotation/proof values. Negative vectors
containing those bytes were recomputed while preserving their existing failure
codes. Two additional negatives distinguish a non-decodable point from a
noncanonical `y >= p` encoding.

This is fixture-evidence drift only. It does not change algorithms, labels,
role ordering, confirmation/rejection semantics, or the proposed status of
ADR 0002.

## Coverage

Positive vectors cover:

- normalized role-ordered negotiation and pairing context construction;
- pairing, confirmation, and transport exporter API inputs;
- 55-bit SAS extraction and five fixed word-list indices;
- initiator and responder peer-confirmation HMAC values;
- initiator and responder authenticated-rejection HMAC values with typed,
  terminal, no-trust-write outcomes;
- byte-exact device-identifier input, label, digest, and text representation;
- transport context and both role-specific finished values;
- rotation context plus old-key and new-key Ed25519 proof input bytes.

Negative vectors cover malformed canonical encoding, duplicate and omitted
fields, role and identity-key swaps, replay on fresh pairing and transport
contexts, authenticated rejection where confirmation is required, invalid and
substituted confirmation decisions, device-identifier label/key/output
alternates and wrong-key verification, negotiation downgrade, malformed raw
transcript order, wrong fixed lengths, stale rotation counters, invalid
rotation domains, invalid and noncanonical Ed25519 points, identity and
non-identity low-order keys, a mixed-order key, and output mismatch.

Pairing-control positives cover both confirmation arrival orders, atomic local
commit success, authenticated rejection, and an established reconnect under
the exact active pin. Hostile controls cover commit failure, cancellation
before commit invocation, duplicate decisions, bad magic, duplicate sequence
and semantic messages, replay on a fresh nonce, role swap, oversized
declaration, unknown profile and ALPN, downgrade, duplicate/missing/reordered/
unknown fields, trailing data, a reconnect pin mismatch, a stored-floor
downgrade, and invalid transport-finished input. The validator reconstructs ADR
0002 kind-`01` and kind-`02` objects from accepted control frames, requires the
existing normative `pair_context` digest, and checks the established kind-`04`
transport context and both finished HMACs. This prevents the registered wire
profile from silently diverging from the accepted cryptographic vectors.

## Word list

`wordlist.txt` is the English word list from BIP 39, used only as a fixed
zero-based mapping from an 11-bit index to an ASCII display word. XnnTransfer
does not use BIP39 mnemonic checksums, seed derivation, or wallet semantics.
The SHA-256 of the canonical LF-delimited ASCII bytes is pinned in
`vectors.json` and checked before any vector is evaluated. A Git checkout that
converts every LF to CRLF is normalized back to LF before hashing and parsing;
a bare carriage return is invalid. This keeps the logical word-list fixture
byte-exact without making its digest depend on the host checkout policy.

Source:
`https://github.com/bitcoin/bips/blob/master/bip-0039/english.txt`
