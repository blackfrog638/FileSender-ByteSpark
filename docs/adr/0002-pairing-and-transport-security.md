# ADR 0002: Authenticated pairing and bound TLS transport

- Status: accepted
- Date: 2026-08-06
- Accepted: 2026-08-07
- Security requirements: `protocol/security/threat-model.md`
- Required negative coverage: `protocol/security/negative-test-matrix.md`
- Runtime provider review: `protocol/security/XT-024-runtime-review.md`

## Context

XnnTransfer discovers peers and transfers private files on a LAN. The LAN,
discovery packets, addresses, names, and all pre-authentication protocol input
are hostile. Discovery can establish reachability, but it cannot establish
identity or user intent.

The repository implements LAN discovery, protected identity storage, the
byte-exact security-profile primitives, and a TLS 1.3 provider. It does not
implement the pairing/session state machine or authenticated file transfer.
This ADR selects the security design that all of those implementations must
satisfy; design acceptance and lower-level provider review are not an
end-to-end implementation claim. Every cross-platform conformance,
secure-storage, session, and negative-test prerequisite below remains
mandatory before production use or a security claim.

The design must:

- authenticate a first pairing without a pre-existing PKI or shared secret;
- bind every later transport to the identities approved during pairing;
- reject replay, active interception, version downgrade, and silent key
  replacement;
- preserve forward secrecy for file traffic;
- make revocation, key loss, and re-pairing explicit;
- work with bounded resource use on an adversarial LAN.

## Decision

### Security profile

The initial mandatory security profile is:

| Purpose | Required primitive or policy |
| --- | --- |
| Device identity | Ed25519 key pair generated from the OS CSPRNG |
| Transport | TLS 1.3 only, with mutual proof of identity-key possession |
| Key agreement | Fresh X25519 ECDHE for every connection |
| Mandatory TLS suite | `TLS_AES_128_GCM_SHA256` |
| Optional TLS suite | `TLS_CHACHA20_POLY1305_SHA256` |
| First-pair authentication | Five-word, 55-bit short authentication string |
| Channel binding | TLS exporter with distinct pairing and transport labels |
| Established-peer trust | Exact pin of the peer's Ed25519 public key |
| Early data and resumption | Disabled in v1 |

An implementation must use a maintained TLS 1.3 library. A self-signed X.509
certificate may carry the Ed25519 public key, but certificate chain, DNS name,
LAN address, validity metadata, and discovery metadata confer no trust. The
native session layer must verify proof of private-key possession and pin the
canonical public key bytes, not the certificate serialization.

The wire specification must assign an unambiguous identifier to this complete
profile. Adding a weaker profile or changing a primitive requires a new ADR,
new interoperability vectors, and an explicit compatibility rule. There is no
plaintext or TLS 1.2 fallback.

### Byte-exact profile definition

This section makes the accepted design deterministic for independent golden
vector and implementation-conformance review. It remains design-level test
evidence and does not claim that TLS, pairing, or rotation is implemented.

All integers use unsigned big-endian encoding. All labels are the exact ASCII
octets shown below, without a terminator. A role is one octet: initiator is
`01` and responder is `02`. A confirmation decision is one octet: reject is
`00` and confirm is `01`. Ed25519 public keys, nonces, SHA-256 values, exporter
outputs, and HMAC-SHA256 values are exactly 32 octets. A transfer session
identifier is exactly 16 octets. Within this accepted design,
`security_profile` is the two-octet value `00 01`; production use remains
blocked on wire registration and the required implementation prerequisites
below.

An accepted Ed25519 identity or rotation public key must satisfy all of the
following before use:

1. It is exactly 32 octets and canonically decodes as the compressed
   Edwards25519 point `P` under RFC 8032 section 5.1.3.
2. `P` is not the Edwards25519 identity `(0,1)`.
3. `[L]P` is the identity, where the prime subgroup order is
   `L = 2^252 + 27742317777372353535851937790883648493`.

