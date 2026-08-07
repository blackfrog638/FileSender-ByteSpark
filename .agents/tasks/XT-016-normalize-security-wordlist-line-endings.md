---
id: XT-016
title: Normalize security wordlist line endings
initial_state: ready
workstream: protocol
initial_owner: unassigned
depends_on:
  - XT-015
owned_paths:
  - protocol/testdata/security/v1/**
  - native/tests/protocol/CMakeLists.txt
contract_changes: []
handoff: .agents/handoffs/XT-016.md
---

## Outcome

Make the security-profile word-list oracle produce the same pinned SHA-256 and
word indices from LF and Git-converted CRLF checkouts, while continuing to
reject malformed line endings and word-list content.

## Context

GitHub Actions run `31125643822` failed only in
`Native (windows-latest)`: Git converted `wordlist.txt` from LF to CRLF, while
`validate_vectors.py` hashed the checkout bytes directly. ADR 0002 and XT-015
define the word list as deterministic fixture evidence; this task repairs its
host-independent representation without changing security-profile outputs.

## Constraints

- Define the pinned digest over LF-normalized ASCII bytes.
- Accept a complete CRLF checkout conversion, but reject bare `CR` bytes and
  all existing invalid word-list content.
- Preserve all 17 positive outputs and 37 negative error contracts.
- Add a host-independent regression that constructs a CRLF fixture on every
  platform, so Linux and macOS CI catch future drift.
- Do not change wire bytes, security algorithms, ADR status, or runtime
  networking and transfer claims.

## Acceptance criteria

- [ ] LF and CRLF word-list copies validate to the same pinned SHA-256.
- [ ] A word list containing a bare `CR` fails with `WORDLIST_MISMATCH`.
- [ ] The security-profile README defines the canonical line-ending rule.
- [ ] All existing positive and negative vector outcomes remain unchanged.
- [ ] Focused line-ending regression and repository verification pass.

## Verification

```bash
python3 protocol/testdata/security/v1/test_wordlist_line_endings.py
python3 protocol/testdata/security/v1/validate_vectors.py
make verify
```
