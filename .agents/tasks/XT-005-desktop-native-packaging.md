---
id: XT-005
title: Package the native library for Flutter desktop
initial_state: done
workstream: integration
initial_owner: packaging-agent
depends_on: []
owned_paths:
  - apps/desktop/linux/**
  - apps/desktop/macos/**
  - apps/desktop/windows/**
  - tool/harness/**
  - .github/workflows/**
contract_changes: []
---

## Outcome

Build, bundle, locate, and load the native core in Debug and Release Flutter
desktop applications on Linux, macOS, and Windows.

## Acceptance criteria

- [x] Each platform packages the native library in a relocatable location.
- [x] Bundle smoke tests load the packaged library and verify ABI version.
- [x] Debug and Release packaging run on all supported CI platforms.
- [x] Packaged Dart callback tests use the real bundled library.

## Verification

See `.agents/records/XT-005.json` and
`tool/harness/XT-005-handoff.md`.
