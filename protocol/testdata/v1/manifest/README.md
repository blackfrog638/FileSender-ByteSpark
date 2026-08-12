# XnnTransfer v1 manifest vectors

`vectors.json` is the deterministic protocol-object corpus for the v1
`TRANSFER_OFFER`, `MANIFEST_ENTRY`, and `MANIFEST_END` rules. It contains
legal boundaries and hostile relative paths, entry combinations, ordering,
collisions, and summaries.

Run the standard-library validator from the repository root:

```bash
python3 protocol/testdata/v1/manifest/validate_vectors.py
```

The validator reads only the fixture JSON. It does not call host path APIs,
inspect a destination, resolve links, create files, or claim storage safety.
It is an oracle for rejection before filesystem access, not a production wire
parser or storage implementation.

## Format

- `format_version` versions this fixture format, not the wire protocol.
- `defaults` supplies repeated fixture-only transfer IDs and commitments.
- `comparison_profiles` makes destination comparison behavior explicit rather
  than deriving it from the validator host.
- `manifest.offer`, `manifest.entries`, and `manifest.end` expand to protocol
  objects. Omitted transfer IDs and offer/end commitments use `defaults`.
- `relative_path_hex` represents hostile raw UTF-8 bytes.
- `relative_path_repeat` is compact fixture syntax for a repeated text
  component.
- `generated_entries.fixed_path_series` deterministically expands large exact
  boundary cases without storing 100,000 entries in JSON.
- `expect.error` is the v1 protocol error category. `expect.reason` is a stable
  fixture reason used to identify the rejected invariant.

Every generated entry has a contiguous index and a unique ASCII path of the
requested encoded length. Generation occurs in memory before normal protocol
validation; generated objects receive no relaxed limits.

## Comparison profiles

Wire paths must already be NFC. A non-NFC path is rejected before duplicate or
ancestor comparison. The corpus deliberately applies NFC, not compatibility
normalization.

`posix_case_sensitive_nfc` models a case-sensitive POSIX destination.
`windows_ascii_case_insensitive_nfc` models the portable Windows boundary
exercised by these vectors: ASCII case variants collide. The latter is not a
complete replacement for the destination's native Unicode comparison or path
policy. Production code must still use its destination policy as required by
`protocol/spec/v1.md`.

The profiles are fixture inputs, not wire fields or negotiated capabilities.
They keep the same vector result stable on macOS, Linux, and Windows without
asserting that every filesystem on one operating system has identical
comparison behavior.

## Covered boundaries

Legal vectors include files, directories, empty files, nested paths, 32
components, 255-byte components, 1,024-byte paths, 100,000 entries,
33,554,432 aggregate path bytes, 16 TiB file totals, and 64-byte commitments.

Hostile vectors include traversal, POSIX absolute paths, drive and UNC forms,
backslash separators, alternate data streams, invalid UTF-8, controls,
noncharacters, non-NFC text, Windows reserved device aliases, components with
trailing dots or spaces, duplicate and case-colliding paths, file ancestor
conflicts, unrepresentable special-file kinds, invalid file/directory
combinations, noncontiguous indexes, checked aggregate-limit failures, and
inconsistent offer/end summaries.
