# ADR 0008: P1 operation C ABI

- Status: accepted
- Date: 2026-08-07
- Initial owner task: XT-021
- Accepted by task: XT-021
- Event model: `docs/adr/0003-asynchronous-native-event-abi.md`

## Context

Discovery, pairing, transfer, destination policy, and resume must cross the
Flutter/native boundary without exposing C++ types, borrowed asynchronous
memory, secrets, or unbounded attacker-controlled values. The existing ABI
only supports engine lifecycle and a bounded wakeup-and-drain event queue.

## Decision

P1 operations will extend the C ABI additively with struct-size/version
negotiation, opaque operation identifiers, copied command inputs, bounded
events, and snapshot pagination for overflow recovery. Presentation may request
actions but cannot inject identity keys, transcript values, trust decisions
other than explicit confirm/reject, protocol frames, or peer-controlled local
paths.

The discovery extension establishes the common shape:

- Existing ABI v1 values, layouts, and symbols remain unchanged. New adapters
  feature-detect the complete operation extension they require.
- Command structs contain bounded inline input. Native code copies the input
  before returning and retains no Dart-owned pointer.
- Peer events use the existing 64-entry wakeup-and-drain queue and fit in its
  256-byte inline payload. Addresses and display labels are copied with
  explicit lengths.
- A nonzero `peer_id` identifies one process-local observation. It is not
  stable across expiry or restart and proves neither device identity nor
  trust. Later pairing commands refer to the native observation rather than
  accepting a Dart-selected endpoint.
- The bridge maintains at most 256 peers independently from the event queue.
  A dropped-event flag requires reconciliation through fixed eight-peer
  snapshot pages.
- Snapshot revision zero starts a read. Continuation pages pass the returned
  revision and `offset + count`. Concurrent mutation returns
  `STALE_SNAPSHOT`; callers discard partial results and restart.
- Discovery start requires a running engine. Discovery stop is idempotent,
  stops the system runtime, waits for its callbacks, then publishes bounded
  expiry events. Engine stop performs the same barrier before its final
  lifecycle event.
- Operation commands and snapshots are rejected from inside the synchronous
  native wakeup callback. Dart's `NativeCallable.listener` executes later,
  after that native callback has returned.

The Dart adapter copies every inline payload before decoding it. Event loss
triggers a bounded snapshot retry; no native pointer or array view survives a
call boundary.

The pairing extension follows the same boundary:

- A caller may open one pairing window for at most 120 seconds, close it
  idempotently, and start an outgoing attempt only from a live native
  `peer_id`. It cannot provide an address, identity key, certificate, role,
  protocol frame, transcript value, nonce, or security profile.
- A native-selected random 128-bit attempt ID authorizes exactly one visible
  SAS ceremony. Confirmation and rejection accept only that copied ID. A
  stale, unknown, terminal, or replaced ID returns `STALE_HANDLE` and cannot
  affect another attempt.
- Attempt events expose only a public state, the selected discovery
  observation, a monotonic decision deadline, five display-only SAS word
  indices, and a collapsed local error. Parser, certificate, pin, identity,
  storage, transcript, and confirmation details never cross the ABI.
- Successful local trust creates a process-local nonzero `trust_id`. Trust
  events and snapshots expose only that ID, its active or revoked state, and
  the originating discovery observation when one exists. They never expose a
  device ID, public key, fingerprint, profile floor, record revision, or
  persisted metadata.
- Revocation accepts only a native-issued `trust_id`. Unknown and already
  revoked IDs have the same idempotent success result, preventing a
  trust-record oracle. A known active ID is reported revoked only after the
  native repository operation succeeds.
- At most one visible attempt is retained for snapshot recovery. Trust
  snapshots are revisioned and paged in fixed groups of eight across the
  native repository's 256-record ceiling. Any queue drop requires Dart to
  reconcile discovery, pairing, and trust snapshots.
- Engine stop closes admission, cancels the active attempt through the native
  session owner, publishes its terminal state while the queue is still open,
  and then applies the existing callback barrier.

The bridge deliberately has no presentation-controlled fallback when the
production TLS connection dispatcher or protected identity repository is
unavailable. Pairing start returns `UNAVAILABLE`; it does not manufacture a
session, SAS, identity, transcript, or trust result. The C ABI remains usable
by the native session/network owner once that owner supplies the completed
XT-025 attempt updates.

XT-030 and XT-037 may extend this accepted pattern only with integration-owner
review and ABI regression tests. They must preserve lifecycle symbols, copied
payloads, opaque native-selected identifiers, bounded pagination, explicit
overflow, and shutdown barriers.

## Consequences

- Snapshot storage stays bounded even when Flutter is stalled, while stale
  pagination is explicit instead of returning mixed revisions.
- The fixed inline structs are larger than pointer-based APIs but remove
  asynchronous ownership ambiguity and platform allocator coupling.
- Discovery remains an untrusted reachability hint. The ABI deliberately does
  not expose identity keys, trust state, or a command that accepts an arbitrary
  Dart-provided address.
- Pairing presentation can compare and decide one native ceremony but cannot
  construct one. End-to-end pairing still depends on a production connection
  dispatcher that consumes the XT-025 session API.