The implementation must perform the subgroup-membership computation. A
small-order blacklist, point decompression alone, or a cofactor-only check
such as `[8]P != identity` does not satisfy this rule. The check applies before
a key enters TLS identity acceptance or private-key-possession verification,
and before pairing or transport transcript hashing, pin comparison, device-ID
derivation, rotation processing, or durable trust. The exact accepted 32
octets must be the same bytes used for possession verification, transcript
fields, pinning, and device-ID derivation.

Canonical security objects use this envelope:

| Field | Encoding |
| --- | --- |
| `magic` | four ASCII octets `XNNS` |
| `canonical_version` | `U8`, exactly `01` |
| `object_kind` | `U8`, defined below |
| `field_count` | `U16` |
| `body_length` | `U32`, octets following the envelope |
| fields | exactly `field_count` field records |

Each field record is `field_id:U16 || value_length:U32 || value`. Field IDs
are strictly increasing and every field listed for an object kind occurs
exactly once. Unknown, duplicate, omitted, out-of-order, truncated, or trailing
data is invalid. The encoded body is limited to 1,048,576 octets and 32 fields.
No text normalization, host struct layout, host byte order, or locale operation
participates in encoding.

Object kind `01`, normalized negotiation, has these fields:

| ID | Value |
| ---: | --- |
| 1 | initiator role, `U8` |
| 2 | initiator version range |
| 3 | initiator offered capabilities |
| 4 | initiator required capabilities |
| 5 | initiator receive limits |
| 6 | responder role, `U8` |
| 7 | responder version range |
| 8 | responder offered capabilities |
| 9 | responder required capabilities |
| 10 | responder receive limits |
| 11 | selected version |
| 12 | selected capabilities |
| 13 | effective receive limits |

A version range is
`min_major:U8 || min_minor:U8 || max_major:U8 || max_minor:U8`.
A selected version is `major:U8 || minor:U8`. A capability list is
`count:U16 || count * capability:U32`; values are unique and numerically
sorted, and one capability ID cannot occur at multiple versions. Receive
limits are `max_body:U32 || max_in_flight:U32 || max_streams:U16`.
Selection must be the highest common version, the exact sorted capability
intersection satisfying both required sets, and the component-wise minimum
receive limits. This is the byte-exact representation of
`normalized_negotiation` required by `protocol/spec/v1.md`.

Object kind `02`, pairing context input, has:

| ID | Value |
| ---: | --- |
| 1 | ASCII `XnnTransfer pairing v1` |
| 2, 3 | initiator role, responder role |
| 4, 5 | initiator nonce, responder nonce |
| 6, 7 | initiator Ed25519 key, responder Ed25519 key |
| 8 | `security_profile:U16` |
| 9 | complete encoded normalized-negotiation object |

`pair_context` is SHA-256 over the complete kind-`02` object. The pairing TLS
exporter API tuple is:

```text
label   = ASCII "EXPORTER-XnnTransfer-Pairing-v1"
context = pair_context
length  = 32
```

The returned 32-octet exporter output is the HKDF-SHA256 pseudorandom key for
SAS derivation. HKDF performs Expand only. Its `info` is a complete canonical
kind-`03` object with field 1 equal to ASCII
`XnnTransfer SAS words v1` and field 2 equal to `pair_context`; output length
is seven octets. The first 55 bits are the five consecutive 11-bit,
most-significant-bit-first indices. The unused least-significant bit of the
seventh octet is ignored.

Indices select zero-based entries from the 2,048-entry BIP39 English word list
stored at `protocol/testdata/security/v1/wordlist.txt`. BIP39 checksum and seed
semantics are not used. Implementations display the five selected lowercase
ASCII words in index order and do not translate, normalize, substitute, or
accept prefixes.

The peer-confirmation exporter API tuple is:

```text
label   = ASCII "EXPORTER-XnnTransfer-Pairing-Confirmation-v1"
context = pair_context
length  = 32
```

For sender role `role` and decision `decision`:

