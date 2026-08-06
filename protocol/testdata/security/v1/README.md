# XnnTransfer v1 security-profile golden vectors

This directory contains deterministic test evidence for the security profile
proposed by ADR 0002. It does not implement TLS, pairing, identity storage,
Ed25519 signing, or a production protocol parser.

## Contents

- `vectors.json`: versioned, byte-exact positive and negative vectors.
- `validate_vectors.py`: Python standard-library fixture oracle.
- `wordlist.txt`: the 2,048-entry BIP39 English list in index order.

The manifest records the input encoding, expected output or stable failure, and
security invariants for every vector. TLS exporter bytes are explicit fixture
inputs. They are not derived from a live TLS connection.

## Validation

Run from the repository root:

```bash
python3 protocol/testdata/security/v1/validate_vectors.py
```

The command is deterministic, performs no network or filesystem writes, and
does not depend on host byte order, locale, Unicode normalization, or
third-party packages.

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
rotation domains, and output mismatch.

## Word list

`wordlist.txt` is the English word list from BIP 39, used only as a fixed
zero-based mapping from an 11-bit index to an ASCII display word. XnnTransfer
does not use BIP39 mnemonic checksums, seed derivation, or wallet semantics.
The exact file SHA-256 is pinned in `vectors.json` and checked before any vector
is evaluated.

Source:
`https://github.com/bitcoin/bips/blob/master/bip-0039/english.txt`
