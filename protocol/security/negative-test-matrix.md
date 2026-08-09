# Pairing and transport negative test matrix

- Status: required coverage; blocked until protocol and native implementations
  exist
- Threat model: `threat-model.md`

Passing this matrix is required before pairing or transport can claim the
security properties in ADR 0002. A rejected input passes only if it produces
the required state and cleanup result; a process crash, hang, leaked secret,
partial trust record, repeated side effect, or silent fallback is a failure.

The versioned wire specification must map the error classes below to exact
machine-readable values. Tests must assert errors only where doing so does not
give an unauthenticated peer a trust-membership oracle.

## Test levels

| Level | Required evidence |
| --- | --- |
| Unit | Deterministic transcript, parser, state-machine, limit, and storage fault tests |
| Golden | Cross-platform byte vectors for contexts, exporters, SAS, HMAC, signatures, and canonical rejection |
| Integration | Two real native engines plus adversarial relay/proxy and controlled secure storage |
| Platform | TLS and secure-storage behavior on macOS, Windows, and Linux |
| Fuzz | Pairing/transport stateful sequences, framing, certificate input, and durable record decoding |

The harness must support deterministic nonces and exporter fixtures for unit
and golden tests. Production entropy must never be replaced by deterministic
test entropy outside an explicitly test-only build.

## Pairing-control golden evidence

`protocol/testdata/security/v1/pairing-control-vectors.json` provides the
wire-level golden evidence owned by the pairing-control specification:

| Rows | Golden evidence | Remaining runtime evidence |
| --- | --- | --- |
| P-03, P-04 | fresh-context confirmation replay plus duplicate HELLO and post-confirm decision rejection | XT-025 live attempt replay set and idempotent local callback behavior |
| P-05, P-06, P-07 | role swap, deterministic transcript/context binding, exact profile/ALPN, and downgrade rejection | XT-025 live TLS/session integration |
| P-09, P-10, P-17, P-18 | authenticated rejection, peer-confirm-first ordering, commit failure, and cancellation-before-commit are explicit terminal fixtures | XT-025 protected-store fault injection, crash, and executor race tests |
| P-13, P-19 | exact certificate-key equality plus strict canonical message fields | XT-061 pre-handshake certificate byte enforcement and XT-025 parser integration |
| T-01, T-02, T-09 | exact active-pin reconnect, stored floor, fresh-handshake flags, kind-`04` context, and both finished values are fixed; wrong pin, lower floor, and invalid finished reject | XT-025 live TLS reconnect, resumption/early-data, and metadata-override tests |
| T-03 through T-05 | exact non-overlapping ALPN modes and registered profile policy | XT-025 live provider configuration tests |
| T-06 through T-10 | context/role/profile/version/capability binding and hostile canonical controls | XT-025 cross-connection state integration |
| R-03 through R-06, R-09 through R-12 | public abort classes, byte/message/admission/deadline ceilings, and one terminal transition are normative | XT-025 timers, admission, concurrency, and oracle tests |

These vectors are specification evidence, not runtime passes. A row remains
open while its remaining owner has no executable implementation evidence.

## Discovery and candidate handling

| ID | Attack or fault | Required result |
| --- | --- | --- |
| D-01 | Truncated, oversized, duplicate-field, invalid-encoding, or trailing discovery input | Reject before unbounded allocation; no candidate, trust write, crash, or detailed remote error |
| D-02 | Attacker clones a paired peer's display name, address, and advertised version | Candidate remains untrusted; established connection fails unless the exact pin proves possession |
| D-03 | Attacker replays an expired discovery instance token | It cannot restore an endpoint, pairing attempt, or trust state |
| D-04 | Advertisement contains a stable identity, fingerprint, file metadata, or overlong label | Sender conformance test fails; receiver ignores prohibited fields and bounds escaped display text |
| D-05 | Discovery flood rotates source addresses and instance tokens | Global candidate, memory, log, and connection limits hold; no persistent write or UI prompt storm |
| D-06 | Candidate endpoint changes between selection and connect | Connected identity is authenticated by SAS/pin; endpoint change never carries trust |

## First pairing

