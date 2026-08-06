# Agent handoff: XT-001

## Delivered

- Task: XT-001, Define the LAN pairing threat model
- From owner: security-agent
- To owner or reviewer: integration owner, protocol owner, security reviewer
- Branch: `task/XT-001`
- Worktree:
  `/Users/bytedance/XnnTransfer/XnnTransfer-XT-001`
- Base SHA: `784375866646b9aa3374734ba6f02a00d11b5208`
- Head SHA: task commit containing this handoff; see final handoff response
- Worktree clean: expected and verified after the task commit
- Owned paths:
  `docs/adr/0002-pairing-and-transport-security.md`,
  `protocol/security/**`
- Observable behavior: none; this task adds design requirements and does not
  implement or enable networking, pairing, cryptography, storage, or transfer

Delivered documents:

- A proposed ADR selecting TLS 1.3, Ed25519 device identity, fresh X25519
  ECDHE, five-word 55-bit SAS first pairing, exact peer-key pinning, TLS
  exporter channel binding, and disabled v1 early data/resumption.
- A threat model covering assets, attacker capabilities, trust boundaries,
  security states, 20 release-blocking invariants, key lifecycle, failure
  closure, privacy, and concrete pre-authentication denial-of-service limits.
- A 70-case negative test matrix covering discovery, first pairing, transport,
  downgrade, replay, storage, rotation, revocation, privacy, and resource
  exhaustion.
- An implementation-prerequisite checklist requiring reviewed wire semantics,
  canonical encodings, golden vectors, cross-platform TLS conformance, secure
  storage fault injection, native API boundaries, and independent review.

## Contracts

- Added or changed:
  - Proposed security profile and trust-state requirements for future LAN
    pairing and authenticated transport.
  - Normative prerequisites for the future versioned protocol and native
    implementation.
  - No C ABI, wire encoding, or existing runtime behavior changed.
- Compatibility impact:
  - A future v1 protocol cannot claim ADR 0002 unless it binds the complete
    negotiation transcript, implements exact pinned mutual authentication,
    and satisfies the listed prerequisites and negative tests.
  - Weaker TLS, plaintext fallback, TOFU without SAS comparison, 0-RTT, and
    v1 session resumption are intentionally incompatible.
- ADR or protocol reference:
  - `docs/adr/0002-pairing-and-transport-security.md`
  - `protocol/security/threat-model.md`
  - `protocol/security/negative-test-matrix.md`

## Verification evidence

- Command: `git diff --cached --check`
  - Result: passed.
- Command: owned-path assertion over `git diff --cached --name-only`
  - Result: passed; before this handoff, all four staged files were the ADR or
    under `protocol/security/`.
- Command: local-reference, required-section, and Markdown-whitespace
  assertions
  - Result: passed.
- Command: requirement-ID assertion
  - Result: passed; 20 invariant IDs and 70 negative-test IDs are unique.
  - Note: the first helper used `Array#tally`, which this machine's Ruby does
    not provide. The same assertion was rerun with a compatible hash counter
    and passed.
- Command: `make verify`
  - Result: passed.
  - Repository layout and six-task backlog validation passed.
  - Native CMake build passed; CTest passed 1/1.
  - Flutter formatting changed 0 files; analysis found no issues; tests passed
    1/1.
- Skipped gate and reason:
  - Standalone `markdownlint` was unavailable on this machine. It is not a
    repository gate; `git diff --check` and focused structure/whitespace
    assertions passed.
  - No pairing/security implementation tests exist yet. The documents
    explicitly mark the matrix blocked until implementation.

## Residual risk

- Known limitation:
  - The wire protocol still needs canonical encodings, exact state messages,
    error values, profile identifiers, SAS word list, and golden vectors.
  - TLS library integration and secure-storage behavior are unproven on
    macOS, Windows, and Linux.
  - A platform without trusted monotonic storage may not detect restoration of
    a complete valid old secure-storage snapshot.
  - The SAS guarantee depends on the user comparing all five words. Encryption
    does not hide endpoints, timing, or volume, and bounded local work cannot
    guarantee availability under LAN saturation or distributed denial of
    service.
  - A stolen identity key permits impersonation until each peer revokes it;
    there is no central revocation or recovery service.
- Follow-up task:
  - The protocol owner must incorporate these security prerequisites into a
    reviewed versioned specification without inferring unspecified wire
    behavior.
  - Future native tasks must select and verify the TLS library, implement
    protected storage and explicit pairing APIs, add golden vectors, and
    automate the negative matrix before claiming security behavior.

## Review focus

- Files or invariants requiring close review:
  - Mandatory primitive/profile portability across all three desktop targets.
  - Completeness and canonical encoding of pairing and transport contexts.
  - Separation of exporter labels and the 55-bit SAS security/usability bound.
  - Exact-pin enforcement, monotonic security floor, and no-fallback rules.
  - Dual-key rotation, revocation ordering, identity-loss, and re-pairing.
  - Whether the concrete pre-authentication ceilings fit the future framing
    and discovery specifications.
  - Explicit distinction between design requirements and implemented behavior.

## Acceptance

- Accepted by: pending review
- Accepted at: pending review
- Follow-up runtime state: `review`
