# XnnTransfer

XnnTransfer is a peer-to-peer LAN file transfer desktop client. The desktop UI
is written in Flutter, while discovery, secure sessions, and transfer I/O live
in a C++20 core exposed through a stable C ABI.

The repository is currently in the harness phase: module boundaries, build
gates, and agent collaboration rules are established before networking
features are implemented.

## Repository layout

```text
apps/desktop/       Flutter desktop application
native/             C++20 core and C ABI bridge
protocol/           Versioned wire protocol specifications
docs/               Architecture, ADRs, and delivery roadmap
.agents/            Plans, TaskSpecs, Gate policy, and migration snapshot
tool/harness/       Bootstrap and verification entrypoints
```

`AGENTS.md` is the canonical entrypoint for both human and AI contributors.

## Quick start

```bash
make doctor
make bootstrap
make verify
make security-test
```

`make bootstrap` never installs system packages. It reports missing tools,
fetches project dependencies, and generates Flutter desktop runners when a
Flutter SDK is available.

Required for a full local verification:

- CMake 3.24+
- A C++20 compiler
- Flutter 3.32.x with desktop support enabled
- Git, Perl, and pkg-config for the pinned vcpkg dependency build

`make bootstrap` checks out the exact vcpkg commit under `out/tools/`, then
resolves static Asio 1.38.2, OpenSSL 3.5.7, and utf8proc 2.11.3 dependencies
from committed manifests and hashes. On macOS, install the reproducible
toolchain with `brew bundle` first. FVM reads `apps/desktop/.fvmrc` and
installs Flutter 3.32.8. `XNN_TRANSFER_INSTALL_TOOLS=1 make bootstrap`
combines Homebrew and project bootstrap.

Use `make dependency-test` to validate provenance, compile and link all three
dependencies, and reject dynamic dependency artifacts. This supplies build
infrastructure only; it does not claim discovery, TLS, or transfer behavior.

## Run parallel agents

Create and approve a V2 Delivery Plan and TaskSpec, then claim one task per
agent:

```bash
tool/harness/agent.sh list
tool/harness/agent.sh claim XT-NNN
tool/harness/agent.sh tdd-red XT-NNN
tool/harness/agent.sh submit XT-NNN --red-sha SHA
```

Claim returns an isolated worktree. Review creates an immutable submission,
releases the development worktree, and retains owned-path reservation while
the merge train validates. Runtime state and acceptance evidence live in
independent Git refs, not product commits. See `.agents/README.md`.

`docs/testing.md` records exactly which performance and security checks are
executable and which remain blocked on product implementation.

## Current scope

- C++ core lifecycle and stable C ABI: scaffolded
- Flutter shell and native boundary: scaffolded
- LAN peer discovery: not implemented
- Secure peer pairing: not implemented
- Chunked, resumable file transfer: not implemented

See `docs/roadmap.md` for the implementation order. A UI mock must never be
presented as a working transfer feature.
