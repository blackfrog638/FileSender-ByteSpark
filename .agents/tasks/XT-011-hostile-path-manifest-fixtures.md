---
id: XT-011
title: Define hostile path and manifest fixtures
initial_state: ready
workstream: protocol
initial_owner: unassigned
depends_on:
  - XT-002
owned_paths:
  - protocol/testdata/v1/manifest/**
  - protocol/spec/v1.md
contract_changes: []
handoff: .agents/handoffs/XT-011.md
---

## Outcome

Provide a versioned, deterministic fixture corpus and validator for legal and
hostile v1 manifests and relative paths. The fixtures define rejection before
future filesystem access; this task does not create files or implement storage.

## Context

Section 11 of `protocol/spec/v1.md` defines manifest and wire-path rules.
Architecture requires bounded manifests, canonical destination containment,
link refusal, temporary writes, and atomic commit. XT-002 supplies the wire
contract and XT-006 supplies bounded frame parsing.

## Constraints

- Cover POSIX, Windows, and normalization/case-collision behavior explicitly;
  a fixture must not depend on the host running the validator.
- Include traversal, absolute/drive/UNC paths, separators, alternate data
  streams, invalid UTF-8, controls, normalization collisions, duplicates,
  ancestor conflicts, special-file representations, and limit arithmetic.
- Check entry count, aggregate path bytes, total file bytes, index ordering,
  offer/end summaries, and integer overflow incrementally.
- Fixtures validate protocol objects only; do not inspect the local filesystem
  or claim symlink-safe destination implementation.
- If deterministic cross-platform behavior requires changing v1 semantics,
  stop and request an ADR instead of silently choosing one platform.

## Acceptance criteria

- [ ] Legal vectors cover files, directories, empty files, nested paths, and
      exact maximum boundaries.
- [ ] Hostile vectors cover traversal, absolute/drive/UNC paths, ADS,
      separator, Unicode, duplicate, collision, ancestor, and special-file
      cases.
- [ ] Manifest vectors cover count/size overflow, noncontiguous indexes,
      inconsistent summaries, invalid kind/size/commitment combinations, and
      aggregate-limit failures.
- [ ] Every vector has a stable expected result and failure reason.
- [ ] A host-independent validator passes on macOS, Linux, and Windows.
- [ ] Any protocol clarification preserves ADR 0004 compatibility or is
      escalated to a new ADR.
- [ ] Focused fixture validation and `make verify` pass.

## Verification

```bash
python3 protocol/testdata/v1/manifest/validate_vectors.py
make verify
```
