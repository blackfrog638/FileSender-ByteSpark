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

The security protocol is deliberately not selected in this scaffold. It needs
a threat-model ADR and interoperable test vectors before implementation.

## Data flow

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

The scaffold exposes these engine states:

```text
created -> running -> stopped
```

`start`, `stop`, cancellation, and destruction must be idempotent. The current
scaffold implements only this synchronous lifecycle so later agents have a
testable base. The asynchronous event ABI must add an observable stopping phase
before worker threads exist.

## Versioning

- C ABI: integer ABI version plus `struct_size` on extensible structs.
- Wire protocol: explicit major/minor negotiation and versioned specifications
  under `protocol/spec/`.
- Persisted state: schema version and migration policy before state is stored.

Breaking contract changes require an ADR. Unsupported versions fail closed
with a machine-readable error.
