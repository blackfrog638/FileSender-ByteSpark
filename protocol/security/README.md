# Protocol security requirements

This directory defines the security constraints that the versioned LAN
pairing and transport protocol must satisfy.

- `threat-model.md` defines assets, adversaries, trust boundaries, security
  invariants, key lifecycle, privacy, denial-of-service limits, and protocol
  prerequisites.
- `negative-test-matrix.md` defines the adversarial and fault-injection
  coverage required from future implementations.
- `../../docs/adr/0002-pairing-and-transport-security.md` selects the
  first-pair SAS and pinned TLS 1.3 design.

## Implementation status

These documents are design requirements. Discovery, pairing, TLS transport,
key storage, rotation, revocation, and the listed tests are not implemented in
the current repository. A future implementation must first define the missing
wire details under `protocol/spec/`, provide cross-platform golden vectors,
and pass the negative matrix.

Security requirements in this directory do not make an authenticated peer
safe input. Manifest, path, storage, transfer, and receiver-consent checks
remain mandatory after pairing.
