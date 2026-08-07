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

## P1 runtime boundaries

ADR 0006 selects standalone Asio 1.38.2 for asynchronous sockets, timers, and
cancellation, and OpenSSL 3.5.7 LTS as the sole TLS and general cryptographic
provider. utf8proc 2.11.3 supplies complete UTF-8, NFC, scalar, and Unicode
category validation at native trust boundaries. These dependencies are
infrastructure details and must not appear in domain interfaces, C ABI structs,
or Flutter types.

One engine owns one Asio `io_context`. Discovery, connection, and transfer
state is serialized on engine-owned executors or strands. Blocking filesystem
work may use a separate bounded worker pool, but state changes and event
publication return to the owning executor. Platform interface monitors feed
bounded snapshots into discovery; they do not mutate peer state directly.

Protected identity storage is platform-specific behind one fail-closed
interface:

- macOS Keychain with synchronization disabled and device-only accessibility;
- Windows Credential Manager with local-machine persistence for the current
  user, never enterprise persistence;
- a qualified device-local Linux Secret Service backend.

There is no production secret-storage fallback. Locked, unavailable, corrupt,
or unqualified storage disables pairing and authenticated transport.

The native build aggregates fixed leaf targets:

```text
xnn_transfer_core
  -> discovery
  -> identity
  -> tls
  -> session
  -> storage
  -> transfer
```

Each P1 module and its tests own a leaf CMake entry point. The pinned vcpkg
manifest, Asio/OpenSSL overlays, utf8proc registry version, static triplets,
and compiled dependency probe are implemented. Product leaf targets remain
empty build boundaries until their owning tasks replace them. Installing the
dependencies does not implement protected storage, discovery sockets, pairing,
TLS policy, or transfer behavior.

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
negotiation independently from transport authentication. ADR 0006 accepts the
P1 runtime and dependency boundaries but does not install or implement those
providers. No current component may claim discovery, protected identity
storage, authenticated pairing, encrypted transfer, or production v1
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

P1 retains that single-owner model. The selected Asio executor will own network
and timer completion, while bounded filesystem workers return results through
the executor before touching observable state. Stop remains a barrier across
both execution domains.

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

P1 dependencies are pinned through vcpkg manifest mode, project-owned static
triplets, and Asio/OpenSSL overlay ports; utf8proc comes from the same pinned
registry commit. The toolchain rejects unpinned vcpkg checkouts and system
OpenSSL fallback. Static dependencies preserve one project dynamic library in
each application bundle.

## Versioning

- C ABI: integer ABI version plus `struct_size` on extensible structs.
- Wire protocol: explicit major/minor negotiation and versioned specifications
  under `protocol/spec/`, governed by ADR 0004.
- Persisted state: schema version and migration policy before state is stored.

Breaking contract changes require an ADR. Unsupported versions fail closed
with a machine-readable error.
