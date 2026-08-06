# LAN pairing and transport threat model

- Status: design requirements; no implementation exists
- Scope: first pairing, authenticated reconnect, transport binding, and peer
  key lifecycle
- Governing decision:
  `../../docs/adr/0002-pairing-and-transport-security.md`
- Negative coverage: `negative-test-matrix.md`

The key words MUST, MUST NOT, REQUIRED, SHOULD, SHOULD NOT, and MAY describe
requirements for future protocol and native implementations. They do not
describe behavior present in the current repository.

## Security objectives

XnnTransfer must let a user select and pair with an intended LAN device, then
transfer only over a confidential, integrity-protected channel authenticated
to the pinned device identity. A nearby attacker must not gain trust by
controlling discovery, addresses, names, or first-contact traffic.

The protocol must also preserve explicit user intent. Pairing authorizes a
device identity to request transfers; it never authorizes automatic file
acceptance, arbitrary destination paths, or unlimited resource use.

## Assets

| Asset | Required property |
| --- | --- |
| File contents and hashes | Confidentiality and integrity in transit |
| File names, paths, sizes, manifests, and resume state | Confidentiality and integrity before receiver consent |
| Device identity private key | Confidentiality, integrity, device locality |
| Peer pins and trust state | Integrity, atomicity, rollback detection where supported |
| Ephemeral TLS and exporter keys | Confidentiality, uniqueness, non-persistence |
| Pairing transcript and user decision | Authenticity, freshness, attempt binding |
| Version, capability, role, and session negotiation | Integrity, peer and channel binding |
| Revocation and rotation state | Integrity, monotonicity, immediate enforcement |
| Device and relationship metadata | Minimized disclosure before authentication |
| CPU, memory, sockets, storage, and UI attention | Bounded consumption |

## Adversaries and capabilities

### Hostile LAN participant

The primary attacker can:

- observe, inject, modify, delay, drop, duplicate, reorder, and replay packets;
- spoof multicast discovery, names, addresses, ports, routes, DNS, ARP, and
  IPv6 neighbor discovery;
- terminate or relay connections and run an active man-in-the-middle attack;
- open many connections from one or many addresses and send fragmented,
  oversized, malformed, or state-invalid input;
- race a legitimate peer, reuse old transcripts, and advertise unsupported or
  weaker versions;
- observe packet endpoints, timing, and volume even when payloads are
  encrypted.

The attacker does not break correctly implemented Ed25519, X25519, SHA-256,
HKDF, HMAC-SHA256, or TLS 1.3, and does not control the OS CSPRNG.

### Malicious or compromised paired peer

A paired peer is authenticated, not trusted with the local filesystem or
availability. It can send syntactically valid but hostile manifests, paths,
sizes, hashes, transfer ordering, cancellation, and resume messages. It knows
metadata deliberately disclosed to it and may retain received data.

A stolen identity key lets an attacker impersonate that device until each
peer revokes it. Rotation signed only by a compromised old key cannot recover
trust; recovery requires explicit revocation and a new SAS pairing.

### Local attacker

An unprivileged local process may try to read files, logs, pairing records, or
IPC and may race application state. Platform access controls and native API
validation are in scope. A process already able to act as the XnnTransfer user
or inject into the process can exercise that user's authority and is not
fully contained by this protocol.

### Explicitly out of scope

- a compromised kernel, administrator/root account, firmware, or TLS/crypto
  dependency;
- a user who confirms without comparing all SAS words or knowingly accepts a
  mismatch;
- confidentiality after the receiver intentionally accepts and stores a file;
- hiding LAN endpoints, traffic timing, traffic volume, or radio/Ethernet
  presence;
- availability under link saturation, OS socket-table exhaustion, or a
  sufficiently distributed denial-of-service attack;
- centralized account recovery or global revocation distribution.

These exclusions must be stated in product claims. They do not permit unsafe
fallback behavior.

## Trust boundaries

