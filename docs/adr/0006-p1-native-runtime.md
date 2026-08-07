# ADR 0006: P1 native runtime and dependency boundaries

- Status: accepted
- Date: 2026-08-07
- Accepted by task: XT-018
- Amended by tasks: XT-046 (utf8proc and initial dependency provenance),
  XT-051 (pinned Linux libsecret dependency)
- Security profile: `docs/adr/0002-pairing-and-transport-security.md`

## Context

P1 adds cross-platform multicast discovery, TLS 1.3, Ed25519 identity,
protected storage, authenticated sessions, filesystem transactions, and
transfer scheduling. The repository needs one reproducible I/O and
cryptographic stack without moving hostile-input policy or mutable domain state
into third-party callbacks.

The current application packages one native dynamic library and runs CMake,
Ninja, CTest, ASan/UBSan, libFuzzer, and Flutter bundle checks on macOS,
Windows, and Linux. Dependency integration must preserve that shape.

## Decision

### I/O and concurrency

Use standalone Asio 1.38.2 under the Boost Software License for TCP, UDP,
multicast sockets, timers, cancellation, and the native I/O executor. Do not
take a dependency on the rest of Boost.

One engine owns one `asio::io_context`. Mutable discovery, connection, and
transfer state is serialized through engine-owned executors or strands. A
separate bounded worker pool may perform blocking file operations, but its
results return to the owning executor before state changes or events are
published. No module owns an unbounded thread pool.

Asio does not define interface-change policy. Platform infrastructure maps:

- macOS network changes through SystemConfiguration dynamic-store callbacks;
- Windows changes through `NotifyIpInterfaceChange`;
- Linux changes through a bounded `NETLINK_ROUTE` subscription.

Every notification triggers a complete, bounded interface snapshot comparison.
Startup and wake also rescan, so missed notifications cannot keep stale
membership indefinitely.

### Unicode validation

Use utf8proc 2.11.3 under the MIT license for complete UTF-8 decoding, NFC
normalization checks, scalar counting, and Unicode general-category checks at
native trust boundaries. Standard C++20 does not provide those operations, and
platform-specific Unicode behavior would make discovery labels and path policy
inconsistent across macOS, Windows, and Linux.

Wire parsers still enforce byte limits and TLV structure before invoking
utf8proc. The library remains an infrastructure detail and does not define
wire, display, path, or authorization policy.

### TLS and cryptography

Use OpenSSL 3.5.7 LTS under Apache-2.0 as the sole TLS and general
cryptographic provider. The 3.5 branch is supported upstream through
2030-04-08. It is statically linked into `xnn_transfer_core`; the Flutter
bundle continues to ship one project-owned `.dylib`, `.dll`, or `.so`.

XT-023 must configure every client and server context fail-closed with:

```text
minimum TLS version       TLS 1.3
maximum TLS version       TLS 1.3
cipher suites             TLS_AES_128_GCM_SHA256
                          TLS_CHACHA20_POLY1305_SHA256
key-exchange group        X25519
signature algorithm       Ed25519
session cache             disabled
session tickets           zero
maximum early data        zero
renegotiation             unavailable under TLS 1.3
```

Configuration calls are checked individually. Handshake completion also
checks the negotiated version, cipher, group, signature identity,
`SSL_session_reused() == 0`, and absence of early data. Exporter derivation
uses `SSL_export_keying_material`; early exporters are forbidden.

A self-signed certificate may carry the Ed25519 public key, but CA status,
certificate validity, DNS name, address, and discovery data are ignored for
trust. The exact raw 32-byte public key is extracted, validated, and compared
against the live pairing context or stored pin. OpenSSL signature verification
does not replace ADR 0002 canonical decoding, non-identity, and prime-subgroup
validation.

### Protected storage

The identity and pairing-record abstraction uses these production backends:

- macOS Keychain generic-password items with synchronization explicitly off
  and a `ThisDeviceOnly` accessibility class;
- Windows Credential Manager generic credentials with
  `CRED_PERSIST_LOCAL_MACHINE`; enterprise persistence and machine-wide DPAPI
  are forbidden;
- Linux Secret Service through libsecret, only when the concrete service is
  qualified as device-local and non-synchronizing.

An unknown Linux Secret Service, a locked or unavailable store, denied access,
corruption, or an unqualified backend produces `storage_unavailable` and
disables pairing and authenticated transport. There is no plaintext,
encrypted-file, environment-variable, or in-memory production fallback.

The selected OpenSSL integration requires the identity seed in process memory,
so P1 cannot claim a hardware-backed non-exportable key. The protected seed is
loaded only for the shortest signing/configuration lifetime and is explicitly
zeroized. XT-022 owns the logical record format, atomicity, platform
qualification, and failure tests in ADR 0009.

No selected desktop backend supplies a universally trusted monotonic counter.
The implementation detects malformed, partial, and internally inconsistent
rollback, but must not claim detection of a complete valid old system snapshot.
ADR 0002 already records this residual limitation.