```text
PAIR_CONFIRMATION(role, decision) =
  HMAC-SHA256(confirmation_exporter,
              pair_context || role:U8 || decision:U8)
```

Confirmation verification takes the expected live `pair_context` and expected
sender role as separate state-machine inputs; neither is inferred from the
message. It first requires the exact 34-octet message shape, the expected role,
and a decision in `{00, 01}`, then authenticates the complete message with the
confirmation exporter and compares its context to the expected live context.
An authenticated result is typed as follows:

| Decision | Authenticated result | State-machine effect |
| ---: | --- | --- |
| `00` | `authenticated_reject` | terminally reject the live attempt, erase pending state, and prohibit a trust write |
| `01` | `affirmative_confirm` | record peer confirmation for this live attempt; this result alone does not authorize trust |

An out-of-range decision is `invalid_decision` and fails closed. A decision
octet substituted without its matching HMAC is
`confirmation_mismatch`, not an authenticated decision. When an affirmative
peer confirmation is required, a correctly authenticated `00` produces the
distinct terminal result `authenticated_reject`; it must never be returned as
success merely because its HMAC is valid.

The trust commit gate requires all of the following for the same currently
live attempt: the local decision is exactly `01`, the authenticated peer
decision is exactly `01`, both confirmations bind the attempt's exact
`pair_context` and role, and all other pairing checks still hold. Any local or
peer `00`, invalid decision, stale context, role mismatch, authentication
failure, timeout, or closed attempt makes the gate terminally reject without a
trust write. A previously authenticated decision cannot be applied to a new
attempt.

Object kind `04`, transport context input, has:

| ID | Value |
| ---: | --- |
| 1 | ASCII `XnnTransfer transport v1` |
| 2, 3 | initiator role, responder role |
| 4, 5 | initiator Ed25519 key, responder Ed25519 key |
| 6, 7 | initiator session nonce, responder session nonce |
| 8 | `security_profile:U16` |
| 9 | complete encoded normalized-negotiation object |
| 10 | exact `raw_negotiation_transcript` from v1 section 6.2 |
| 11 | transfer session identifier |

`transport_context` is SHA-256 over the complete kind-`04` object. The
transport TLS exporter API tuple and finished values are:

```text
label   = ASCII "EXPORTER-XnnTransfer-Transport-v1"
context = transport_context
length  = 32

TRANSPORT_FINISHED(role) =
  HMAC-SHA256(transport_exporter, transport_context || role:U8)
```

Object kind `05`, rotation context input, has:

| ID | Value |
| ---: | --- |
| 1 | ASCII `XnnTransfer rotation v1` |
| 2, 3 | old Ed25519 key, distinct new Ed25519 key |
| 4 | nonzero monotonic rotation counter, `U64` |
| 5 | fresh rotation nonce, 32 octets |
| 6 | current `transport_context`, 32 octets |

`rotation_context` is SHA-256 over the complete kind-`05` object. The exact
message submitted independently to each Ed25519 signer is a kind-`06` object:
field 1 is ASCII `XnnTransfer rotation proof v1`, field 2 is
`rotation_context`, and field 3 is `01` for the old key or `02` for the new
key. The signatures themselves and Ed25519 implementation are deliberately
outside these exporter-input fixtures.

Object kind `07`, device identifier input, has:

| ID | Value |
| ---: | --- |
| 1 | exact 32 ASCII octets `XnnTransfer device identifier v1` (hex `586e6e5472616e7366657220646576696365206964656e746966696572207631`) |
| 2 | canonical 32-octet Ed25519 public key |

The canonical public key is the 32-octet compressed Edwards-y encoding defined
by RFC 8032, exactly matching the bytes used for public-key pinning and
private-key-possession verification. DER, PEM, certificate serialization,
text, hexadecimal, length prefixes inside field 2, and other public-key
representations are not accepted.

`device_identifier_input` is the complete encoded kind-`07` object, including
the `XNNS` envelope and both field records. The stable identifier is:

