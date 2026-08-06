# ADR 0004: Version 1 framing and negotiation

- Status: accepted
- Date: 2026-08-06
- Specification: `protocol/spec/v1.md`
- Security profile: `docs/adr/0002-pairing-and-transport-security.md`

## Context

The native session layer and Flutter adapter need one bounded, versioned wire
contract before networking code can safely allocate buffers or interpret peer
metadata. The first protocol task delivered a detailed v1 specification and
golden vectors, but the repository contract also requires an ADR for a new wire
protocol decision.

Framing and negotiation are separable from authenticated transport. Test
vectors may exercise clear framing, but a parser accepting a frame does not
authenticate a peer or authorize transfer behavior.

## Decision

Adopt the framing, TLV, message registry, version negotiation, hard limits,
state ordering, error scopes, and compatibility rules in
`protocol/spec/v1.md` as the version 1 wire contract.

The fixed header is 28 bytes and uses big-endian fixed-width fields. Header and
body lengths are validated before allocation or body reads. TLVs are canonical,
ordered, bounded, and reject unknown critical fields. Directional message
identifiers and transcript states fail closed.

Version 1 reaches transfer-capable state only after both peers complete the
specified negotiation and transport-binding sequence. ADR 0002 owns concrete
pairing, TLS, identity, exporter, and finished-value semantics. Because ADR
0002 remains proposed, no current implementation may claim authenticated
transport or production v1 conformance.

Compatible additive fields use noncritical TLVs and the specification's
version rules. A change to fixed framing, canonical encoding, message meaning,
security binding, or compatibility behavior requires a new ADR and protocol
major-version review.

## Consequences

- Native parsers can enforce hard limits independently from sockets and TLS.
- Golden vectors are normative regression fixtures for framing behavior.
- Parser acceptance alone is not a security or transfer-readiness claim.
- Unknown critical extensions fail closed; additive noncritical extensions may
  be ignored only where the specification permits.
- Implementations must update the versioned specification and compatibility
  section before changing wire behavior.

## Alternatives rejected

- Ad hoc serialization tied directly to socket reads: unsafe allocation and
  compatibility behavior would be difficult to test independently.
- Treating the detailed specification as the only decision record: this would
  leave the repository's ADR requirement for a new wire contract unmet.
- Coupling parser acceptance to a specific TLS library: framing tests and
  security-profile conformance have different ownership and maturity.
