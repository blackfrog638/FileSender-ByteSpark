# XnnTransfer LAN Discovery Protocol v1.0

Status: accepted normative contract for XT-019. No discovery runtime is
implemented by this document.

This document specifies the unauthenticated UDP advertisement used to find an
XnnTransfer connection endpoint on one LAN link. Discovery produces an
untrusted, short-lived candidate only. It never authenticates a device,
restores a pairing, authorizes a connection, negotiates the transfer protocol,
or accepts a file.

The key words MUST, MUST NOT, REQUIRED, SHALL, SHALL NOT, SHOULD, SHOULD NOT,
RECOMMENDED, NOT RECOMMENDED, MAY, and OPTIONAL are to be interpreted as
described in BCP 14 when, and only when, they appear in all capitals.

## 1. Scope and terminology

- A **publisher scope** is one active local interface generation and one IP
  address family. It owns one random instance token and sequence.
- An **observer scope** is the receiving interface generation and address
  family reported by packet-info metadata.
- A **candidate** is an untrusted source address, advertised service port,
  optional display label, and expiry owned by one observer scope.
- An **interface generation** is a receiver-local opaque identifier. Removing
  and re-adding an interface creates a new generation even if the OS reuses an
  interface index.
- A **tombstone** is bounded volatile state containing a candidate key and its
  highest accepted sequence. It prevents reordered or recently replayed
  datagrams from recreating an expired candidate.
- `receive_time` and all deadlines use one monotonic clock. `wall clock` is not
  used by this protocol.

The discovery service is not DNS, mDNS, a directory, or a security protocol.
It sends no response to an advertisement and performs no automatic connection.

## 2. UDP endpoints

All discovery datagrams use UDP destination port `45878`.

| Family | Destination group | Required sender setting |
| --- | --- | --- |
| IPv4 | `239.255.88.78` | IP multicast TTL 1 |
| IPv6 | `ff12::584e:4e44` | multicast hop limit 1 |

The IPv4 group is administratively scoped. The IPv6 group is transient and
link-local scoped. Routers MUST NOT forward either advertisement beyond the
originating link. A sender joins and transmits separately for every eligible
interface and address family. A receiver MUST request destination-address and
receiving-interface packet metadata and accepts a datagram only when:

1. its destination is the group for its address family;
2. its destination port is `45878`;
3. its observer scope is currently eligible; and
4. its source is a unicast, non-unspecified, non-multicast, non-loopback
   address that is not an IPv4 limited or interface-directed broadcast and is
   not an IPv4-mapped IPv6 address.

An OS truncation or message-too-large indication rejects the datagram even when
the returned prefix is 512 octets. The receive path MUST distinguish an exact
512-octet datagram from a larger truncated datagram.

IPv4 link-local and IPv6 link-local sources are allowed. An IPv6 link-local
endpoint retains the observer interface scope. The UDP source port is ignored.
The connection endpoint is the observed source address plus the service port
inside the datagram. There is no advertised address field.

Senders MUST keep multicast loopback enabled so the same host behavior is
testable. Section 8 removes self advertisements.

## 3. Datagram envelope

One UDP payload contains exactly one discovery message. Integers are unsigned
network-byte-order values. Implementations MUST parse integers from octets and
MUST NOT copy a native structure from the wire.

The maximum UDP payload is 512 octets. The fixed header is 44 octets:

| Offset | Width | Field | v1.0 rule |
| ---: | ---: | --- | --- |
| 0 | 4 | `magic` | ASCII `XNND` (`58 4e 4e 44`) |
| 4 | 1 | `version_major` | 1 |
| 5 | 1 | `version_minor` | 0 |
| 6 | 1 | `message_type` | section 4 |
| 7 | 1 | `flags` | zero |
| 8 | 2 | `total_length` | 44 through 512; exact UDP payload length |
| 10 | 2 | `header_length` | 44 |
| 12 | 8 | `sequence` | 1 through `2^64 - 1` |
| 20 | 16 | `instance_token` | nonzero opaque random octets |
| 36 | 2 | `service_port` | section 4 |
| 38 | 2 | `ttl_seconds` | section 4 |
| 40 | 2 | `tlv_length` | `total_length - header_length` |
| 42 | 2 | `reserved` | zero |

There is no padding, checksum, compression, signature, encryption, or trailing
data. UDP and IP checksums do not authenticate an advertisement.

### 3.1 Instance token and sequence