```text
device_identifier = SHA-256(device_identifier_input)
```

The canonical identifier is the 32 digest octets. Where a text representation
is required, it is exactly 64 lowercase ASCII hexadecimal characters in digest
byte order, without a prefix, separators, whitespace, or terminator. Decoders
must not accept an alternate label, alternate public-key encoding, or alternate
text representation as an equivalent derivation.

The normative fixtures are under `protocol/testdata/security/v1/`. Exporter
material in those files is fixed test input, not output from a TLS
implementation. Any encoding or cryptographic output mismatch fails closed;
there is no alternate decoding or fallback derivation.

The fixtures record the BR-04 identity-key forgery with public key `(0,1)`,
`R = B`, and `S = 1`, for which the cofactored verification equation reduces
to `[8]B = [8]B + [8]k(0,1)` for every message without a corresponding private
key. They also record the independent-review observation that OpenSSL 3.6.3,
Node/OpenSSL, and Apple CryptoKit accepted the identity signature while
libsodium rejects small-order and non-main-subgroup points. Identity,
non-identity low-order, and mixed-order fixture keys must all fail the
backend-independent rule above. These observations are security-profile design
evidence, not a claim of production Ed25519, TLS, pairing, or platform
conformance implementation.

### Device identity

Each installation has one device-local identity key. The stable device
identifier is the byte-exact kind-`07` SHA-256 derivation above. It is a stable
scope and display identifier only: it never authenticates a peer, proves
private-key possession, or overrides an exact Ed25519 public-key pin mismatch.
The identifier and public key must not be included in unauthenticated
discovery advertisements.

The private key and pairing records must be stored using a platform-protected,
non-synchronizing secure-storage backend. Private keys should be non-exportable
where the selected TLS library and platform permit it. There is no plaintext
file fallback. If secure storage is locked, corrupt, unavailable, or detects a
rollback, pairing and authenticated transport fail closed.

### First pairing

Pairing is possible only after a local user opens a short-lived pairing window
and chooses a candidate. A discovered name, endpoint, or previously observed
address is only a display hint.

1. The peers establish TLS 1.3 with fresh ECDHE and exchange self-signed
   identity certificates. Both identities are untrusted at this point. Only
   bounded pairing-control data may be processed on this connection.
2. Both sides exchange fresh 256-bit nonces, explicit initiator/responder
   roles, identity public keys, the security-profile identifier, and offered
   and selected protocol versions and capabilities.
3. They construct the same canonical `pair_context`:

   ```text
   SHA-256(canonical(
     "XnnTransfer pairing v1",
     initiator role, responder role,
     initiator nonce, responder nonce,
     initiator identity key, responder identity key,
     security profile,
     offered versions and capabilities,
     selected version and capabilities))
   ```

4. They derive 32 bytes with:

   ```text
   TLS-Exporter(
     "EXPORTER-XnnTransfer-Pairing-v1",
     pair_context,
     32)
   ```

   A separately labelled HKDF expansion maps the first 55 bits to five
   11-bit indices in a fixed 2,048-word, locale-independent word list. The
   exact encoding, word list, and golden vectors are protocol prerequisites.
5. Both devices display the words in the same order. The user compares both
   displays and explicitly confirms or rejects each pending attempt. Device
   name or address comparison is not a substitute. The UI confirmation is
   bound to an opaque attempt identifier so a stale confirmation cannot
   approve another connection.
6. After its local decision, each peer sends a key-confirmation value over the
   pairing channel. It is an HMAC over `pair_context`, role, and the
   confirmation decision using a second, distinctly labelled TLS exporter. A
   valid authenticated rejection terminates the attempt; it is not affirmative
   confirmation.
7. A peer becomes trusted only when both its local decision and the
   authenticated peer decision are exactly `01` for the same live attempt.
   Each side then atomically stores the peer's public key, device identifier,
   approved security floor, a local display label, and trust-state metadata.

