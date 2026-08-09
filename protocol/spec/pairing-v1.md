# XnnTransfer Pairing Control Protocol v1.0

Status: proposed normative specification.

This document registers the XnnTransfer v1 pairing-control wire contract. It
uses the cryptographic profile accepted by ADR 0002 and supplies the exact
identifiers, framing, state, limits, deadlines, errors, and replay behavior
required before a pairing session can be implemented.

This specification does not implement TLS, a session state machine, protected
storage, a C ABI, or a user interface. Conformance requires those independent
runtime and platform prerequisites.

The key words MUST, MUST NOT, REQUIRED, SHALL, SHALL NOT, SHOULD, SHOULD NOT,
RECOMMENDED, NOT RECOMMENDED, MAY, and OPTIONAL are to be interpreted as
described in BCP 14 when, and only when, they appear in all capitals.

## 1. Registered identifiers

All identifiers are immutable byte strings. They are not text subject to case
folding, normalization, a terminating zero, or locale processing.

| Purpose | ASCII | Length | Hexadecimal octets |
| --- | --- | ---: | --- |
| First-pairing ALPN | `xnn-transfer-pairing/1` | 22 | `786e6e2d7472616e736665722d70616972696e672f31` |
| Established-transport ALPN | `xnn-transfer/1` | 14 | `786e6e2d7472616e736665722f31` |
| Security profile | n/a | 2 | `0001` |

The TLS ALPN ProtocolNameList encoding of the first-pairing identifier is
`16 || 786e6e2d7472616e736665722d70616972696e672f31`. The established
identifier is
`0e || 786e6e2d7472616e736665722f31`. The leading octet is the identifier
length and is not part of the identifier.

A v1 endpoint offers exactly one of these ALPN values on a connection:

- `xnn-transfer-pairing/1` only while an explicit local pairing window is
  open and the connection is a new pairing attempt;
- `xnn-transfer/1` only for a peer with an active exact Ed25519 public-key pin.

The server selects that exact value or aborts the TLS handshake. It MUST NOT
select from a list containing both modes, retry with the other mode, or infer
mode from a certificate, address, discovery record, display name, or device
identifier. Absence, mismatch, or an unknown ALPN is terminal.

Discovery advertises one service port. The server TLS provider on that port
therefore owns a typed ALPN demultiplexer configured with exactly these two
allowed values. A client still offers exactly one value. The provider rejects
an empty, multi-entry, duplicate, or unknown client list and returns one typed
`pairing` or `established` channel mode before application dispatch. Session
code MUST NOT inspect ClientHello bytes itself or convert an arbitrary ALPN
byte string into authority.

For inbound established mode, the TLS provider extracts and validates the raw
Ed25519 key, resolves that exact key against the active pairing repository, and
requires an active exact pin before returning an established capability. It
MUST NOT return an unpinned established capability for later session code to
upgrade. In pairing mode, the provider returns a distinct unpinned capability
that can be consumed only by one bounded first-pairing attempt.

Security profile `0x0001` is exactly the ADR 0002 profile: TLS 1.3, fresh
X25519 ECDHE, Ed25519 identity proof, mandatory
`TLS_AES_128_GCM_SHA256`, optional
`TLS_CHACHA20_POLY1305_SHA256`, no early data, no tickets, and no PSK
resumption. No individual primitive is negotiated independently. A change to
any mandatory primitive requires a new profile identifier and compatibility
review.

## 2. Roles and trust boundary

For a first-pairing connection, the TLS client is the **initiator** and has
role octet `01`; the TLS server is the **responder** and has role octet `02`.
Roles never depend on address ordering, connection arrival order, discovery
metadata, or user-visible labels.

TLS authenticates possession of the Ed25519 keys carried by the two
self-signed identity certificates, but those keys remain untrusted until the
complete SAS ceremony and atomic local trust commit succeed. The certificate
key observed by TLS is an input to pairing state. A `HELLO.identity_key` MUST
equal that exact canonical 32-octet key for the sending endpoint.

Only the messages in section 5 may be parsed after the pairing ALPN has been
selected. No transfer, resume, file, path, storage, trust lookup, or outbound
fan-out operation may be dispatched from pairing-control input.

The application wire version and capabilities selected during pairing are the
same values defined by `v1.md` section 6. They are inputs to the ADR 0002
normalized-negotiation object; they do not authorize transfer traffic on the
pairing connection.

