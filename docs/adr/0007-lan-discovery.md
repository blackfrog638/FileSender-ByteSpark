# ADR 0007: Unauthenticated LAN discovery v1

- Status: proposed
- Date: 2026-08-07
- Owner task: XT-019
- Security profile: `docs/adr/0002-pairing-and-transport-security.md`

## Context

P1 needs peers to learn reachable LAN endpoints while treating every discovery
packet, name, address, and capability as attacker-controlled. Discovery must
handle multiple interfaces, address churn, sleep/wake, duplicates, expiry, and
bounded denial-of-service behavior before a native socket parser is added.

## Proposed boundary

XT-019 will define a byte-exact, versioned datagram contract and lifecycle
model under `protocol/spec/discovery-v1.md`. Advertisements may provide
reachability and display hints only. They cannot establish identity, pairing,
authorization, protocol negotiation, or file-transfer acceptance.

The contract must bound datagram size, fields, peer count, TTL, processing
rate, memory, and duplicate state. Malformed or unsupported advertisements
must not create or refresh a peer.

## Acceptance boundary

This ADR remains proposed until XT-019 provides:

- canonical encoding and compatibility rules;
- privacy and trust-boundary analysis;
- interface and expiry state transitions;
- positive and hostile golden vectors;
- parser allocation and rate limits;
- integration-owner review.

No current component claims LAN discovery behavior.