The 55-bit SAS bounds an online active interception attempt to at most
`2^-55` per independently generated transcript when the user compares all
five words. A timeout, rejection, TLS failure, mismatched SAS, mismatched
transcript, or crash before the atomic commit leaves the peer untrusted.
Asymmetric completion after a crash is allowed to fail on the next connection;
it must not be silently repaired.

### Established transport

Every post-pairing connection uses a fresh TLS 1.3 handshake with mutual
identity-key proof. The peer public key must exactly match an active pairing
record before any application metadata is accepted. A valid CA chain,
matching hostname, matching discovery record, or matching device identifier
does not override a pin mismatch.

After TLS authentication, peers bind the application negotiation to this
specific transport:

```text
transport_context = SHA-256(canonical kind-04 object above)

transport_exporter = TLS-Exporter(
  "EXPORTER-XnnTransfer-Transport-v1",
  transport_context,
  32)

TRANSPORT_FINISHED(role) =
  HMAC-SHA256(transport_exporter, transport_context || role:U8)
```

File offers, paths, sizes, hashes, contents, resume state, and transfer
commands must not be processed until both role-specific finished values are
verified. Application frames remain inside TLS for the complete connection.
Exporter outputs and TLS traffic keys are never persisted or reused.

TLS 0-RTT, session tickets, pre-shared-key resumption, anonymous suites, and
renegotiation are disabled in v1. A fresh ECDHE handshake per connection
provides forward secrecy and ensures local peer revocation takes effect
without a ticket cache.

### Replay, interception, and downgrade

- Fresh TLS handshakes, 256-bit nonces, role binding, and exporter binding
  make pairing and transport confirmations connection-specific.
- Attempt identifiers, nonces, session identifiers, rotation counters, and
  accepted transfer operations need bounded duplicate detection. Replayed
  state-changing messages fail rather than execute twice.
- An active interceptor can relay or terminate the first untrusted TLS
  connection, but cannot make both endpoints display the same 55-bit SAS
  without finding a collision or controlling the user's confirmation.
- TLS versions, security profiles, wire versions, capabilities, identities,
  roles, and session identifiers are included in the bound transcript.
- A pairing record stores the minimum approved security profile. Future
  upgrades are monotonic; a lower profile requires explicit re-pairing and
  must never result from automatic negotiation.
- An authenticated peer may choose any mutually supported application
  capability at or above the stored security floor. The exact deterministic
  selection and unknown-field behavior belong in the versioned wire
  specification.

### Key rotation, revocation, and re-pairing

Routine identity rotation is allowed only over an already authenticated,
exporter-bound transport. A rotation message must identify the old and new
public keys, include a monotonic counter and fresh nonce, prove possession of
both private keys, and be bound to the current connection. The peer atomically
replaces the pin and retains a bounded tombstone for the old key. The UI must
surface the identity change.

If the old key is unavailable or suspected compromised, signed rotation is not
sufficient. The local pairing record must be revoked and the full SAS pairing
ceremony repeated. A changed key is never accepted as an automatic recovery.

Revocation immediately rejects new connections and closes active sessions for
that peer. Because v1 has no tickets, there is no resumption state to preserve.
A revoked fingerprint is retained as a bounded tombstone to prevent automatic
rediscovery from restoring trust. Re-pairing requires an explicit local
`forget and pair again` action followed by the complete first-pair flow.

Resetting or losing the local identity invalidates its existing peer
relationships. Restoring copied pairing records without the corresponding
protected identity key does not restore trust.

### Failure and resource policy

All authentication, transcript, secure-storage, entropy, parser, timeout, and
policy errors close the connection without accepting transfer metadata. Error
reporting must not reveal keys, SAS material, precise trust-record contents,
or a peer-enumeration oracle to an unauthenticated endpoint.

Pairing is disabled outside an explicit user-opened window. Implementations
must impose global and per-source limits on unauthenticated handshakes, cap
pre-authentication bytes and messages, use short handshake and confirmation
timeouts, and release partial state idempotently. Limits and machine-readable
failure classes must be normative in the versioned wire specification.
Network saturation remains outside the availability guarantee.