## 3. Pairing frame encoding

Pairing control uses one ordered TLS byte stream and the following fixed
20-octet header:

| Offset | Width | Field | v1.0 rule |
| ---: | ---: | --- | --- |
| 0 | 4 | `magic` | ASCII `XNNP` (`58 4e 4e 50`) |
| 4 | 2 | `header_length` | exactly 20 |
| 6 | 1 | `pairing_major` | exactly 1 |
| 7 | 1 | `pairing_minor` | exactly 0 |
| 8 | 2 | `message_type` | section 5 |
| 10 | 2 | `flags` | exactly zero |
| 12 | 4 | `message_sequence` | per-direction sequence |
| 16 | 4 | `body_length` | 0 through 4,096 |

All integers are unsigned big-endian. The complete frame length is exactly
`20 + body_length`. There are no header extensions in v1.0.

`message_sequence` starts at 1 independently in each direction and increments
by exactly one. Zero, a gap, duplicate, reorder, or wraparound is terminal
`PAIRING_SEQUENCE_VIOLATION`. A receiver validates the complete fixed header,
declared length, its inbound byte budget, and sequence before allocating or
waiting for a body. It does not drain an oversized declaration and does not
scan for `XNNP` after an error.

Bodies use the TLV record from `v1.md` section 5:

```text
field_id:U16 || wire_type:U8 || field_flags:U8 ||
value_length:U32 || value
```

Every defined field is required and has `CRITICAL` (`field_flags = 01`).
Fields occur once in strictly increasing field-ID order. Unknown fields,
duplicate fields, reserved flags or wire types, wrong types or lengths,
missing fields, out-of-order fields, truncation, and trailing octets are
terminal `PAIRING_MALFORMED`. A body contains at most 16 fields. Pairing v1.0
has no optional or ignorable field.

Composite `BYTES` values use these canonical encodings:

```text
version_range =
  min_major:U8 || min_minor:U8 || max_major:U8 || max_minor:U8

selected_version = major:U8 || minor:U8

capability_list =
  count:U16 || count * capability:U32

receive_limits =
  max_body:U32 || max_in_flight:U32 || max_streams:U16
```

Capability values are unique and numerically increasing. One capability ID
cannot occur at multiple versions. Counts and encoded lengths must agree
exactly. Receive limits and deterministic selection follow `v1.md` section 6.

## 4. Canonical pairing transcript

The pairing transcript has exactly these variable inputs in role order:

1. initiator role, nonce, identity key, security profile, version range,
   offered capabilities, required capabilities, and receive limits;
2. the same responder inputs;
3. the selected version, selected capabilities, and effective receive limits,
   acknowledged byte-for-byte by the responder.

After validating both `HELLO` messages and the matching selection, each
endpoint constructs ADR 0002 canonical object kind `01` from those values,
then kind `02` from the role-ordered nonces, certificate keys, profile
`0001`, and complete kind-`01` object.

Pairing v1.0 rejects every unknown or optional field, fixes message direction,
fixes each sequence number by state, and requires one canonical encoding for
each composite value. Therefore this mapping is one-to-one for every variable
accepted transcript value. The SHA-256 of the complete kind-`02` object is:

- the canonical transcript digest;
- `pair_context` for both ADR 0002 pairing exporters;
- the cryptographic identity of the live pairing attempt;
- the replay and terminal-tombstone key after the transcript is complete.

An implementation MUST compare the computed digest with the one attached to
the live native attempt before accepting a local decision. Neither a UI nor a
peer supplies `pair_context`, roles, identities, exporter output, or an
arbitrary transcript digest.

## 5. Message types and fields

| Type | Name | Direction | State effect |
| ---: | --- | --- | --- |
| `0x0001` | `PAIRING_HELLO` | both | contributes the sender's transcript inputs |
| `0x0002` | `PAIRING_SELECT` | initiator | proposes the only valid deterministic selection |
| `0x0003` | `PAIRING_SELECT_ACK` | responder | byte-exactly acknowledges the selection |
| `0x0004` | `PAIRING_DECISION` | both | authenticates confirm or reject for the live context |
| `0x0005` | `PAIRING_ABORT` | both | sends one bounded public terminal class |

### 5.1 `PAIRING_HELLO`

