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
.agents/            Multi-agent task and handoff harness
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

The native core has no third-party runtime dependency in the harness phase.
On macOS, install the reproducible toolchain with `brew bundle`, then run
`make bootstrap`. FVM reads `apps/desktop/.fvmrc` and installs Flutter 3.32.8.
`XNN_TRANSFER_INSTALL_TOOLS=1 make bootstrap` combines those two steps.

## Run parallel agents

Commit the integration baseline first. Then create one task worktree per agent:

```bash
tool/harness/agent.sh list
tool/harness/agent.sh claim XT-001 security-agent
tool/harness/agent.sh claim XT-002 protocol-agent
tool/harness/agent.sh claim XT-004 flutter-agent
tool/harness/agent.sh prompt XT-001
```

Start each agent with its generated prompt. Never point multiple agents at the
same worktree. See `.agents/README.md` for transitions and handoff rules.

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