## Required implementation prerequisites

No pairing or transfer implementation may claim this ADR until all of the
following exist:

1. A reviewed, versioned wire specification defines canonical transcript
   encoding, roles, state transitions, deterministic negotiation, limits,
   timeouts, ALPN/profile identifiers, errors, and duplicate handling.
2. Cross-platform golden vectors cover both exporter contexts, SAS words,
   confirmations, transport binding, rotation proof, and malformed encodings.
3. The TLS dependency and configuration enforce the selected profile,
   identity proof, exact pinning, fresh ECDHE, and disabled early data and
   resumption on macOS, Windows, and Linux.
4. Platform secure-storage adapters have atomicity, permission, corruption,
   rollback-handling, identity-loss, and non-synchronization tests.
5. Native APIs model pairing attempts, explicit confirmation/rejection,
   revocation, rotation, and trust-state errors without allowing presentation
   code to inject identity or transcript values.
6. Every applicable row in
   `protocol/security/negative-test-matrix.md` has an automated test or an
   explicitly tracked platform test plan.
7. Independent security and integration-owner review accepts the ADR,
   interoperability vectors, and any C ABI or wire-contract change.

### Runtime implementation status

XT-024 independently reviewed the XT-022 protected-identity and XT-023 TLS
provider deliveries. The platform-independent identity repository, macOS and
Windows protected-store adapters, fail-closed Linux adapter boundary,
byte-exact profile primitives, and TLS provider are accepted as lower-level
inputs to session implementation.

This does not close the complete prerequisite list:

- XT-060 owns production pairing ALPN/profile registration, pairing states,
  errors, timeouts, duplicate behavior, and hostile golden vectors;
- XT-061 owns pre-handshake enforcement of the registered certificate limits;
- XT-062 owns device-local, non-synchronizing GNOME Keyring qualification and
  positive Linux lifecycle tests;
- XT-025 and XT-026 own pairing/session APIs and presentation-safe attempt
  handling, while later acceptance tasks retain adversarial integration,
  stateful fuzzing, and the remaining negative matrix.

The authoritative prerequisite and test-row disposition is
`protocol/security/XT-024-runtime-review.md`. Pairing and transfer conformance
remain blocked while any listed runtime-review blocker or negative-matrix
owner lacks executable evidence.

## Consequences

- Normal transfers use standard TLS 1.3 and exact peer pins, while first use
  requires a visible user ceremony.
- Five words are less convenient than trusting a discovered name, but provide
  a quantifiable active-interception bound without a central service.
- Disabling resumption adds a handshake to each connection and simplifies
  revocation, replay analysis, and forward-secrecy guarantees for v1.
- A compromised device identity key permits impersonation until peers revoke
  it. There is no central revocation or account recovery service.
- A platform without trusted monotonic storage cannot reliably distinguish a
  complete, valid old secure-storage snapshot from current state. Restoring
  such a snapshot may restore an older local pin or revocation state; remote
  private-key possession and all normal authentication checks still apply.
- Encryption does not hide packet timing, volume, endpoints, or the fact that
  two devices communicate.
- Rate limits bound local work but cannot guarantee availability under LAN
  saturation or distributed denial of service.

Alternatives rejected:

- Trusting discovery names, addresses, or multicast records: all are
  attacker-controlled and unstable.
- Trust on first use without comparison: it silently accepts an active
  interceptor on the first connection.
- Public-Web-PKI certificates: LAN devices generally lack stable names and a
  public CA does not express the user's device-pairing intent.
- A short plaintext PIN: without a reviewed PAKE it permits offline guessing
  or transcript substitution; adding a second cryptographic protocol is not
  needed for the selected display-to-display ceremony.
- Unauthenticated plaintext followed by an encrypted transport: it exposes
  negotiation to interception and makes channel binding easy to omit.
