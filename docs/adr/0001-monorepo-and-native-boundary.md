# ADR 0001: Monorepo with a versioned C ABI

- Status: accepted
- Date: 2026-08-06

## Context

The desktop product uses Flutter for presentation and C++ for networking,
security, and file I/O. Independent agents need stable ownership boundaries,
and all three desktop platforms need the same native behavior.

## Decision

Keep Flutter, C++, protocol specifications, tests, and tooling in one
repository. Expose the native engine through a C ABI using opaque handles,
fixed-width values, explicit ownership, ABI versioning, and extensible structs
that carry `struct_size`.

Flutter FFI code is isolated under `apps/desktop/lib/core/native/`. Native
implementation details do not cross the boundary.

## Consequences

- Native behavior and protocol changes can be tested independently of Flutter.
- The ABI requires deliberate lifecycle and memory-ownership design.
- Each desktop package must build and bundle a platform-specific dynamic
  library.
- Contract changes require coordinated tests and an ADR update.

Alternatives rejected:

- C++ types through platform channels: ownership and portability are unclear.
- Business logic in Dart: duplicates security-sensitive behavior and weakens
  cross-platform consistency.
- Separate repositories: contract drift and multi-agent integration cost are
  too high for the current project size.
