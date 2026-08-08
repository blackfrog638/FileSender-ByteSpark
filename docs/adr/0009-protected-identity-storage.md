# ADR 0009: Protected identity and pairing-record storage

- Status: proposed
- Date: 2026-08-08
- Owner task: XT-022
- Security profile: `docs/adr/0002-pairing-and-transport-security.md`

## Context

The device identity private key and pairing records are durable
security-sensitive state. Their encoding, atomicity, rollback behavior,
identity-loss behavior, secret lifetime, and platform protection must be
explicit before pairing can be implemented.

ADR 0006 selects platform facilities but does not provide implementations.
Windows Credential Manager limits a generic credential blob to approximately
2560 bytes. A complete 256-peer snapshot therefore cannot be one protected
item, even if the application permits a much larger logical repository.

This decision separates the platform-independent record and transaction
contract from unresolved platform adapters. Nothing in this ADR claims that
Keychain, Credential Manager, or a qualified Secret Service is implemented or
supported.

## Decision

### Protected-store boundary

The repository uses one root item and one opaque item per peer. Every item
payload is at most 2048 octets. The fixed root item identifier is `root`; a peer
item identifier is:

```text
peer/<lowercase-hex-store-id>/<lowercase-hex-record-id>
```

The store ID is 16 random octets. The record ID is the 32-octet device ID at
the original pairing commit and remains stable across rotations. Identifiers
contain no private material.

The injected `ProtectedStore` provides:

- enumeration of `(item_id, revision)` metadata;
- keyed get returning no item or one `(revision, opaque payload)` pair;
- compare-and-swap put against an expected absent/present revision;
- compare-and-swap delete against an expected present revision.

A new item starts at revision one. Replacement revision is exactly one greater
than the expected revision. A CAS mismatch is `revision_conflict` and has no
observable write. The core does not emulate this contract with process-local
locks. Adapters must reject identifiers over 128 octets and payloads over 2048
octets.

Root revision changes only when the identity generation changes. Each peer
item has its own revision. Commit, rotation, revocation, and forget therefore
touch only one peer item. The root store ID is the repository generation
commit point: peer items from another store ID are unreachable state and are
never loaded as trust. A root may also retain exactly one prior store ID as the
only generation eligible for cleanup.

### Canonical encoding

All integers are unsigned big-endian. Records use this envelope:

| Field | Encoding |
| --- | --- |
| `magic` | four ASCII octets `XNNI` |
| `canonical_version` | `U8`, exactly `01` |
| `record_kind` | `U8` |
| `field_count` | `U16` |
| `body_length` | `U32` |
| fields | exactly `field_count` field records |

A field is `field_id:U16 || value_length:U32 || value`. Field IDs are strictly
increasing. Unknown, duplicate, required-but-omitted, out-of-order, truncated,
oversized, or trailing data is invalid. No host layout, locale, or text
normalization participates in encoding.

Kind `01`, root item:

| ID | Value |
| ---: | --- |
| 1 | 32-octet Ed25519 seed |
| 2 | canonical 32-octet Ed25519 public key |
| 3 | 32-octet ADR 0002 device identifier |
| 4 | fresh nonzero 16-octet store ID |
| 5 | nonzero root item revision, `U64` |
| 6 | optional nonzero 16-octet retired store ID, distinct from field 4 |

On every load, the public key is re-derived from the seed and the device
identifier is re-derived from that public key. The encoded revision must equal
the protected item's revision. A mismatch is corruption or rollback. A
syntactically recognizable root with no seed is `identity_loss`, never an
instruction to generate a replacement key. Field 6 is absent before the first
reset and otherwise names the sole prior generation that cleanup may delete.

Kind `02`, authenticated peer item:

| ID | Value |
| ---: | --- |
| 1 | owning root store ID, 16 octets |
| 2 | immutable peer record ID, 32 octets |
| 3 | owning root device identifier, 32 octets |
| 4 | current peer Ed25519 public key, 32 octets |
| 5 | current peer device identifier, 32 octets |
| 6 | minimum approved security profile, nonzero `U16` |
| 7 | trust state: active `01`, revoked `02` |
| 8 | monotonic rotation counter, `U64` |
| 9 | peer item revision, nonzero `U64` |
| 10 | local display label, 0..96 octets of strict UTF-8 |
| 11 | `count:U8 || count * old_public_key:32`, at most eight |
| 12 | HMAC-SHA256 record authenticator, 32 octets |

