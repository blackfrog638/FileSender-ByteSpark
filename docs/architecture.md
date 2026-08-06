# Architecture

## Product boundary

XnnTransfer is a desktop application for direct file transfer between peers on
the same LAN. Flutter owns user interaction and presentation. C++ owns every
security-sensitive or throughput-sensitive operation: discovery, session
establishment, protocol parsing, file access, hashing, and network I/O.

The first supported targets are macOS, Windows, and Linux.

## Runtime components

```text
+---------------- Flutter process ----------------+
| presentation -> application -> native adapter   |
+-------------------------|------------------------+
                          | versioned C ABI
+-------------------------v------------------------+
| C ABI bridge -> transfer engine                  |
|   discovery | session | transfer | safe storage  |
|              C++20 worker runtime                |
+-------------------------|------------------------+
                          | LAN
                    remote XnnTransfer
```

The C ABI is asynchronous by design. Long-running operations must return an
operation identifier and publish events through a callback or polled event
queue. No C++ exception, STL type, borrowed string, or ownership ambiguity may
cross the ABI.

The implemented ABI currently publishes lifecycle events through a bounded
wakeup-and-drain queue. Discovery, session, and transfer operations have not
yet been added to that event surface.

## Native modules

- `core`: engine lifecycle, operation model, domain types, and orchestration.
- `discovery`: multicast announcements and peer expiry. Discovery establishes
  reachability only; it never establishes identity.
- `session`: protocol negotiation, pairing, authentication, encryption, and
  reconnect behavior.
- `transfer`: manifest validation, chunk scheduling, integrity checks, resume,
  cancellation, and flow control.
- `storage`: path normalization, destination policy, atomic writes, quota and
  free-space checks.
- `bridge`: converts the C++ model to the versioned C ABI.
- `protocol`: bounded v1 frame/TLV parsing and transcript-order validation.
  Parser acceptance does not authenticate a peer or authorize a transfer.

Infrastructure modules may implement domain interfaces, but domain code must
not depend on a concrete socket, crypto, filesystem, or Flutter type.

## Flutter modules

Each feature uses three layers:

- `domain`: immutable UI-facing models and repository contracts.
- `application`: state transitions and use-case orchestration.
- `presentation`: widgets and platform interaction.

Only `lib/core/native/` may import `dart:ffi`. Widgets consume application
state and never manage native pointers.

## Trust boundaries

LAN traffic and peer-advertised metadata are hostile input. The native core
must enforce:

- explicit protocol and ABI version negotiation;
- bounded frame, file, path, and collection sizes;
- authenticated encryption before file metadata or contents are accepted;
- canonical destination containment with no traversal or link following;
- temporary-file writes followed by integrity verification and atomic rename;
- cancellation, timeout, backpressure, and idempotent cleanup;
- no automatic file acceptance based only on discovery.

ADR 0002 accepts the pairing and authenticated transport design, but the
profile remains unimplemented. ADR 0004 accepts bounded v1 framing and
negotiation independently from transport authentication. No current component
may claim authenticated pairing, encrypted transfer, or production v1
conformance.

## Data flow

The following is the target product flow, not current end-to-end behavior:

1. Discovery reports a reachable, untrusted peer candidate.
2. The user selects a peer and completes an authenticated pairing flow.
3. Peers negotiate one protocol version and transfer capabilities.
4. The sender submits a bounded manifest.
5. The receiver validates paths, policy, and available space, then explicitly
   accepts or rejects the offer.
6. Chunks are encrypted, flow-controlled, verified, and persisted to temporary
   files.
7. Complete files are hash-verified and atomically committed.
8. Both peers retain a resumable transfer record with bounded lifetime.

## Concurrency and lifecycle

One native engine owns all native state. Flutter creates exactly one engine per
process and destroys it after subscriptions and operations stop. Native worker
threads never invoke Flutter-owned memory after shutdown begins.

The implemented engine exposes these observable states:

```text
created -> running -> stopping -> stopped
```

`start`, `stop`, callback clearing, and destruction have deterministic
lifecycle rules. Lifecycle events are copied from a bounded native queue into
caller-owned storage. The callback is only a serialized wakeup; it carries no
borrowed event payload. Stop and destruction prevent callbacks from beginning
after their shutdown barriers return.

Flutter has an implemented, pure-Dart transfer application state model and
gateway abstraction. It is currently exercised with a fake gateway and is not
wired to native discovery, sessions, storage, or file transfer.

## Packaging

Debug and Release desktop builds compile and bundle the native core on Linux,
macOS, and Windows. CI loads the packaged library and exercises the real Dart
event callback boundary. This proves packaging and ABI loadability, not LAN
transfer behavior.

## Versioning

- C ABI: integer ABI version plus `struct_size` on extensible structs.
- Wire protocol: explicit major/minor negotiation and versioned specifications
  under `protocol/spec/`, governed by ADR 0004.
- Persisted state: schema version and migration policy before state is stored.

Breaking contract changes require an ADR. Unsupported versions fail closed
with a machine-readable error.