Each publisher scope generates a uniformly random 128-bit `instance_token`
from the OS CSPRNG at startup, after wake, and whenever an interface generation
is added. A publisher also rotates each live token after no more than 15
minutes. Tokens MUST NOT contain a stable device identifier, identity-key
derivative, account identifier, persisted secret, MAC address, or hostname.
The all-zero token is invalid.

The first datagram for a token has sequence 1. Every later datagram, including
a withdrawal, increments the sequence by one. A publisher rotates to a new
token before sequence overflow and never persists either value. Different
publisher scopes use independent tokens.

The token and sequence provide only duplicate and reordering scope. Anyone on
the LAN can copy or forge them.

## 4. Message types

| Value | Name | Required fields |
| ---: | --- | --- |
| 1 | `ANNOUNCE` | `service_port` 1..65535; `ttl_seconds` 5..60 |
| 2 | `WITHDRAW` | `service_port`, `ttl_seconds`, and `tlv_length` all zero |

All other message types are invalid in v1.0.

An `ANNOUNCE` says only that an XnnTransfer service may be listening at the
observed source address and `service_port`. It does not claim the endpoint is
reachable, compatible, paired, or owned by the displayed device.

A publisher sends `WITHDRAW` best-effort before graceful stop, before removing
an eligible publisher scope, or before rotating a still-usable token. Loss,
suspend, and abrupt interface removal may prevent transmission; receivers
therefore always enforce expiry.

## 5. TLV encoding

The bytes after `header_length` are zero or more TLVs:

| Offset within TLV | Width | Field |
| ---: | ---: | --- |
| 0 | 2 | `type` |
| 2 | 2 | `length` |
| 4 | `length` | `value` |

The high bit of `type` is the critical bit. The remaining 15 bits are the
field ID. Field ID zero is reserved. TLVs MUST be in strictly increasing field
ID order and an ID MUST occur at most once, regardless of critical-bit value.
A datagram contains at most 32 TLVs. A TLV header or value extending beyond
`tlv_length` is invalid.

An unknown critical TLV rejects the whole datagram. An unknown noncritical TLV
is included in length and ordering checks, then ignored without allocation or
exposure to the cache, UI, log, pairing layer, or connection layer. Registered
field IDs are never reused.

### 5.1 Display label

`DISPLAY_LABEL` has type `0x0001`, length 1 through 96 octets, and a UTF-8
value containing at most 64 Unicode scalar values.

The value MUST:

- use shortest-form, well-formed UTF-8;
- already be Unicode NFC;
- contain no U+0000, surrogate, Unicode noncharacter, line/paragraph
  separator, or code point whose general category starts with `C`;
- begin and end with a non-whitespace scalar.

The field is optional and disabled by default. A publisher includes it only
after the local user enables LAN name disclosure. Receivers treat it as
untrusted display text, escape it for the presentation context, and never use
it as an identity, lookup key, pin, filename, path, log field, or authorization
input.

No other field is registered in v1.0.

## 6. Canonical validation

A receiver applies these checks in order. Every failure silently drops the
datagram and leaves candidates, tombstones, pairing state, storage, and UI
unchanged.

1. Validate source, destination, port, and observer-scope packet metadata from
   section 2.
2. Consume the global, observer-scope, and source-address rate buckets from
   section 10. Malformed traffic consumes budget.
3. Reject an OS truncation indication, then validate payload length 44..512,
   `magic`, exact version 1.0, `total_length`, `header_length`, `flags`,
   `reserved`, and `tlv_length` before parsing any TLV.
4. Validate the message type, nonzero token, nonzero sequence, and the
   type-specific fields in section 4.
5. Scan TLVs once with checked offsets. Validate ordering, uniqueness,
   critical fields, and known-field values before copying a value.
6. Apply self-filtering and the cache transition in sections 8 and 9.

The parser may use one fixed 512-octet receive buffer. Before cache insertion,
it may retain at most the fixed decoded fields, one 96-octet label, and bounded
parser bookkeeping. Unknown TLVs MUST NOT cause a value-sized allocation.
Length arithmetic is checked before addition or slicing.

Receivers do not send a rejection, ICMP response intentionally, protocol
error, connection attempt, notification, or log line per invalid datagram.
Local aggregate diagnostics MAY count bounded reason classes without retaining
source text or payload bytes.

## 7. Sender lifecycle

An eligible publisher scope sends:

1. one `ANNOUNCE` after joining the group;
2. one new-sequence `ANNOUNCE` every 5 seconds, with independent CSPRNG-derived
   jitter uniformly bounded to plus or minus 500 milliseconds;