| ID | Name | Type | Constraint |
| ---: | --- | --- | --- |
| 1 | `sender_role` | `U8` | `01` initiator or `02` responder; must match TLS direction |
| 2 | `nonce` | `BYTES` | exactly 32 fresh OS-CSPRNG octets |
| 3 | `identity_key` | `BYTES` | exact canonical 32-octet certificate Ed25519 key |
| 4 | `security_profile` | `U16` | exactly `0001` |
| 5 | `version_range` | `BYTES` | canonical 4-octet application version range |
| 6 | `offered_capabilities` | `BYTES` | canonical capability list |
| 7 | `required_capabilities` | `BYTES` | canonical capability list and subset of field 6 |
| 8 | `receive_limits` | `BYTES` | canonical 10-octet limits |

Each endpoint sends exactly one `PAIRING_HELLO`. Both messages may cross on the
wire. A nonce MUST NOT repeat for the same local identity and MUST NOT come
from discovery, a clock, a counter, or deterministic test entropy in a
production build.

The identity key is validated under the ADR 0002 canonical-decode,
non-identity, prime-order-subgroup rule before context hashing. The profile,
role, and key are compared with the selected TLS mode and live certificate,
not accepted merely because their encodings are valid.

### 5.2 `PAIRING_SELECT` and `PAIRING_SELECT_ACK`

| ID | Name | Type | Constraint |
| ---: | --- | --- | --- |
| 1 | `selected_version` | `BYTES` | canonical 2-octet highest common application version |
| 2 | `selected_capabilities` | `BYTES` | exact sorted intersection satisfying both required sets |
| 3 | `effective_receive_limits` | `BYTES` | component-wise minimum of both advertisements |

The initiator sends `PAIRING_SELECT` only after both hellos are validated. A
lower common version is `PAIRING_DOWNGRADE_DETECTED`; a selected capability
set or limit different from the deterministic result is
`PAIRING_MALFORMED`.

The responder sends one `PAIRING_SELECT_ACK` with exactly the same three value
octet strings. A mismatch is terminal. No alternative selection exchange or
retry occurs on the connection.

### 5.3 `PAIRING_DECISION`

| ID | Name | Type | Constraint |
| ---: | --- | --- | --- |
| 1 | `confirmation_message` | `BYTES` | exactly 34 octets: live `pair_context[32] || sender_role:U8 || decision:U8` |
| 2 | `confirmation` | `BYTES` | exactly 32 HMAC octets |

The confirmation is the ADR 0002 value:

```text
HMAC-SHA256(
  confirmation_exporter,
  pair_context || sender_role:U8 || decision:U8)
```

Only the native attempt state supplies the expected role, context, and
exporter capability. The receiver first requires the exact 34-octet message,
then separately compares its context and role with those live values before
constant-time HMAC verification. A valid `00` is terminal
`PAIRING_AUTHENTICATED_REJECT`, never success. A `01` records one affirmative
peer decision but cannot write trust until the local decision for the same
live attempt is also `01`.

An endpoint sends its decision at most once. A peer decision may arrive before
the local user decides and may be retained as one bounded value for this
attempt. It cannot suppress the local ceremony or extend a deadline.

### 5.4 `PAIRING_ABORT`

| ID | Name | Type | Constraint |
| ---: | --- | --- | --- |
| 1 | `public_code` | `U16` | one value from the table below |

| Value | Name | Meaning visible to the peer |
| ---: | --- | --- |
| `0x0001` | `FAILED` | generic protocol, authentication, profile, policy, or internal failure |
| `0x0002` | `BUSY` | bounded admission capacity is unavailable |
| `0x0003` | `CANCELLED` | local cancellation before an affirmative trust commit |
| `0x0004` | `TIMEOUT` | a public pairing deadline expired |

There is no text, retry delay, peer identifier, fingerprint, trust state, or
offending value. A valid authenticated rejection uses
`PAIRING_DECISION(decision = 00)`, not an abort.

At most one abort may be attempted after a well-formed pairing TLS channel
exists. A TLS, certificate, exporter, framing, or parser failure may close
without an abort when sending would create an oracle or amplification. Receipt
of an abort is terminal and never elicits another abort.

## 6. State machine

