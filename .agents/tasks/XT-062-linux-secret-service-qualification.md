---
id: XT-062
title: Linux secret service qualification
state: ready
workstream: native_core
owner: unassigned
depends_on:
  - XT-024
  - XT-051
owned_paths:
  - native/include/xnn_transfer/core/security/identity/linux_secret_service_store.hpp
  - native/src/security/identity/linux_secret_service_backend.cpp
  - native/src/security/identity/CMakeLists.txt
  - native/tests/security/identity/platform_protected_store_test.cpp
  - native/tests/security/identity/CMakeLists.txt
  - .github/workflows/ci.yml
  - docs/adr/0006-p1-native-runtime.md
  - docs/adr/0009-protected-identity-storage.md
  - docs/testing.md
contract_changes:
  - Qualify one exact device-local non-synchronizing Linux Secret Service profile
handoff: .agents/handoffs/XT-062.md
---

## Outcome

Qualify GNOME Keyring's Secret Service as the supported Linux protected
identity backend and prove the complete ADR 0009 lifecycle against a real
noninteractive service in Linux CI.


## Context
XT-051 pins libsecret 0.21.7 and XT-022 supplies a strict fail-closed adapter.
XT-024 recorded XR-024-02 because no concrete service is enabled by production
policy and Linux evidence covers only compilation, locking, and rejected
qualification. ADR 0009 defines the missing positive lifecycle boundary.

## Constraints
- Qualify only a current-user GNOME Keyring Secret Service whose stable D-Bus
  owner, UID, PID, canonical executable, session algorithm, collection, and
  non-synchronizing deployment policy all match the reviewed profile.
- Keep unknown services, service-owner changes, absent policy, locked
  collections, prompts, plaintext sessions, and denied operations fail closed.
- Exercise real create/get/CAS replace/delete/enumerate, restart persistence,
  reset, stale-generation cleanup, corruption, rollback, restore, and missing
  root/seed behavior under `dbus-run-session`.
- Exercise at least two processes contending for first write and stale
  revisions through the real service and payload-free runtime lock.
- Prove item bounds, exact private attributes, default persistent collection,
  local-only policy, deletion, and seed cleanup at the libsecret boundary.
- Add a Linux CI job/setup that provisions the selected service without
  weakening macOS or Windows dependency, bundle, or credential tests.
- Do not add filesystem, environment-variable, plaintext, or production
  in-memory secret fallback. Unsupported Linux desktops remain explicitly
  unavailable rather than silently using weaker storage.
- Amend ADR 0009 and ADR 0006 only after executable evidence identifies the
  exact qualified profile and residual platform assumptions.

## Architecture change

The record declares `none` mode. Keep affected modules,
superseded paths/symbols/targets, temporary leases, and lease retirements
machine-readable in `architecture_change`.

## Risk profile

Resolve every schema version 3 risk dimension in the task record. Every
non-none risk must name trusted gate IDs that also appear in
`verification.gates`; commands are resolved from `.agents/manifest.yaml`.

## Acceptance criteria

- [ ] Production policy accepts only the reviewed GNOME Keyring identity and
      rejects an absent, replaced, wrong-user, or unknown service.
- [ ] Real Linux lifecycle tests cover all ADR 0009 positive and negative
      integration rows, including restart, reset, cleanup, and two-process CAS.
- [ ] Locked, prompt-requiring, denied, corrupt, rolled-back, restored, and
      partially deleted state cannot create or restore trust.
- [ ] No secret bytes enter the filesystem lock, process environment, logs, or
      a fallback store; libsecret buffers are cleared at the API boundary.
- [ ] Linux CI executes the real service tests while macOS and Windows remain
      unaffected.
- [ ] ADRs and testing documentation name the exact supported backend and do
      not generalize the result to arbitrary Secret Service implementations.
- [ ] Repository verification passes.

## Verification

```bash
make native-test
make security-test
tool/harness/desktop_bundle_test.sh
make verify
```