The maximum v1 root is 184 octets and the maximum v1 peer is 632 octets,
including envelope and field headers. Both remain below the 2048-octet
platform-independent cap.

The payload store ID and record ID must exactly match the root and item
identifier. The root device ID is also authenticated. Copying a valid peer
payload to another item, root, or reset generation fails closed.

Kind `03` is never persisted independently. It contains peer fields 1 through
11 and is the canonical peer MAC input.

No migration is defined for canonical version 1. An unknown version fails with
`unsupported_schema`; it is not rewritten. Future migration requires a new
decision defining source versions, crash behavior, and downgrade handling.

### Peer-key validation

`IdentityRepository` requires an injected `PeerPublicKeyValidator`. Success
means the exact 32 octets canonically decode under RFC 8032 section 5.1.3, are
not the Edwards25519 identity, and satisfy `[L]P = identity` for the prime
subgroup order from ADR 0002.

Commit and rotation invoke the validator before device-ID derivation, MAC
calculation, or protected-store access. Invalid input is `invalid_argument`.
Load revalidates the current key and every rotation tombstone; a persisted
invalid key is `corrupt_record`. A small-order blacklist, decompression-only
check, or cofactor-only check does not satisfy the injected contract.

This task defines and enforces the validator boundary but does not claim a
production pairing or TLS provider. A caller that cannot inject a conforming
validator must keep pairing disabled.

### Record-MAC key derivation

The Ed25519 seed is never used directly as an HMAC key. For each MAC operation,
the repository derives a 32-octet key with HKDF-SHA256:

- input key material: the exact 32-octet root seed;
- salt: exact ASCII `XnnTransfer identity root HKDF salt v1`;
- info: exact ASCII `XnnTransfer peer record MAC key v1` followed by the
  16-octet store ID.

The authenticator is HMAC-SHA256 under that derived key over exact ASCII
`XnnTransfer peer record MAC v1` followed by the canonical kind-`03` record.
The derived key is held in a move-only `SecretBuffer`, explicitly cleansed
after HMAC, and also cleansed on every error or destructor path.

The HMAC is internal consistency and root-binding protection. It does not
replace the platform store's confidentiality and integrity guarantees.

### Repository state transitions

An absent root plus an empty enumeration is an empty repository.
Initialization generates one seed and store ID, derives public metadata, and
CAS-creates root revision one. Concurrent initializers race through CAS; losers
cleanse generated material and load the winner. An absent root with any peer
item is `identity_loss`.

Open and refresh load the root, enumerate current-store peer items, keyed-get
each item, validate external and encoded revisions, validate identifier and
root bindings, validate all peer keys, authenticate every record, enforce
global key/tombstone uniqueness, and recheck the root revision before adopting
state. Old-store items are ignored. A concurrent observed revision change is
`revision_conflict`; the caller retries from a fresh read.

An active peer can be created only by explicit pairing commit. Commit
CAS-creates one peer item at revision one. Rotation requires the current pin,
a distinct validated key, exactly the next counter, and available tombstone
capacity; it CAS-replaces the same stable item. Revocation CAS-replaces the
item while retaining pin history. Forget is explicit, applies only to a
revoked peer, and CAS-deletes that peer item. A later pairing is a new commit.
No transition automatically accepts a changed key.

Every mutation encodes and validates the complete candidate peer before store
access and rechecks that the loaded root revision is current. On encoding,
crypto, permission, availability, or CAS failure, durable and in-memory state
remain unchanged. Operations on different peer items are not a multi-item
transaction; the native engine's single-owner rule serializes normal runtime
use, and load fails closed if cross-process races create duplicate trust keys.

Reset generates a new seed and store ID, then CAS-replaces the root at exactly
the next root revision while recording the replaced store ID as the retired
store ID. That successful root CAS is the reset commit point: all old-store
peer items become unreachable immediately, including items written by a stale
process racing after the reset. In-memory state adopts the new empty identity
and cleanses the old seed.

`CleanupStaleItems` validates the current root and may delete only peer items
whose item ID contains that persisted retired store ID. It never treats an
arbitrary mismatch with a process-cached store ID as stale. The marker remains
in the root so cleanup can resume after restart. Before another reset may
replace the marker, enumeration must prove that the marked generation has no
remaining items; otherwise reset fails with `invalid_state`. Cleanup failure
sets `ResetOutcome.cleanup_complete` false but cannot roll back or misreport
the already committed security reset.

### Secret API and lifetime

