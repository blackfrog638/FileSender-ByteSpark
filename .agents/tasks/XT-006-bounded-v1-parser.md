---
id: XT-006
title: Add bounded v1 parser security coverage
state: done
workstream: native_core
owner: protocol-parser-agent
depends_on:
  - XT-002
owned_paths:
  - native/src/protocol/**
  - native/tests/protocol/**
  - native/fuzz/protocol/**
  - protocol/testdata/v1/**
contract_changes: []
---

## Outcome

Implement and fuzz a bounded native v1 frame/TLV parser that rejects hostile
input before unsafe allocation, body parsing, or pre-binding state use.

## Acceptance criteria

- [x] Header preflight validates declared limits before body reads.
- [x] Parser behavior matches every golden vector.
- [x] Transcript validation gates transfer bodies until binding completes.
- [x] Sanitizer, fixture drift, and libFuzzer gates cover the parser.

## Verification

See `.agents/records/XT-006.json` and
`protocol/testdata/v1/HANDOFF_XT-006.md`.
