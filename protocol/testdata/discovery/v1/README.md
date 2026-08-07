# XnnTransfer discovery v1 golden vectors

These fixtures are the deterministic oracle for
`protocol/spec/discovery-v1.md`.

Run:

```bash
python3 protocol/testdata/discovery/v1/validate_vectors.py
```

`vectors.json` contains:

- byte-exact 44 through 512-octet legal advertisements;
- withdrawal, optional-label, Unicode, and ignored noncritical-TLV cases;
- truncated, OS-truncated, oversized, source-metadata, version, length, flag,
  sequence, token, TTL, TLV, and label rejection cases;
- duplicate, stale, conflicting, and newer-sequence cache transitions;
- monotonic expiry and bounded tombstone calculations;
- withdrawal reordering, source-key separation, self-filtering, interface
  removal, and wake behavior; and
- deterministic per-source rate, rate-bucket-table, and per-interface
  candidate-limit scenarios.

Large datagrams may use `prefix_hex`, `repeat_hex`, `repeat_count`, and
`suffix_hex`. The validator expands those fields into one exact octet string
before parsing.

The oracle uses only the Python standard library and performs no socket,
filesystem, TLS, identity, pairing, or transfer operation. All source
addresses, tokens, labels, and times are synthetic. An accepted fixture means
only that an untrusted candidate would satisfy discovery syntax and local
cache policy. It does not prove reachability, identity, compatibility, trust,
or user intent.
