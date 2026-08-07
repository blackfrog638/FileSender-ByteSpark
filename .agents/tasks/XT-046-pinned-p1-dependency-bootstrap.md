---
id: XT-046
title: Pinned p1 dependency bootstrap
state: ready
workstream: integration
owner: unassigned
depends_on:
  - XT-045
owned_paths:
  - .agents/**
  - .gitattributes
  - vcpkg.json
  - vcpkg-configuration.json
  - cmake/**
  - third_party/licenses/**
  - CMakeLists.txt
  - CMakePresets.json
  - Brewfile
  - Makefile
  - README.md
  - docs/adr/0006-p1-native-runtime.md
  - docs/architecture.md
  - docs/testing.md
  - tool/harness/**
  - .github/workflows/ci.yml
  - apps/desktop/linux/CMakeLists.txt
  - apps/desktop/windows/CMakeLists.txt
  - apps/desktop/macos/build_native.sh
contract_changes:
  - ADR 0006 adds utf8proc 2.11.3 as the canonical Unicode validator
  - Release and CI dependency resolution becomes pinned vcpkg manifest mode
handoff: .agents/handoffs/XT-046.md
---

## Outcome

Bootstrap and prove one static, pinned Asio 1.38.2, OpenSSL 3.5.7, and
utf8proc 2.11.3 dependency graph for native and Flutter desktop builds on
macOS, Windows, and Linux.

## Context

ADR 0006 selected standalone Asio and OpenSSL with vcpkg overlay ports, but
XT-018 intentionally installed only empty module boundaries. XT-020 cannot
implement discovery sockets or complete NFC/category validation without the
selected runtime and a Unicode provider. This task supplies those dependencies
without implementing discovery, TLS, identity, storage, or transfer behavior.

## Constraints

- Pin vcpkg to commit `17f35ad2418007a895ced8a4cece4ab34068a58d`;
  no floating branch, system OpenSSL fallback, or untracked vendored source.
- Overlay exactly Asio 1.38.2 and OpenSSL 3.5.7 from upstream release archives
  with committed cryptographic hashes and license notices. Resolve utf8proc
  2.11.3 from the pinned registry and record its upstream archive hash.
- Use static library linkage with project triplets while preserving the
  existing single project dynamic library and the platform-compatible CRT.
- Native, sanitizer, fuzz, and Flutter runner configurations must resolve the
  same manifest and overlays. Dependency headers remain private implementation
  details and do not enter public C ABI or core headers.
- Bootstrap is idempotent, verifies the vcpkg checkout, disables telemetry,
  and stores source, installed, and binary caches only under ignored output
  paths.
- CI cache identity includes the registry commit, overlay/triplet content,
  target platform and architecture, compiler image, and build configuration.
- Add a compiled/linked probe that reports exact dependency versions and
  rejects dynamic OpenSSL or utf8proc artifacts where the platform permits a
  deterministic static-library check.

## Risk profile

Resolve every schema version 2 risk dimension in the task record. Every
non-none risk must name commands that also appear in `verification.commands`.

## Acceptance criteria

- [ ] A clean bootstrap configures, builds, and runs the dependency probe with
  Asio 1.38.2, OpenSSL 3.5.7, and utf8proc 2.11.3.
- [ ] Manifest validation rejects version, baseline, source-hash, overlay,
  triplet, license, or system-fallback drift.
- [ ] Native, sanitizer/fuzz, and Flutter bundle CI use the pinned vcpkg
  checkout and project static triplets on macOS, Windows, and Linux.
- [ ] Packaged applications still contain one project-owned native dynamic
  library and load it through the existing Dart callback test.
- [ ] ADR 0006 and architecture documentation record utf8proc and the landed
  dependency/bootstrap boundary without claiming product runtime behavior.
- [ ] Repository verification passes.

## Verification

```bash
make dependency-test
make security-test
make verify
```
