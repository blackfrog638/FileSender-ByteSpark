# ADR 0002: Authenticated pairing and bound TLS transport

- Status: proposed
- Date: 2026-08-06
- Security requirements: `protocol/security/threat-model.md`
- Required negative coverage: `protocol/security/negative-test-matrix.md`

## Context

XnnTransfer discovers peers and transfers private files on a LAN. The LAN,
discovery packets, addresses, names, and all pre-authentication protocol input
are hostile. Discovery can establish reachability, but it cannot establish
identity or user intent.

The repository does not currently implement discovery, pairing, transport
security, or the transfer protocol. This ADR selects the security design that
those implementations must satisfy; it is not an implementation claim.

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

### Device identity

Each installation has one device-local identity key. The stable device
identifier is the SHA-256 digest of a domain-separation label and the canonical
32-byte Ed25519 public key. The identifier and public key must not be included
in unauthenticated discovery advertisements.

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
6. After local confirmation, each peer sends a key-confirmation value over
   the pairing channel. It is an HMAC over `pair_context`, role, and the
   confirmation decision using a second, distinctly labelled TLS exporter.
7. A peer becomes trusted only after local confirmation and verification of
   the matching peer confirmation. Each side then atomically stores the
   peer's public key, device identifier, approved security floor, a local
   display label, and trust-state metadata.

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
transport_context = SHA-256(canonical(
  "XnnTransfer transport v1",
  initiator role, responder role,
  both identity keys,
  both fresh 256-bit session nonces,
  security profile,
  offered versions and capabilities,
  selected version and capabilities,
  transfer session identifier))

bind_key = TLS-Exporter(
  "EXPORTER-XnnTransfer-Transport-v1",
  transport_context,
  32)

TRANSPORT_FINISHED(role) =
  HMAC-SHA256(bind_key, transport_context || role)
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