3. one immediate new-sequence `ANNOUNCE` after a service-port or enabled-label
   change, subject to at most one transmission per publisher scope per second;
4. one new-sequence `WITHDRAW`, followed by a fresh-token sequence-1
   `ANNOUNCE`, when the token reaches 15 minutes; and
5. one new-sequence `WITHDRAW` on graceful scope removal or shutdown when the
   interface is still usable.

The required advertised TTL is 15 seconds. The wider 5..60-second receive
range permits a future operational profile without changing the v1.0 parser;
v1.0 publishers use 15.

Startup, wake, interface addition, and address-family eligibility changes take
a fresh bounded interface snapshot before joining. Wake destroys old publisher
scopes, rotates their tokens, rejoins current groups, and sends fresh
announcements. A publisher MUST NOT burst historical announcements after
wake.

## 8. Candidate identity and self-filtering

The candidate key is the byte-exact tuple:

```text
(observer interface generation,
 address family,
 observed source address bytes,
 instance_token)
```

The label, service port, UDP source port, hostname, and any TLS or transfer
value are not key components.

The service keeps the active token for each local publisher scope. A received
datagram is self-originated only when its observer interface generation,
address family, and token match an active local publisher scope. Such a
datagram is dropped without a candidate. An address or label match alone does
not establish self-origin.

Token collision or spoofing can hide or create only an unauthenticated
candidate. It cannot affect pairing records or authenticated connections.

## 9. Candidate cache state machine

All cache transitions are serialized on one discovery executor. `highest` is
the greatest sequence accepted for a key. `raw` is the complete canonical
payload accepted at `highest`.

### 9.1 Announcement

For a valid `ANNOUNCE` at monotonic time `now`:

- No entry: insert a candidate if section 10 capacity permits.
- Candidate and `sequence < highest`: drop as stale; do not refresh.
- Candidate and `sequence == highest` with byte-identical `raw`: drop as an
  exact duplicate; do not refresh.
- Candidate and `sequence == highest` with different bytes: drop as a sequence
  conflict; do not refresh.
- Candidate and `sequence > highest`: replace visible fields and `raw`, set
  `highest`, and refresh the deadline.
- Tombstone and `sequence <= highest`: drop; do not recreate the candidate.
- Tombstone and `sequence > highest`: replace it with a candidate if candidate
  capacity permits. If capacity is unavailable, retain the tombstone and its
  previous `highest`.

An accepted candidate deadline is exactly:

```text
deadline = now + ttl_seconds * 1000 monotonic milliseconds
```

Insertion publishes `appeared`. A newer announcement publishes `updated` only
if the service port or display label changed; a pure lease refresh changes no
observable peer event.

### 9.2 Withdrawal

A valid `WITHDRAW` uses the same key:

- A candidate is removed only when `sequence > highest`; publish `expired`
  with reason `withdrawn`, then retain a tombstone at that sequence.
- A tombstone advances only when `sequence > highest`.
- With no entry, insert a tombstone if entry capacity permits.
- An equal or lower sequence changes nothing.

This permits a withdrawal received before an older announcement to suppress
that reordered announcement.

### 9.3 Expiry and tombstones

At the first serialized timer step where `now >= deadline`, remove the
candidate, publish `expired` with reason `ttl`, and retain a tombstone with the
same `highest`. A tombstone expires exactly 60 seconds after its creation or
latest accepted withdrawal. An expired tombstone is purged before processing
new input.

Tombstones provide bounded reordering and replay suppression, not
authentication. After their bounded retention or a process restart, a replay
can at most create another untrusted candidate until TTL expiry. It cannot
restore a pairing, authenticated endpoint, connection, user selection, or
trust state.

### 9.4 Interface removal and wake

Removing an observer interface generation immediately publishes `expired`
with reason `interface_removed` for its candidates, then deletes all candidate
and tombstone state for that generation. No later callback from the removed
generation may mutate the cache. Re-addition creates a new generation.

On wake, the service:

1. expires every candidate with reason `wake`;
2. converts those candidates to tombstones retained for 60 seconds;
3. takes a fresh bounded interface snapshot;
4. removes state for absent generations;
5. rejoins groups and resumes receive operations; and
6. rotates publisher tokens as required by section 7.

This rule is independent of whether the platform monotonic clock advanced
during suspend. A missed interface notification is repaired by the same
snapshot process.

Stopping discovery is a barrier. It cancels sockets and timers, clears cache
state without publishing new peer events after the barrier, and prevents late
receive, interface, or timer completions from mutating state.

