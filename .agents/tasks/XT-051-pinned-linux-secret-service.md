---
id: XT-051
title: Pinned linux secret service
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-046
owned_paths:
  - .agents/**
  - vcpkg.json
  - cmake/dependencies/**
  - third_party/licenses/**
  - CMakeLists.txt
  - docs/adr/0006-p1-native-runtime.md
  - docs/roadmap.md
  - docs/testing.md
  - tool/harness/dependency_test.sh
  - .github/workflows/ci.yml
contract_changes: []
handoff: .agents/handoffs/XT-051.md
---

## Outcome

Pin and prove a Linux-only libsecret dependency so the canonical identity
module can implement the ADR 0006 Secret Service adapter without a system
library fallback or an untracked dependency graph.

## Context

XT-046 pinned the cross-platform Asio, OpenSSL, and utf8proc graph. XT-022
exposed the remaining gap: ADR 0006 selects libsecret on Linux, but the manifest
does not install it and the dependency gate does not verify it. XT-022 must not
claim a production Linux protected store until this prerequisite is accepted.

## Constraints

- Resolve libsecret from the existing pinned vcpkg commit; no host package,
  floating registry, source-tree vendoring, or plaintext storage fallback.
- Install libsecret only for Linux. macOS and Windows dependency graphs and
  application package contents must remain unchanged.
- Record exact version and source/license provenance, and reject manifest,
  version, platform-expression, static-linkage, and system-fallback drift.
- Keep libsecret and GLib headers private to native implementation targets.
- Do not implement a ProtectedStore adapter or claim that a Secret Service
  backend is qualified; XT-022 owns runtime backend qualification and behavior.
- Keep CI caches content-addressed by all dependency inputs and preserve one
  project-owned dynamic library in each desktop bundle.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 2 risk dimension in the task record. Every
non-none risk must name commands that also appear in `verification.commands`.

## Acceptance criteria

- [ ] A clean Linux bootstrap installs and links the pinned libsecret version
      through the project static triplet.
- [ ] Dependency fixtures reject missing/wrong libsecret version, missing Linux
      platform scoping, source/license drift, and system fallback.
- [ ] macOS and Windows bootstrap and bundles do not install or link libsecret.
- [ ] Linux native, sanitizer/fuzz, and Flutter bundle CI consume the same
      pinned manifest graph.
- [ ] ADR 0006 and the roadmap record the dependency prerequisite without
      claiming that protected identity storage is complete.
- [ ] Repository verification passes.

## Verification

```bash
make dependency-test
make security-test
make verify
```
