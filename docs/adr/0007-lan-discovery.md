# ADR 0007: Unauthenticated LAN discovery v1

- Status: accepted
- Date: 2026-08-07
- Accepted by task: XT-019
- Security profile: `docs/adr/0002-pairing-and-transport-security.md`

## Context

P1 needs peers to learn reachable LAN endpoints while treating every discovery
packet, name, address, and capability as attacker-controlled. Discovery must
handle multiple interfaces, address churn, sleep/wake, duplicates, expiry, and
bounded denial-of-service behavior before a native socket parser is added.

## Decision

Use the byte-exact v1.0 contract in
`protocol/spec/discovery-v1.md`. A publisher sends a 512-octet-or-smaller UDP
datagram to one administratively or link-local scoped multicast group per
eligible interface and address family. The envelope carries:

- an interface-scoped random 128-bit instance token;
- a monotonically increasing sequence scoped to that token;
- a short lease;
- the service port; and
- an optional, user-enabled display label.

The endpoint address comes only from the observed UDP source address and
receiving-interface metadata. The wire format has no address, identity,
fingerprint, trust, file, transfer, or capability field.

The candidate key includes observer interface generation, address family,
source address, and instance token. A higher sequence refreshes or updates the
candidate. Equal byte-identical or lower sequences do not refresh its
monotonic deadline. Equal sequences with different bytes are conflicts and
also do not refresh. Expiry and withdrawal leave a bounded volatile tombstone
so reordered or recently replayed sequences cannot immediately recreate a
candidate.

Interface removal immediately removes state for that interface generation.
Wake expires visible candidates independently of platform clock behavior,
rescans interfaces, rejoins multicast groups, and rotates publisher tokens.
Stopping discovery is a barrier against later socket, timer, or interface
callbacks.

The receiver applies fixed global, per-interface, and per-source token buckets,
a 256-candidate ceiling, a 512-entry candidate-plus-tombstone ceiling, and a
fixed 512-octet parse budget. At capacity it rejects new attacker-controlled
keys instead of evicting live candidates. Malformed, unsupported, stale,
self-originated, rate-limited, and over-capacity datagrams create no candidate
and trigger no response.

## Trust and privacy boundary

Discovery remains unauthenticated. A candidate is a reachability and display
hint only. A label, token, address, port, duplicate history, or prior
observation cannot:

- create, restore, select, rotate, or raise trust;
- prove identity-key possession;
- replace first-contact SAS comparison or an exact stored pin;
- start pairing without a current local user action;
- authorize a transfer or disclose file metadata; or
- survive as persisted discovery state.

Random tokens are deliberately short-lived and never derived from a stable
device value. The display label is absent by default because enabling it
reveals user-selected text to the LAN. Source addresses, timing, the service
port, and service presence remain observable.

Bounded tombstones suppress ordinary reordering and recent replay. They do not
make an unauthenticated datagram replay-proof. After tombstone expiry or
process restart, replay can create only another untrusted, short-lived
candidate; it cannot restore an authenticated endpoint, pairing attempt, or
trust state.

## Compatibility

Receivers accept exactly envelope version 1.0. Unknown versions, messages,
flags, critical TLVs, and malformed canonical values fail closed without peer
creation. Unknown noncritical TLVs are length-checked and skipped without
allocation or exposure. A future sender that requires v1.0 visibility emits a
separate canonical v1.0 datagram.

Changing endpoint construction, trust meaning, fixed-header semantics, or hard
resource ceilings requires a new reviewed protocol version. Adding an
optional noncritical hint is compatible only when v1.0 receivers can ignore it
without changing any security or resource decision.

## Evidence and implementation boundary

`protocol/testdata/discovery/v1/` contains positive, malformed, spoofing, rate,
duplicate, expiry, interface, wake, and tombstone vectors. Its standard-library
oracle is deterministic and performs no network I/O.

This ADR accepts a public wire and lifecycle contract only. Native multicast
sockets, interface monitors, timers, cache events, and platform
interoperability remain unimplemented until XT-020. The C ABI and Flutter
surface remain unimplemented until XT-021.

## Consequences

- Source-address-derived endpoints avoid an attacker-selected address field,
  but LAN source spoofing and connection races remain possible.
- Per-interface tokens reduce stable cross-link correlation, but timing, label,
  and source addresses can still correlate a publisher.
- Not refreshing equal sequences prevents a captured duplicate from extending
  a lease, at the cost of requiring a fresh sequence for every periodic send.
- Refusing new entries at capacity preserves bounded memory and stable
  existing candidates, but an attacker can still reduce discovery
  availability.
- Immediate wake and interface invalidation may briefly remove a reachable
  peer until its next valid announcement.

## Alternatives rejected

- mDNS/DNS-SD: useful for naming and reachability, but it adds a larger parser
  and naming contract while providing no authentication. P1 needs only one
  bounded endpoint hint.
- Stable device identifiers or identity fingerprints in advertisements: they
  increase tracking and are still forgeable before authentication.
- Advertised IP addresses: they permit off-link and cross-interface endpoint
  substitution. The UDP source address is the only v1.0 address hint.
- Refreshing equal-sequence duplicates: a captured packet could keep a
  candidate alive without a live publisher.
- Persisting discovery cache or replay state: it turns hostile LAN metadata
  into durable state without providing trust.
- Evicting existing candidates to admit new tokens: an attacker could
  deterministically churn the visible peer list.