| ID | Attack or fault | Required result |
| --- | --- | --- |
| P-01 | Active MITM terminates two independent initial TLS sessions | Endpoints derive different SAS values; no trust record is committed unless the user incorrectly overrides the mismatch, which the UI must not offer |
| P-02 | Byte relay forwards one TLS handshake without termination | Peers may pair directly, but the relay cannot derive traffic/exporter keys or alter the transcript |
| P-03 | Captured peer confirmation is replayed on a fresh TLS handshake | Confirmation verification fails because exporter and nonces differ; attempt returns to unpaired |
| P-04 | Captured confirmation is replayed twice on the same attempt | Second input is rejected or idempotent; no duplicate record or state transition |
| P-05 | Initiator/responder roles or identity-key order are swapped | Context or role-specific HMAC mismatch; no trust commit |
| P-06 | Offered versions/capabilities are changed after SAS display | Context mismatch closes the attempt; UI confirmation cannot apply |
| P-07 | Security profile or selected version is omitted, unknown, or below policy | Generic unsupported/failed result; no weaker retry or trust record |
| P-08 | Flutter submits a valid confirmation for an expired or different attempt ID | Native layer rejects it; current attempt and durable trust remain unchanged |
| P-09 | User rejects or one endpoint reports rejection | Both sides close and remove pending state; neither persists trust |
| P-10 | Peer confirmation arrives before local user confirmation | It may be held only within bounds; it cannot create trust or suppress the local ceremony |
| P-11 | SAS word differs, is out of range, or word-list/version metadata differs | Pairing fails; implementations do not normalize to a possibly matching alternative |
| P-12 | Pairing window is closed, expired, or already has a visible pending attempt | New request is rejected as unavailable/busy without a second prompt |
| P-13 | TLS certificate uses the wrong key type, has multiple identity certificates, or lacks proof of possession | TLS/pairing fails before SAS display |
| P-14 | Certificate CA chain, DNS name, or discovery name is valid but the pairing transcript names another key | External metadata grants no trust; transcript identity is the proved key only |
| P-15 | File offer, path, size, hash, or transfer command arrives during initial pairing | Reject and close; no transfer parser, consent flow, file I/O, or persistent transfer state runs |
| P-16 | OS CSPRNG or TLS exporter fails or returns an invalid length | Fail closed before SAS display; no deterministic/reused fallback material |
| P-17 | Disconnect or crash occurs before both confirmations and atomic commit | Restart yields no trusted record from the incomplete attempt |
| P-18 | One peer commits and the other crashes before commit | Next connection fails on the unpaired side; neither side silently heals or replaces a key |
| P-19 | Canonical transcript contains duplicate, non-canonical, invalid Unicode, or trailing data | Reject before SAS derivation; cross-platform parsers agree on rejection |
| P-20 | Attacker repeatedly starts pairing without local user initiation | No SAS prompt or trust state; rate and concurrency limits engage |

## Established transport and downgrade resistance

| ID | Attack or fault | Required result |
| --- | --- | --- |
| T-01 | Peer presents a key different from the active pin | TLS authentication fails before any application metadata is processed |
| T-02 | Mismatched key has a publicly valid CA chain, matching hostname, name, address, or device-ID claim | Pin mismatch remains fatal; no PKI/TOFU fallback |
| T-03 | Peer offers TLS 1.2, plaintext upgrade, anonymous mode, or renegotiation | Connection fails; no retry with a weaker mode |
| T-04 | Peer omits mandatory X25519/Ed25519/AES-GCM profile support or selects an unapproved algorithm | Profile negotiation fails closed with no partial application session |
| T-05 | Peer offers 0-RTT, a ticket, or PSK resumption | Early data is not sent or accepted; v1 performs a full fresh handshake |
| T-06 | Captured `TRANSPORT_FINISHED` is replayed on another connection | HMAC verification fails because exporter/nonces/session differ |
| T-07 | Finished value uses the wrong role, identity order, session ID, version, or capabilities | Verification fails and no transfer state is created |
| T-08 | MITM removes the highest offered version or capability | Authenticated transcript/finished mismatch closes the connection |
| T-09 | Authenticated peer proposes a profile below the pairing record's security floor | Reject; no automatic floor reduction |
| T-10 | Unknown critical profile, version, capability, or message appears | Reject at the defined state; optional fields cannot alter transcript interpretation |
| T-11 | File metadata arrives after pin verification but before both transport finished values | Reject and close without invoking transfer or storage behavior |
| T-12 | TLS records or application frames are truncated, reordered, duplicated, or corrupt | Authentication/state check fails; cleanup is bounded and idempotent |
| T-13 | Same transfer operation ID or state-changing command is replayed | Return the defined duplicate result or reject; effect occurs at most once |
| T-14 | Peer claims a prior resume session under a new transport without valid bound resume state | Reject resume; never attach it by name, address, or unbound session ID |
| T-15 | Pinned but malicious peer sends paths, sizes, hashes, or file counts at limits | Pairing grants no storage authority; transfer/storage validation and user consent still run |
| T-16 | Peer is revoked while handshake, negotiation, or transfer is active | New work is rejected and active work terminates before revocation reports success |

## Key storage, rotation, and re-pairing