The persistence codec and root record are private to the identity
implementation. Resident repository state contains only root revision, public
key, device ID, current store ID, and optional retired store ID metadata.
Public headers expose neither root codec functions nor a seed getter. A caller
accesses the seed only through synchronous `UseIdentitySeed(callback)`, whose
borrowed span is valid only for the callback invocation and must not be
retained.

Seeds and derived keys use move-only buffers that are explicitly cleansed on
clear, move-from, replacement, and destruction. They never enter errors, logs,
the C ABI, peer views, item identifiers, or plaintext fallback storage.

### Failure semantics

The core exposes stable error classes without record contents or secret
material:

- `storage_locked`, `storage_unavailable`, and `permission_denied`;
- `not_found`, `revision_conflict`, and `invalid_state`;
- `corrupt_record`, `unsupported_schema`, `rollback_detected`, and
  `identity_loss`;
- `capacity_exceeded`, `invalid_argument`, `entropy_failure`, and
  `crypto_failure`.

All failures are fail-closed for pairing and authenticated transport.
Corruption, identity loss, and rollback are not repaired automatically.

### Rollback and restore limits

Encoded/external revisions, root derivations, store/item binding, and peer
HMACs detect malformed, partial, mixed-root, and internally inconsistent
restores. They do not detect restoration of a complete valid older root item,
or a complete valid older revision of one peer item, when the platform rolls
back both revision metadata and payload. None of the selected desktop
facilities supplies a universally trusted monotonic counter. This residual
limitation is inherited from ADR 0002 and ADR 0006 and must not be represented
as rollback protection.

If only peer items survive without the root seed, load returns
`identity_loss`; copied peer records never restore trust. After reset, restoring
old peer items without the matching old root store ID does not restore trust.

### Platform status

Platform adapter status is:

| Platform | Required facility | Status |
| --- | --- | --- |
| macOS | non-synchronizing `ThisDeviceOnly` Keychain items | unresolved |
| Windows | current-user Credential Manager with `CRED_PERSIST_LOCAL_MACHINE` | unresolved |
| Linux | qualified device-local, non-synchronizing Secret Service items | adapter implemented; concrete backend qualification unresolved |

The Linux adapter uses pinned libsecret 0.21.7 and only the default persistent
collection. It does not unlock collections or execute Secret Service prompts.
Locked items return `storage_locked`; an absent service, absent default
collection, service-owner change, denied operation, malformed attributes,
duplicate item, or unexpected response fails closed.

Factory construction requires a backend qualifier. Before invoking it, the
adapter resolves the unique D-Bus owner, verifies that the service runs as the
current user, and supplies the owner's PID and `/proc/<pid>/exe` path. The
qualifier must establish from deployment-owned evidence that this exact
service is device-local and non-synchronizing. There is deliberately no
built-in executable allowlist. An absent, throwing, or rejecting qualifier
returns `storage_unavailable`, so the repository currently enables no concrete
Linux service by default.

The libsecret session must negotiate
`dh-ietf1024-sha256-aes128-cbc-pkcs7`; a plaintext session is rejected. Binary
values use libsecret `SecretValue` secure memory and a private `XNSL` version-1
envelope containing the revision, payload length, and canonical record bytes.
Only the schema, application identifier, and item identifier are non-secret
lookup attributes.

Secret Service does not expose compare-and-swap. Cooperating XnnTransfer
processes serialize the read-check-write sequence with `flock` on a
payload-free lock file in `XDG_RUNTIME_DIR`. The adapter rejects a relative,
non-user-owned, group/other-accessible, symlinked, multiply linked, or
non-regular lock path. Every operation rechecks the unique service owner,
default collection alias, and lock state. Create, replace, and delete use
direct D-Bus calls and reject any prompt instead of displaying it.

Until a concrete backend proves non-synchronization, item limits, deletion,
restart persistence, and locked/denied behavior in platform integration tests,
Linux pairing remains explicitly unsupported. There is no filesystem,
environment-variable, or production in-memory secret fallback; the runtime
file is synchronization metadata only and never contains a key or record.

## Acceptance boundary

The platform-independent schema, failure model, internal codec, cryptographic
helpers, validator boundary, fake-store transaction tests, and repository
state machine can be accepted independently. The ADR remains proposed, and no
platform support may be claimed, until each production adapter has integration
evidence for:

- non-synchronizing device-local protection and qualification;
- bounded enumerate/get/CAS put/delete behavior across processes;
- locked, unavailable, denied, corrupt, rollback, restore, and deletion paths;
- root replacement and stale-item cleanup after reset;
- seed lifetime and cleanup at the operating-system API boundary.