```text
PAIRING_TLS
    | exact ALPN, certificate profile, identity proof
    v
EXCHANGING_HELLOS
    | both role-correct HELLO messages
    v
SELECTING
    | initiator SELECT, responder identical ACK
    v
AWAITING_DECISIONS
    | local 01 sent and authenticated peer 01 received
    v
READY_TO_COMMIT
    | invoke one protected-store atomic write
    v
COMMITTING
    | atomic local trust write succeeds
    v
PAIRED_LOCAL -> CLOSED

PAIRING_TLS / EXCHANGING_HELLOS / SELECTING / AWAITING_DECISIONS /
READY_TO_COMMIT
    | reject / abort / cancel / timeout / failure
    v
CLOSED
```

`PAIRING_TLS` admits no application frame until the exact pairing ALPN,
certificate constraints, TLS profile, and identity proof succeed.

`EXCHANGING_HELLOS` accepts one role-correct hello in each direction.
`SELECTING` accepts only the initiator selection followed by the responder's
identical acknowledgement. SAS material becomes available only after the ACK,
the canonical transcript digest, and both exporters are available.

`AWAITING_DECISIONS` accepts one decision per direction. The local UI may
confirm or reject only through a locally generated, random 128-bit opaque
native attempt handle. The handle never appears on the wire and binds the
pairing-window generation, connection generation, both roles and certificate
keys, `pair_context`, and absolute deadline. A stale, unknown, terminal, or
different handle has no effect on the current attempt.

`READY_TO_COMMIT` is entered only after the local affirmative decision frame's
TLS write completes successfully and the peer's affirmative decision has been
authenticated for the same context. Reaching this state is not pairing success
and does not expose trust to another connection.

`COMMITTING` invokes exactly one protected-store compare-and-swap for the local
peer record. Its success is the local trust linearization point. A write
failure is terminal `PAIRING_INTERNAL_FAILURE` and closes unpaired. The record
contains the exact peer key, derived device identifier, approved profile floor
`0001`, and local metadata required by ADR 0002.

`PAIRED_LOCAL` means only that this endpoint's atomic write succeeded. The
pairing connection closes and is never promoted to a transfer connection. A
crash may leave the endpoints asymmetric. A later connection must independently
pass the established reconnect rules below; an unpaired side does not silently
create, repair, or replace trust.

Every error and timeout transition is terminal. Cleanup is idempotent and
invalidates the attempt handle before publishing the terminal result. No
network bytes, timer, callback, or repeated local action may leave `CLOSED`.

## 7. Established reconnect

A paired reconnect is a fresh TLS 1.3 connection with:

1. only the established ALPN `xnn-transfer/1`;
2. a fresh X25519 ECDHE handshake with no ticket, PSK, or early data;
3. mutual Ed25519 proof;
4. an active local pairing record whose exact peer key equals the live
   certificate key;
5. selected profile `0001` at or above the record's stored security floor.

Only after those checks may `v1.md` begin its `HELLO`, negotiation, and
transport-finished state machine. The channel becomes established only after
the deterministic `HELLO`/`NEGOTIATE`/`NEGOTIATE_ACK` exchange and both
role-specific `TRANSPORT_FINISHED` conditions succeed. No pairing-control frame
is legal under the established ALPN, and no `XNNT` frame is legal under the
pairing ALPN.

An absent, revoked, corrupt, lower-profile, or different pin produces the same
externally observable TLS failure class. It never retries with the pairing
ALPN. Re-pairing requires an explicit local `forget and pair again` action, a
new pairing window, fresh nonces, and the complete SAS ceremony.

## 8. Duplicates, replay, and races

TLS is ordered and reliable, so v1.0 does not retransmit pairing frames on one
connection.

- A repeated, reordered, or skipped frame sequence is terminal even when the
  bytes are identical.
- A second message of a type already consumed in that role and state is
  terminal even when it carries a new valid sequence number.
- A second identical local decision call returns the cached local progress or
  terminal result without sending another frame. A conflicting second decision
  is `PAIRING_ALREADY_DECIDED` and does not change the first result.
- A captured decision on another connection fails because fresh nonces produce
  a different `pair_context` and the fresh TLS handshake produces a different
  confirmation exporter.
- A terminal `pair_context` is retained in a bounded in-memory replay set for
  600 seconds. The set holds at most 1,024 entries globally and 32 entries per
  remote canonical identity key. Expired entries are removed first. An
  implementation MUST reject new admission rather than evict an unexpired
  entry to make room.