| Boundary | Untrusted side | Enforcement required on trusted side |
| --- | --- | --- |
| Discovery to native session | Advertisements, names, endpoints, versions | Parse with strict bounds; treat only as a candidate; never create trust |
| Network to TLS library | Records, certificates, alerts, handshake order | Maintained TLS 1.3 parser, strict profile, timeouts, size and concurrency caps |
| Initial TLS to paired identity | Self-signed keys and pairing messages | Canonical transcript, exporter SAS, explicit local and peer confirmation |
| Established TLS to protocol parser | Authenticated but potentially malicious peer | Pin check, transport finished, state machine, framing and semantic limits |
| Native session to secure storage | Records loaded from disk and storage failures | OS access control, authenticated integrity, schema/version checks, atomic updates |
| Flutter to native core | Attempt IDs and user actions | Native-derived identity/transcript; reject stale, duplicate, or wrong-state actions |
| Paired peer to transfer/storage | File metadata, paths, bytes, resume claims | Separate receiver consent, path containment, quotas, hashes, atomic commit |

Flutter may display a native-derived SAS and submit a decision for an opaque
attempt ID. It must not supply the peer key, transcript, selected profile, or
SAS that the native layer uses for trust.

## Security state model

```text
unpaired
   |
   | explicit local pairing window + candidate selection
   v
pairing_pending
   |  \
   |   \ reject / mismatch / timeout / any error
   |    ----------------------------------------> unpaired
   |
   | local SAS confirmation + verified peer confirmation + atomic commit
   v
paired
   |  \
   |   \ authenticated dual-key rotation
   |    -------------------------------> paired(new pin)
   |
   | local revoke / key loss / identity reset / trust-record corruption
   v
revoked
   |
   | explicit "forget and pair again" + complete SAS ceremony
   v
paired(new trust record)
```

`pairing_pending` is never a partially trusted state. A crash before durable
commit resolves to `unpaired`; a corrupt or ambiguous durable record resolves
to `revoked`. A peer on one side may remain paired after asymmetric failure,
but the other side must reject it until explicit re-pairing.

## Security invariants

These invariants are release blockers for pairing and transport code.

| ID | Invariant |
| --- | --- |
| SEC-01 | Discovery data never creates, restores, rotates, or raises trust. |
| SEC-02 | Pairing starts only from a current explicit local user action and expires within a bounded window. |
| SEC-03 | Initial TLS remains unauthenticated until both local SAS confirmation and matching peer key confirmation complete. |
| SEC-04 | Established transport accepts exactly the active pinned Ed25519 key; CA, name, address, and device ID cannot override a mismatch. |
| SEC-05 | No file metadata, content, resume state, or transfer command is accepted before mutual transport-finished verification. |
| SEC-06 | Every connection uses fresh ECDHE; traffic keys, exporter keys, nonces, and attempt IDs are not reused or persisted. |
| SEC-07 | TLS 1.2, plaintext, 0-RTT, tickets, PSK resumption, anonymous modes, and automatic security-profile downgrade are rejected. |
| SEC-08 | Pairing and transport transcripts bind both identities, ordered roles, nonces, offered and selected versions/capabilities, profile, and session. |
| SEC-09 | Replayed or duplicate state-changing messages fail or return the prior idempotent result; they never repeat an effect. |
| SEC-10 | Trust is stored only by an atomic, schema-versioned commit after confirmation; partial or detected rolled-back state grants no trust. |
| SEC-11 | A peer key is never silently replaced. Routine rotation proves both old- and new-key possession; recovery uses full re-pairing. |
| SEC-12 | Revocation blocks new work and terminates active work for that peer before reporting success to the caller. |
| SEC-13 | Identity private keys and trust-record integrity keys never enter logs, Flutter memory, discovery, or plaintext storage. |
| SEC-14 | Entropy, TLS, parser, transcript, storage, timeout, and policy errors fail closed with idempotent cleanup. |
| SEC-15 | Pre-authentication messages, bytes, connections, cryptographic work, timers, and UI prompts are strictly bounded. |
| SEC-16 | Pairing authorizes peer identity only; each incoming transfer still requires receiver policy and explicit consent. |
| SEC-17 | Unauthenticated output and logs do not reveal SAS material, trust membership, stable peer identity, file metadata, or path data. |
| SEC-18 | A stale UI confirmation, delayed peer confirmation, or confirmation from another connection cannot approve the current attempt. |
| SEC-19 | Unknown critical fields, algorithms, profiles, and state transitions are rejected; optional extensions cannot weaken an invariant. |
| SEC-20 | Security state and cleanup are deterministic across timeout, cancellation, disconnect, process restart, and duplicate input. |

