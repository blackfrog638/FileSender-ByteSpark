# ADR 0018: Portable Windows Path Aliases

- Status: accepted
- Date: 2026-08-12
- Requester: project owner
- Decision makers: project owner, integration owner
- Scope: v1 manifest compatibility and native receive-path validation
- Acceptance basis: the project owner selected Windows reserved aliases as
  the explicit Harness V2 code pilot boundary on 2026-08-12

## Context

The v1 manifest path contract already excludes absolute paths, drive-qualified
paths, alternate data streams, traversal, controls, noncharacters, and
non-NFC text. It did not exclude names that Windows resolves as device aliases
or components that Windows normalizes by removing a trailing dot or space.

Consequently, a manifest containing `CON`, `nul.txt`, or `docs/name.` could
pass the shared validator. A POSIX receiver could persist it while a Windows
receiver would reject it or address a different namespace. Deferring the
decision to filesystem creation makes the same wire manifest platform
dependent and moves rejection past the untrusted-input boundary.

## Decision

Every v1 relative-path component is portable before filesystem access:

- A component ending in an ASCII dot or space is invalid.
- The ASCII-case-insensitive stem before the first dot must not be `CON`,
  `PRN`, `AUX`, `NUL`, `COM1` through `COM9`, or `LPT1` through `LPT9`.
- Adding an extension does not make a reserved stem valid.
- Interior dots and spaces remain valid.
- Similar non-reserved names such as `console.txt`, `com0`, and `com10`
  remain valid.

The shared native validator and the host-independent manifest oracle implement
the same rule and report the existing reserved-component error category. The
rule applies on every receiver platform; it is not conditional on the local
filesystem.

## Compatibility

This deliberately tightens v1 manifest acceptance without changing frame
encoding or negotiation. A sender that previously emitted one of these names
now receives `INVALID_MANIFEST`; conforming senders must rename or omit that
entry. Older receivers may still accept such a manifest, so senders cannot use
that behavior as evidence of portability.

No new wire field or capability bit is introduced. The change restores the v1
requirement that path policy be verified before acceptance and makes one
portable minimum explicit.

## Consequences

- Linux and macOS reject names that their local filesystems could otherwise
  store.
- Windows namespace aliases cannot reach backend path creation.
- Manifest vectors cover exact aliases, mixed-case aliases with extensions,
  and trailing dot/space components.
- Future additions to the portable-name denylist require another reviewed
  compatibility decision; the validator must not silently inherit a
  host-specific denylist.