### Dependency provenance

Release and CI builds use vcpkg manifest mode with:

- a pinned vcpkg tool/registry commit and `builtin-baseline`;
- project-owned overlay ports for exactly Asio 1.38.2 and OpenSSL 3.5.7;
- utf8proc 2.11.3 from the same pinned built-in registry;
- Linux-only libsecret 0.21.7 from the same pinned built-in registry;
- upstream release archives, SHA-256 verification, and committed license
  notices;
- static dependency triplets matching each application architecture;
- binary-cache keys containing registry commit, overlay content, triplet,
  compiler identity, and configuration.

The public registry did not contain both selected Asio and OpenSSL releases at
this decision point, so a floating registry branch is not an acceptable
substitute for the overlay ports. The pinned registry contains utf8proc 2.11.3
and Linux-only libsecret 0.21.7. Release builds must not discover system
OpenSSL or libsecret. A populated, content-addressed vcpkg binary or source
cache is the supported offline build path; vendored untracked source trees are
not.

Sanitizer and fuzz configurations build dependencies from the same pinned
sources with sanitizer-compatible flags. Release assembly optimizations may
remain enabled, while fuzz configurations may use a reviewed no-assembly
variant to improve instrumentation. A dependency update is a reviewed task:
patch updates within the accepted Asio, OpenSSL, and utf8proc lines rerun all
security and packaging gates; a different OpenSSL minor/major, TLS provider,
I/O runtime, Unicode provider, libsecret minor line, or storage backend
requires an ADR update.

### Module and build ownership

The central native build aggregates these fixed targets:

```text
xnn_transfer_core
  -> xnn_transfer_discovery
  -> xnn_transfer_identity
  -> xnn_transfer_tls
  -> xnn_transfer_session
  -> xnn_transfer_storage
  -> xnn_transfer_transfer
```

Each target is defined in its own leaf `CMakeLists.txt`. XT-018 installs empty
`INTERFACE` boundary targets only. The owning P1 task replaces its placeholder
with an implementation and registers tests from the matching leaf directory.
This does not claim discovery, security, session, storage, or transfer
behavior.

Domain code depends on project interfaces for clocks, entropy, sockets, TLS,
protected storage, filesystem, and event publication. Asio, OpenSSL, libsecret,
Keychain, Windows Credential Manager, and interface-monitoring APIs remain
infrastructure details.

## Consequences

- Networking and TLS share one cancellation and executor model without making
  Asio a domain type.
- Static OpenSSL preserves current single-library packaging at the cost of
  larger artifacts and project-owned security updates.
- One Unicode provider makes native normalization and category decisions
  consistent across all desktop platforms.
- Overlay ports become security-sensitive code and require source-hash,
  license, CVE, and cross-platform review.
- macOS deployment target 10.14 remains provisional until the selected stack
  passes packaged runtime tests; raising it requires a separate compatibility
  decision.
- Linux pairing support is fail-closed when a qualified local Secret Service
  is unavailable.

## Alternatives rejected

- Native IOCP/epoll/kqueue sockets plus OpenSSL: maximum control, but P1 would
  have to build and secure three cancellation, timeout, and shutdown runtimes.
- libuv plus OpenSSL: libuv provides no TLS stream, requiring a custom
  memory-BIO pump and another subtle state machine.
- Platform TLS stacks: they do not provide one auditable implementation of the
  exact exporter, Ed25519 pinning, X25519, ticket, and early-data policy.
- Botan plus Asio: viable, but its TLS/socket integration, support horizon, and
  deployment evidence are weaker for this repository.
- BoringSSL: its project explicitly does not provide a stable third-party API
  or ABI contract.
- FetchContent for all dependencies: Asio is CMake-friendly, but OpenSSL uses
  its own Configure/build flow and cross-platform cache/provenance handling
  would be duplicated.
- System packages in release builds: macOS and Windows do not provide the
  selected OpenSSL, and Linux package versions/options are not reproducible.

## Upstream references

- OpenSSL releases: `https://openssl-library.org/source/`
- OpenSSL release strategy: `https://openssl-library.org/policies/releasestrat/`
- OpenSSL exporter API:
  `https://docs.openssl.org/3.5/man3/SSL_export_keying_material/`
- Asio standalone documentation:
  `https://think-async.com/Asio/AsioStandalone.html`
- Asio SSL native handle:
  `https://think-async.com/Asio/asio-1.38.2/doc/asio/reference/ssl__stream/native_handle.html`
- utf8proc: `https://juliastrings.github.io/utf8proc/`
- vcpkg manifest mode:
  `https://learn.microsoft.com/vcpkg/consume/manifest-mode`
- Secret Service API:
  `https://specifications.freedesktop.org/secret-service/latest/`
- libsecret:
  `https://gitlab.gnome.org/GNOME/libsecret/`