## 10. Resource and rate limits

These are hard v1.0 receiver ceilings. Implementations MAY apply lower local
limits, but MUST apply them deterministically and MUST NOT present a dropped
candidate as trusted.

| Resource | Limit |
| --- | ---: |
| UDP payload | 512 octets |
| Display label | 96 octets and 64 scalars |
| TLVs per datagram | 32 |
| Eligible observer/publisher scopes | 32 per engine |
| Active candidates | 256 global, 64 per observer scope |
| Candidate plus tombstone entries | 512 global, 128 per observer scope |
| Retained candidate payload bytes | 131,072 global |
| Per-source rate-bucket entries | 1,024 global, 128 per observer scope |
| Idle per-source bucket lifetime | 60 seconds |
| Candidate TTL | 5 through 60 seconds |
| Tombstone retention | 60 seconds |
| Global receive bucket | 256 datagrams/second, burst 512 |
| Per-observer-scope receive bucket | 128 datagrams/second, burst 256 |
| Per-source-address receive bucket | 8 datagrams/second, burst 16 |

Rate buckets use a monotonic token bucket. Tokens refill continuously at the
listed rate up to the burst. A packet consumes one token from all three
buckets atomically; if any bucket lacks a token, it is silently dropped and no
bucket is consumed. The source key is observer scope plus byte-exact source
address, not a claimed token or label.

Expired per-source buckets are purged before creating one. If the global or
observer-scope bucket table is full, traffic from a source without an existing
bucket is silently dropped. Existing bucket state is not evicted to admit a
new source. The eligible-scope snapshot uses a stable local ordering when more
than 32 scopes exist; ignored scopes publish and observe nothing until a new
snapshot admits them.

Expired entries are purged before a capacity check. At a candidate limit, a
new candidate is dropped; an existing candidate may still receive a valid
newer update. At an entry limit, an unknown candidate or withdrawal is
dropped. Live entries are never evicted merely to admit attacker-controlled
new keys. Limits do not trigger persistent writes, automatic connections,
repeated logs, or UI prompts.

## 11. Privacy and security boundary

An advertisement MUST NOT contain, directly or by derivative:

- an Ed25519 public key, fingerprint, certificate, stable device ID, account
  identifier, pairing-record identifier, trust state, or revocation state;
- file name, path, size, hash, manifest, destination, transfer ID, transfer
  intent, resume state, or acceptance state;
- a MAC address, persisted installation secret, or hostname unless the user
  deliberately uses that text as the optional display label.

The source address, timing, service presence, random token, service port, and
user-enabled label remain visible to the LAN. Encryption is intentionally not
provided because no authenticated key exists at discovery time.

Cloned labels, tokens, source addresses, and ports do not clone an identity.
After explicit local candidate selection, ADR 0002 obtains identity from TLS
proof of possession and authenticates first contact with the SAS. A known
pairing still requires the exact stored Ed25519 pin. Discovery never bypasses
either rule.

Receivers expose candidates as hostile display data. They do not persist
discovery state, restore it after restart, automatically open a pairing
window, initiate more than an explicitly bounded user-requested connection,
or infer that a previously seen endpoint is the same device.

## 12. Compatibility

The envelope version is exactly 1.0. Unknown major or minor versions are
silently dropped and do not create or refresh a candidate. A future sender
that needs v1.0 visibility emits a separate canonical v1.0 datagram; it does
not rely on an old receiver partially interpreting a newer envelope.

Within v1.0, optional noncritical TLVs may be assigned only when old receivers
can safely ignore them. A field affecting endpoint construction, parsing,
resource use, trust, security policy, or required behavior is critical or
requires a new envelope version. Existing field IDs and semantics are never
reused.

Hard limits cannot be raised for v1.0. Lower local cache or rate limits affect
availability only and do not change wire validity.

## 13. Golden vectors and implementation status

Byte-exact datagrams and deterministic cache scenarios are under
`protocol/testdata/discovery/v1/`. Run:

```bash
python3 protocol/testdata/discovery/v1/validate_vectors.py
```

The oracle validates encoding, canonical rejection, source and destination
metadata, duplicate and conflict behavior, expiry calculations, withdrawals,
tombstones, interface removal, wake, self-filtering, capacity, and rate
limits. Fixture source addresses and labels are synthetic.

This specification and its fixtures do not open sockets, monitor interfaces,
authenticate peers, or prove multicast interoperability. XT-020 owns the
native runtime and platform evidence. No component may claim LAN discovery
behavior from XT-019 alone.