- Replay state is not authority after restart. A fresh TLS handshake, fresh
  nonces, exporter binding, explicit local decision, and atomic trust rules
  still apply after process restart.

When local rejection, peer rejection, cancellation, timeout, parser failure,
disconnect, and shutdown race, the first event serialized by the attempt's
native executor selects one terminal result. Cleanup and later events are
idempotent. No rejection or cancellation may delete a trust record whose
atomic commit completed first; conversely, no commit may begin after a
terminal event won.

If the same two validated identity keys create crossed pairing connections,
both endpoints retain the connection whose lexicographically smaller
32-octet key is the initiator key. For duplicate connections in the same
direction, the responder retains its first admitted connection and rejects
later duplicates. Once the retained connection validates its selection ACK,
the winner freezes; a later connection can never replace a displayed attempt.
Every loser closes before SAS display. Equal local and remote identity keys are
invalid. Connections involving different key pairs do not participate in this
tie-break and cannot displace the one locally selected candidate or the
reserved user-initiated capacity. At most one attempt reaches a visible prompt.

Sending an affirmative decision is irreversible for that attempt; it cannot
later be changed to rejection. A serialized local cancellation observed before
the protected-store operation is invoked closes locally unpaired, even when an
affirmative decision was already sent. Once the operation is invoked, terminal
publication waits for its completion and cancellation cannot win: durable
success reports `PAIRED_LOCAL`; failure reports `PAIRING_INTERNAL_FAILURE`
without trust. The peer may already have committed, which is the explicitly
allowed asymmetric result. A cancellation observed after the local atomic
write cannot roll back or delete that record.

## 9. Resource ceilings and deadlines

Every value in this table is a hard v1.0 maximum. Local policy may lower it.
Limits are checked before proportional allocation, hashing, UI work, or
persistent access.

| Resource | Hard maximum |
| --- | ---: |
| Peer leaf certificate DER | 4,096 octets |
| TLS 1.3 peer `certificate_list` contents | 8,192 octets, excluding its 3-octet vector length |
| Complete TLS 1.3 peer `Certificate` handshake message | 8,200 octets: 4-octet handshake header, empty 1-octet request context, 3-octet list length, and list contents |
| Peer certificate request context | exactly zero octets |
| Peer certificate chain | exactly 1 certificate |
| Canonical Ed25519 identity key | exactly 32 octets |
| Incomplete pre-trust TLS handshakes | 8 process-wide, 2 per source address |
| Pairing attempts reaching a visible SAS prompt | 1 process-wide |
| Pairing frame body | 4,096 octets |
| Complete pairing frame | 4,116 octets |
| TLV fields in one pairing body | 16 |
| Received pairing-control frames | 16 from the peer at each endpoint |
| Received pairing-control bytes | 65,536 from the peer at each endpoint, including frame headers |
| Terminal replay entries | 1,024 global and 32 per remote key, retained for 600 seconds |

The process-wide handshake limiter uses a token bucket of capacity 16,
refilling one token per second. The per-source limiter has capacity 4,
refilling one token every 15 seconds. Admission consumes one token from each
bucket before TLS work. Source addresses are resource-accounting hints only;
they never identify or authenticate a peer. A user-initiated outgoing attempt
reserves capacity but does not bypass the process-wide hard concurrency bound.

All deadlines use a monotonic clock:

| Timer | v1.0 deadline and action |
| --- | --- |
| Pairing window | at most 120 seconds from explicit local opening |
| TLS handshake | 5 seconds from admission |
| First pairing frame | 5 seconds after TLS completion |
| HELLO through matching selection ACK | 10 seconds from first valid HELLO |
| One frame assembly | 10 seconds from its first header octet |
| Pairing-control idle | 30 seconds without a complete valid state-advancing frame |
| SAS decision | 90 seconds from matching selection ACK |
| Complete pairing attempt | 120 seconds from handshake admission and never later than window expiry |
| Terminal abort flush | at most 1 second, then close |

Partial, malformed, duplicate, out-of-state, or authentication-failing input
does not reset a deadline. A local prompt, peer decision, retry, UI reconnect,
or abort flush does not extend the absolute attempt or pairing-window
deadline. Expiry invalidates the native attempt handle, erases pending
material, releases admission capacity, and leaves durable trust unchanged.