| ID | Attack or fault | Required result |
| --- | --- | --- |
| K-01 | Secure storage is locked, unavailable, permission-denied, or unsupported | Pairing/transport is unavailable; no plaintext fallback or regenerated identity |
| K-02 | Pairing record has invalid authentication, schema, length, or key encoding | Treat as corrupt/revoked; do not recover trust from display metadata |
| K-03 | Older valid record or rotation counter is restored | A backend with trusted monotonic state detects and rejects it; other backends document the limitation, do not claim rollback resistance, and still require possession of the restored pin |
| K-04 | Routine rotation proves only the old key or only the new key | Reject rotation; old pin stays active |
| K-05 | Rotation is sent outside the pinned exporter-bound transport | Reject without changing durable state |
| K-06 | A previously accepted rotation message/counter is replayed | Reject as stale; current pin and counter remain unchanged |
| K-07 | Crash or storage fault occurs at every rotation write boundary | Exactly the complete old or complete new record survives; never a mixed/empty trusted record |
| K-08 | Peer reconnects with the old key after successful rotation | Reject using old-key tombstone; do not roll back the pin |
| K-09 | Local identity key is missing while peer records remain | Enter visible identity-reset/revoked state; do not generate a key and reuse old trust |
| K-10 | Attacker changes discovery/certificate metadata to resemble a known peer with a new key | Treat as unknown; require explicit full SAS re-pairing |
| K-11 | Revoked peer is rediscovered or presents a valid old certificate | Tombstone prevents automatic trust restoration |
| K-12 | User requests re-pairing but SAS ceremony is incomplete | Remain revoked/unpaired; prior trust and names cannot skip confirmation |
| K-13 | Private key or trust database is configured for cloud/profile synchronization | Platform conformance fails; identity and trust state must remain device-local |
| K-14 | Duplicate revoke/cancel/cleanup calls race with disconnect | Final state is revoked with no active session, use-after-free, or duplicate callback |

## Privacy, errors, and denial of service

| ID | Attack or fault | Required result |
| --- | --- | --- |
| R-01 | Discovery is inspected across restart and 15-minute epochs | No stable identity/fingerprint is present; random instance token rotates as required |
| R-02 | Secrets, SAS, fingerprints, malicious labels, or file paths reach error paths | Logs/telemetry/crash output redact prohibited values and escape bounded labels |
| R-03 | Unauthenticated client probes known, unknown, paired, and revoked keys | Externally observable error class, timing budget, and response shape do not provide a practical trust-membership oracle |
| R-04 | Certificate, pairing message, collection, or declared length exceeds its cap | Reject before proportional allocation, hashing, UI work, or persistent write |
| R-05 | Ninth global or third per-source incomplete handshake arrives | Reject/busy within constant resource bounds; existing user attempt retains reserved capacity |
| R-06 | Handshake exceeds 5 seconds, is idle for 30 seconds, or pairing exceeds 120 seconds | Timeout closes connection, cancels work, erases pending state, and leaves durable trust unchanged |
| R-07 | One source continuously fails authentication | Bounded token bucket and jittered backoff engage without unbounded timer/log growth |
| R-08 | Many source addresses fail authentication | Global bucket and concurrency caps still bound CPU, memory, sockets, and UI prompts |
| R-09 | Peer sends 64 KiB plus one byte or a seventeenth pre-trust pairing message | Attempt closes at the ceiling with deterministic cleanup |
| R-10 | Slowloris fragments every allowed byte or TLS record | Absolute and idle timeouts cap lifetime and retained memory |
| R-11 | Malformed input triggers an error response loop | At most one bounded terminal response is attempted; connection then closes |
| R-12 | Cancellation, timeout, parser failure, and disconnect occur concurrently | One terminal state/callback; repeated cleanup is harmless; no callback-after-free |
| R-13 | Unauthenticated request asks for file scan, hash, resume lookup, or outbound fan-out | Request is rejected before filesystem/network side effects |
| R-14 | Packet capture observes an authenticated transfer | Payload and file metadata are unreadable, while tests explicitly record that endpoints/timing/volume remain visible |

## Completion criteria

The implementation owner must annotate every row with a test name and level.
Platform-specific skips require an issue and do not count as a pass. Fuzz
findings must be minimized into deterministic regression inputs.

Release evidence must include:

- cross-platform agreement on all golden vectors and malformed-vector
  rejection;
- an adversarial proxy test for MITM, replay, reorder, downgrade, and timeout;
- secure-storage fault injection at every durable transition;
- sanitizer and stateful-fuzz runs over pairing and transport state machines;
- proof that no test path enables production fallback, deterministic entropy,
  early data, or resumption.