## Threat analysis and controls

### Discovery spoofing and peer confusion

An attacker can clone a display name and win a connection race. Discovery
therefore exposes only a candidate endpoint and a rotating instance token.
The pairing transcript obtains the identity from TLS proof of possession, not
from discovery. The user authenticates the transcript-derived SAS on both
devices.

No unauthenticated advertisement may include a stable device ID, identity
public key or fingerprint, pairing relationship, account identifier, file
name, path, size, transfer intent, or trust state. A user-enabled display label
must be bounded and treated as untrusted text.

### Man in the middle and unknown-key share

During first contact, a MITM can create two independent TLS connections. The
TLS exporter makes each connection produce an independent SAS. Binding both
ordered identity keys and roles prevents a transcript from being interpreted
as pairing with another peer or in the opposite role. Both endpoints must
display the same five words and receive confirmation for the same context.

After pairing, exact mutual pins authenticate TLS before protocol input is
accepted. A connection authenticated as peer A cannot carry a context naming
peer B because identities and role-specific finished values are exporter
bound.

### Replay and cross-connection substitution

Pairing and transport use independent labels, fresh peer nonces, and the TLS
exporter. Captured confirms are invalid on another handshake. Native state
must additionally track bounded sets of live attempt IDs, transport session
IDs, rotation counters, and state-changing operation IDs so a replay on the
same or a later authenticated connection cannot duplicate an effect.

Transfer-level replay rules, including chunk and resume idempotency, remain a
versioned wire-protocol prerequisite. TLS record replay protection alone is
not sufficient across reconnects.

### Version and capability downgrade

The complete offered sets and selected values are part of both bound
transcripts. Selection must be deterministic and must meet the local policy,
the peer record's stored security floor, and the mandatory profile. Unknown or
unsupported mandatory values fail with a generic authenticated error. No
failure path retries with plaintext, an older TLS version, a weaker profile,
or unauthenticated trust-on-first-use.

### Key replacement and rollback

An endpoint presenting a different key is an unknown peer even if its device
name, address, certificate fields, or derived device identifier resembles a
known peer. Routine rotation is accepted only on a channel authenticated to
the old pin and with proof by both old and new keys. Rotation counters and
revocation tombstones detect replay of an older signed rotation.

Secure storage must distinguish absence, first initialization, corruption,
detected rollback, and locked/unavailable states. Only first initialization
may create a new local identity without prior records. Any later identity loss
is a visible reset that invalidates existing pairings. Platforms without a
trusted monotonic facility cannot reliably identify a complete, valid old
secure-storage snapshot; they must document that residual risk and must not
claim general rollback resistance.

### Failure handling

Before trust, externally visible errors should distinguish only broad classes
needed for interoperability, such as unsupported security profile, busy, or
failed pairing. They must not confirm whether a particular identity is paired
or revoked. Detailed diagnostics are local, redacted, and rate-limited.

On any failure, the implementation cancels timers and pending I/O, zeroizes
ephemeral key material where supported, closes the connection, removes
in-memory pending state, and leaves durable trust unchanged. Cleanup is safe
to invoke repeatedly.

## Key lifecycle requirements

### Generation and storage

- Identity keys MUST be generated by the platform CSPRNG in native code.
- Platform backends MUST be device-local and non-synchronizing: Keychain on
  macOS, CNG/DPAPI-protected storage on Windows, and Secret Service or an
  equivalently reviewed backend on Linux.
- A backend that cannot protect key confidentiality and trust-record integrity
  MUST make pairing unavailable. Plaintext private-key or trust-record
  fallback is prohibited.
- Pairing records MUST use authenticated, schema-versioned serialization and
  atomic replace with durability appropriate to each platform.