The frame and byte ceilings are inbound budgets at each endpoint. An
initiator-to-responder vector exercises the responder's budget and a
responder-to-initiator vector exercises the initiator's budget. They are not
summed into a fictitious third observer, and neither endpoint may receive twice
the stated limit.

Network saturation remains outside the availability guarantee.

## 10. Local terminal results and wire privacy

The native session implementation exposes these stable machine-readable
terminal results to trusted local application code:

| Value | Name |
| ---: | --- |
| `0x1001` | `PAIRING_MALFORMED` |
| `0x1002` | `PAIRING_LIMIT_EXCEEDED` |
| `0x1003` | `PAIRING_SEQUENCE_VIOLATION` |
| `0x1004` | `PAIRING_UNSUPPORTED_VERSION` |
| `0x1005` | `PAIRING_UNSUPPORTED_PROFILE` |
| `0x1006` | `PAIRING_DOWNGRADE_DETECTED` |
| `0x1007` | `PAIRING_ROLE_MISMATCH` |
| `0x1008` | `PAIRING_STATE_VIOLATION` |
| `0x1009` | `PAIRING_REPLAY_DETECTED` |
| `0x100a` | `PAIRING_CONFIRMATION_FAILED` |
| `0x100b` | `PAIRING_AUTHENTICATED_REJECT` |
| `0x100c` | `PAIRING_LOCAL_REJECT` |
| `0x100d` | `PAIRING_ALREADY_DECIDED` |
| `0x100e` | `PAIRING_CANCELLED` |
| `0x100f` | `PAIRING_TIMEOUT` |
| `0x1010` | `PAIRING_BUSY` |
| `0x1011` | `PAIRING_CERTIFICATE_REJECTED` |
| `0x1012` | `PAIRING_INTERNAL_FAILURE` |

All are terminal for the affected attempt except
`PAIRING_ALREADY_DECIDED`, which reports an ignored conflicting local action
without changing the already selected state.

Detailed values are never serialized directly. Before trust exists, local
pin presence, revocation, storage state, certificate details, profile floor,
SAS material, identities, and parser details map to a TLS close or one public
abort class from section 5.4. Timing and response size follow the same bounded
path for known, unknown, paired, revoked, and malformed peer keys.

Logs and telemetry record only bounded local result values and opaque local
operation correlation. They MUST NOT record nonces, exporter output,
confirmations, SAS words, identity keys, device identifiers, certificate
bytes, pair contexts, addresses combined with trust state, or untrusted peer
text.

## 11. Compatibility

- Pairing ALPN, established ALPN, profile `0001`, `XNNP` magic, frame fields,
  message IDs, public abort values, and local terminal values are permanently
  registered and MUST NOT be reinterpreted or reused.
- Pairing v1.0 accepts exactly major 1, minor 0. A future compatible minor must
  first define how its fields are transcript-bound; v1.0 rejects them rather
  than skipping unknown data.
- A future pairing major uses a distinct ALPN identifier. There is no in-band
  retry, downgrade, or plaintext upgrade.
- A new security profile uses a new U16 value. Automatic selection below a
  stored profile floor is forbidden. A lower profile requires explicit forget,
  re-pairing, and a new user-confirmed ceremony.
- Numeric application versions and capabilities retain the compatibility rules
  in `v1.md`. Pairing selection cannot assign them new meanings.
- Persisted trust records store the profile identifier and exact canonical peer
  key, not ALPN text, certificate bytes, discovery metadata, or an address.

Silent reinterpretation of any registered byte is a protocol violation.

## 12. Golden vectors

Normative positive and hostile vectors are in
`protocol/testdata/security/v1/pairing-control-vectors.json`. They cover:

- first pairing with either decision arrival order followed by atomic local
  commit success;
- commit failure, cancellation before commit invocation, and a duplicate
  decision after both confirmations;
- authenticated rejection;
- cancellation and invalid public abort codes;
- established reconnect under the transport ALPN, exact pin, stored profile
  floor, fresh handshake, canonical transport context, and both finished
  values;
- malformed, duplicate, replayed, role-swapped, oversized, unknown-profile,
  downgrade, missing, reordered, unknown-field, and trailing input.

Run:

```bash
python3 protocol/testdata/security/v1/validate_vectors.py
```

The validator is a deterministic fixture oracle. It does not create a TLS
connection, enforce runtime timers, access protected storage, or implement the
production session.