- Backends SHOULD anchor record generations in trusted monotonic state when
  the platform provides it. A detected stale generation MUST fail closed.
- File permissions are defense in depth, not a replacement for protected
  storage.

### Rotation

Routine rotation requires an authenticated transport, a fresh new key, a
monotonic per-identity rotation counter, a fresh nonce, and signatures by both
old and new keys over the full rotation context and current channel binding.
The peer verifies all fields before atomically updating the pin. Interrupted
rotation retains the old valid record.

### Revocation and re-pairing

Local revocation closes active sessions, rejects subsequent connections, and
records a bounded tombstone containing the old fingerprint and monotonic
metadata. It does not send a network message whose delivery is assumed.
Remote revocation notice MAY be advisory but cannot replace local enforcement.

Re-pairing is a new trust decision. It requires a current user action, a new
TLS handshake, fresh nonces, full SAS comparison, and a new atomic record.
Names, prior address, prior trust, or a signed statement from a revoked or lost
key cannot skip the ceremony.

## Privacy requirements

- Discovery is off unless the product indicates discoverability to the user.
- Discovery instance tokens contain at least 128 random bits and rotate on
  process restart and at least every 15 minutes while advertising.
- Advertisements contain only routing/version hints needed to initiate a
  bounded pairing or authenticated connection. Stable identity is revealed
  only inside TLS.
- File metadata is disclosed only after pinned transport authentication and
  as required for a user-visible transfer offer.
- Logs, telemetry, crash reports, clipboard, and notifications omit private
  keys, traffic/exporter keys, SAS words, full fingerprints, file paths, and
  file names. Locally displayed peer labels are escaped and length-bounded.
- The product must not claim peer anonymity or traffic-analysis resistance.

## Denial-of-service limits

The versioned protocol may choose stricter values, but it must not exceed
these pre-authentication ceilings without a security review:

| Resource | Maximum |
| --- | --- |
| User-opened pairing window | 120 seconds |
| Concurrent user-visible pending pairing attempts | 1 |
| Incomplete unauthenticated TLS handshakes | 8 global, 2 per source address |
| TLS handshake completion | 5 seconds |
| Idle pairing-control interval | 30 seconds |
| Total pairing attempt | 120 seconds |
| Pairing-control application data before trust | 64 KiB and 16 messages |
| Certificate chain | One self-signed identity certificate, size capped by the wire/TLS profile |

Authentication failures must feed bounded per-source and global token buckets
with jittered backoff. Global limits must reserve capacity for one
user-initiated attempt so background discovery traffic cannot permanently
starve pairing. Per-address limits are not treated as identity or a complete
distributed-DoS defense.

All length checks precede allocation and expensive parsing. Unauthenticated
requests do not trigger filesystem scans, file hashing, persistent writes,
unbounded logs, repeated UI prompts, or outbound connection fan-out.

## Protocol implementation prerequisites

Before implementation, the versioned protocol and testdata owners must provide:

1. A canonical binary encoding with rejection rules for duplicates,
   non-canonical values, invalid Unicode, unknown critical fields, and
   trailing data.
2. Exact state diagrams for pairing, confirmation, established transport,
   rotation, revocation notice, cancellation, timeout, and reconnect.
3. Deterministic version/capability selection and a monotonic security-floor
   rule, including compatibility behavior for future profiles.
4. Exact transcript field order, domain-separation labels, key encodings, SAS
   bit extraction, a fixed 2,048-word list, and cross-platform golden vectors.
5. Framing, certificate, message, collection, and aggregate byte limits plus
   machine-readable error classes that do not create a trust-membership oracle.
6. Duplicate and replay semantics for every state-changing operation,
   transfer session, chunk, cancellation, and resume message.
7. A TLS configuration conformance harness that proves profile enforcement,
   exact pinning, exporter agreement, disabled early data/resumption, and
   rejection of fallback on all target platforms.
8. Secure-storage fault-injection tests and the complete negative matrix in
   `negative-test-matrix.md`.

The implementation must not infer any missing wire behavior from this threat
model. Missing details block implementation and require review in the
versioned specification.
