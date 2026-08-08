# XT-024 identity and TLS runtime security review

- Reviewer: `security-runtime-review-agent`
- Review base: `375a9ac7ae5cfcfbeafbdc1bddf7fe33de0cb6f1`
- Identity delivery: `7c1691df3896b9e9cc3c3d9a2e71539b0d2b6142`
- TLS delivery: `d787f3a4b577903e3da8d119207ee563919d7ac0`
- Decision: accept the implemented provider boundaries; reject any production
  pairing or three-platform conformance claim

## Scope and method

This review covers the complete XT-022 and XT-023 delivery diffs, their public
native interfaces, implementation files, tests, CMake targets, governing ADRs,
and recorded cross-platform CI. It does not treat either task handoff as
independent evidence.

The review reconstructed these attacker-controlled paths:

1. protected-store bytes through platform envelope and record decoding, key
   validation, root binding, record authentication, and repository adoption;
2. peer certificates and negotiated TLS state through OpenSSL verification,
   canonical Ed25519 validation, exact optional pinning, capability creation,
   and exporter access;
3. pairing, confirmation, transport, rotation, and device-identifier bytes
   through canonical decoding and byte-exact cryptographic operations.

The implementation was compared against ADR 0002, ADR 0006, ADR 0009, the
threat model, the complete negative-test matrix, the versioned v1 framing
specification, and the accepted security-profile vectors.

No production session currently calls `IdentityRepository`,
`OpenSslTlsContext`, or `VerifiedTlsConnection`. The review therefore does not
convert an API-misuse hypothesis into an exploitable finding without a
reachable hostile-input path.

## Security finding disposition

No demonstrably exploitable vulnerability survived source-to-sink review of
the two delivery diffs.

Three conformance blockers remain open. They are missing prerequisites, not
vulnerabilities in a reachable production pairing path:

| ID | Blocker | Required disposition |
| --- | --- | --- |
| XR-024-01 | No reviewed pairing-control wire specification registers production ALPN/profile bytes, certificate limits, pairing states, errors, timeouts, or duplicate handling. `protocol/spec/v1.md` explicitly delegates those semantics and the TLS provider accepts only caller-supplied ALPN bytes. | XT-060 owns the versioned contract and hostile vectors. XT-061 owns pre-handshake certificate-limit enforcement. XT-025 depends on both and cannot select private production values. |
| XR-024-02 | Linux has a fail-closed libsecret adapter but no qualified concrete Secret Service or positive lifecycle evidence. | Linux pairing remains disabled. XT-062 owns qualification of the exact GNOME Keyring profile and the complete ADR 0009 real-service lifecycle before XT-032 can claim three-platform support. |
| XR-024-03 | Pairing/session state, liveness, authorization, replay sets, deadlines, admission limits, revocation of active work, and the presentation boundary do not exist yet. | XT-025 and XT-026 must implement these controls. Passing provider tests must not be represented as pairing completion. |

XT-024 accepts the provider implementations as inputs to XT-025. It does not
close the blockers above, accept Linux pairing, or claim ADR 0002 production
conformance. XT-060, XT-061, and XT-062 make the previously unowned
prerequisites durable; their existence is not evidence that their acceptance
criteria have passed.

## Identity provider review

### Trust and record adoption

Protected bytes enter through a platform backend and are bounded before
allocation. The common envelope enforces magic, version, nonzero revision, and
exact payload length. The private record codec then enforces exact fields,
strict ordering, fixed widths, UTF-8, item limits, and no trailing data.

Before a peer becomes visible in repository state, load verifies:

- external and encoded revisions;
- current root store ID, root device ID, and stable item identifier;
- current and tombstoned Ed25519 keys with the ADR 0002 prime-subgroup rule;
- device ID derivation from the exact public-key bytes;
- an HMAC-SHA256 record authenticator under a store-specific HKDF key;
- global uniqueness of current and tombstoned keys;
- the root again before adopting the candidate state.

Malformed, copied, mixed-generation, unauthenticated, or detected rolled-back
records fail closed. Complete valid platform snapshot rollback remains outside
the selected backends' trust boundary and is documented without a stronger
claim.

### Mutation and secret lifetime

Peer commit, rotation, revocation, forget, identity reset, and stale-generation
cleanup use compare-and-swap revisions. Candidate state is validated before
the durable operation and becomes visible only after the operation succeeds.
Reset commits the new root generation before old peer cleanup, and cleanup is
limited to the one persisted retired store ID.

Resident repository state contains no identity seed. Each cryptographic use
reloads and revalidates the protected root, passes a borrowed seed only to a
synchronous callback, and clears the move-only `SecretBuffer`. Peer-record
HMAC uses an HKDF-derived key rather than the Ed25519 seed. Production identity
and TLS code contains no secret logging or plaintext storage fallback.

### Platform result

| Platform | Evidence | Result |
| --- | --- | --- |
| macOS | Device-only, non-synchronizing Keychain attributes are set and revalidated; no authentication UI is allowed; real create/get/CAS/delete runs in the native matrix. | Accepted |
| Windows | Generic credentials require current-user local-machine persistence; enterprise persistence and malformed attributes are rejected; returned blobs are cleared; real create/get/CAS/delete runs in the native matrix. | Accepted |
| Linux | Encrypted libsecret session, stable D-Bus owner, current-user service, default unlocked collection, noninteractive operations, strict attributes, and private runtime lock are enforced. Missing or rejecting qualification fails closed. | Provider boundary accepted; production backend unresolved by XR-024-02 |

The current-user platform stores do not defend against an administrator, a
compromised kernel, or complete valid OS snapshot rollback. Those are existing
documented limits, not newly accepted properties.

## TLS provider review

Untrusted certificate bytes are parsed by pinned OpenSSL 3.5.7. The verify
callback accepts exactly one self-signed Ed25519 identity certificate whose
signature verifies and whose raw key canonically decodes as a non-identity
prime-subgroup point. Trust is not derived from CA, DNS, address, certificate
validity, discovery metadata, or certificate serialization.

Every verified connection is checked after handshake for:

- TLS 1.3 as both minimum and maximum version;
- only `TLS_AES_128_GCM_SHA256` or
  `TLS_CHACHA20_POLY1305_SHA256`;
- X25519 key exchange and Ed25519 peer signature;
- exact ALPN equality;
- no session reuse and no early data;
- exactly one verified self-signed identity certificate;
- exact raw-key pin equality when the caller supplies an established-peer pin.

Session cache, tickets, and early data are disabled at context construction.
Configuration calls are checked. Exporters require a move-only capability from
the same context and live `SSL*`, recheck all negotiated parameters, and
re-extract the same peer key before deriving material with a typed,
domain-separated label.

The capability proves TLS profile and peer key possession. It does not by
itself prove user pairing or durable trust. In particular, `VerifyPeer` with no
pin is valid only for the bounded first-pairing state. XT-025 must require an
active repository pin before treating the same provider result as established
transport authority.

The custom Ed25519 validator performs canonical decompression, rejects the
identity, and computes `[L]P` over a fixed 256-bit loop. Published RFC keys, 512
OpenSSL-derived keys, identity, order-2, mixed-order, invalid-curve, and
noncanonical encodings exercise the implementation.

## Required prerequisite map

| ADR 0002 prerequisite | Evidence and status |
| --- | --- |
| 1. Reviewed versioned wire specification | **Open: XR-024-01, owned by XT-060 and XT-061.** v1 framing defines normalized and raw transfer negotiation, but pairing-control messages, production ALPN/profile registration, certificate cap, pairing errors, and pairing duplicate semantics are not specified or enforced. |
| 2. Cross-platform golden vectors | **Pass for the accepted profile objects.** Native tests consume all 17 positive and 37 negative vectors for contexts, SAS, confirmations, transport binding, rotation proof inputs, device identifiers, and malformed encodings. The same test target runs in the Linux, macOS, and Windows native matrix. |
| 3. TLS dependency and configuration | **Pass for the provider.** Pinned OpenSSL 3.5.7, strict TLS 1.3 configuration, mutual identity proof, raw-key pinning, X25519, exporter checks, and disabled tickets/cache/early data execute in all three native jobs. XT-060 and XT-061 own production registration and certificate bounds. |
| 4. Platform protected storage | **Partial: XR-024-02, owned by XT-062.** Platform-independent fault and race tests plus real macOS and Windows lifecycle tests pass. Linux fails closed but has no qualified concrete backend or positive lifecycle test. |
| 5. Native pairing and trust APIs | **Open: XR-024-03.** The lower-level repository and TLS/profile primitives exist. Pairing attempts, liveness, explicit UI decisions, replay state, revocation of live sessions, and presentation-safe APIs belong to XT-025 and XT-026. |
| 6. Negative-test matrix | **Partial and tracked below.** Provider-applicable unit/golden/platform rows have evidence. The remaining rows are assigned to XT-060 through XT-062 or the existing session, transfer, presentation, and acceptance tasks. |
| 7. Independent review | **Pass for provider scope when integration-owner accepts XT-024.** XT-010 accepted the design and vectors. This review independently covers the XT-022/023 runtime diffs. No C ABI or wire contract changed in those deliveries. |

## Negative-test matrix mapping

This table is the tracked evidence plan required by ADR 0002. "Provider" means
the row has executable identity/TLS evidence but may still require session or
end-to-end coverage before production acceptance.

| Rows | Current evidence | Remaining owner |
| --- | --- | --- |
| D-01, D-03, D-04 | Discovery parser, cache, expiry, prohibited-field, and bounded-input tests from XT-019/020. | XT-032 cross-platform acceptance |
| D-02, D-06 | Exact TLS pin enforcement exists; endpoint-to-live-attempt binding does not. | XT-025 |
| D-05 | Discovery bounds exist; end-to-end connection and prompt admission limits do not. | XT-025 and XT-032 |
| P-01, P-02 | Exporter separation and agreement are tested, but no adversarial relay/MITM integration exists. | XT-025 and XT-032 |
| P-03 | Golden confirmation replay fails for a fresh context; live fresh-handshake replay remains untested. | XT-025 |
| P-04, P-08, P-10, P-12, P-17, P-18, P-20 | No live attempt state exists. | XT-025 |
| P-05, P-06, P-07 | Role/key swaps, canonical context mismatch, negotiation downgrade, unknown profile, and exact profile fields have golden rejection evidence. | XT-025 state integration |
| P-09 | Authenticated and local rejection are typed and cannot permit the profile trust gate. | XT-025 terminal cleanup and no-write integration |
| P-11 | Exact HKDF expansion, five 11-bit indices, and fixed word-list outputs have golden evidence. | XT-026 presentation integration |
| P-13, P-14 | Missing identity certificates, invalid Ed25519 keys, self-signature, single-certificate chain, and exact pin checks execute. External metadata override and certificate-byte limits remain incomplete. | XT-060, XT-061, and XT-025 |
| P-15 | No session can dispatch transfer data yet. | XT-025 and XT-028 |
| P-16 | Identity entropy failure and invalid exporter input lengths fail closed. Live RNG/exporter fault injection is incomplete. | XT-025 and XT-032 |
| P-19 | Duplicate, missing, out-of-order, truncated, trailing, wrong-domain, wrong-role, invalid-key, and malformed transcript vectors execute. | XT-025 parser/state integration |
| T-01 | A live TLS handshake with the wrong exact pin fails. | XT-025 established reconnect |
| T-02 | The provider trusts only the exact raw key and ignores certificate metadata by construction. | XT-025 explicit valid-metadata/wrong-pin integration |
| T-03, T-04, T-05 | TLS 1.3 bounds, approved suites, X25519, Ed25519, ALPN, no tickets/cache, no early data, and attempted resumption all execute. | XT-025 connection integration |
| T-06, T-07 | Transport replay, context, role, key order, session ID, negotiation, and finished-value vectors execute. | XT-025 live cross-connection replay |
| T-08, T-10 | Deterministic selection and unknown/noncanonical fields fail closed. | XT-025 adversarial negotiation integration |
| T-09 | Security floor is authenticated in storage but is not yet enforced by a session. | XT-025 |
| T-11, T-12, T-13, T-14, T-16 | No established session or transfer dispatch exists. | XT-025, XT-028, and XT-029 as applicable |
| T-15 | Pairing grants no implemented storage path. Hostile manifest/storage coverage belongs to the transfer chain. | XT-027 and XT-028 |
| K-01 | Locked, unavailable, denied, unsupported, and unqualified stores fail closed with no fallback. | XT-062 for positive Linux evidence |
| K-02 | Schema, field, length, key, MAC, item binding, and revision corruption tests execute. | XT-025 error-state integration |
| K-03 | Inconsistent rollback is detected; complete valid snapshot rollback is explicitly not claimed. | Platform limitation retained |
| K-04, K-05, K-06 | Dual-key proof inputs, transport context, and monotonic counters have golden/unit evidence; signature verification and live authorization do not. | XT-025 |
| K-07 | Candidate-state CAS, reset commit point, cleanup failures, and stale-process races execute with fault injection. Real OS fault injection at every boundary is incomplete. | XT-062 for Linux; XT-032 for platform acceptance |
| K-08 | Rotation retains bounded old-key tombstones and rejects reuse in repository state. | XT-025 reconnect enforcement |
| K-09 | Missing root or seed with surviving peer records returns identity loss and never regenerates trust. | XT-025 visible reset/revocation state |
| K-10, K-11, K-12 | Exact pin and revoked durable state exist; rediscovery and re-pair state do not. | XT-025 |
| K-13 | macOS and Windows non-synchronizing/local attributes are enforced and revalidated. | XT-062 for Linux |
| K-14 | Repository cleanup and reset races execute; session cancellation/disconnect callbacks do not exist. | XT-025 and XT-029 |
| R-01 | Discovery privacy and token rotation are outside the identity/TLS provider diff. | XT-020 evidence and XT-032 acceptance |
| R-02 | Identity/TLS production code emits no secret-bearing logs. Session, UI, and transfer error paths do not exist. | XT-025 through XT-031 |
| R-03 | No unauthenticated session response exists. | XT-025 |
| R-04 | Canonical objects and protected items are bounded before allocation. Certificate byte/chain policy is open. | XT-060 and XT-061 |
| R-05 through R-13 | Admission, deadline, rate, parser-to-state, terminal response, cancellation, and pre-auth side-effect controls require the session runtime. | XT-025 and XT-032 |
| R-14 | TLS encrypts the future channel, but no authenticated file transfer exists. | XT-028 and XT-032 |

Rows with a remaining owner are not passes. The mapping prevents a generic
repository gate from being used as specialized evidence for those behaviors.

## Executed evidence

- Focused identity and TLS build: passed.
- Focused CTest selection: 5/5 passed for identity repository, platform
  protected store, Ed25519 validation, security-profile vectors, and live TLS
  provider.
- GitHub Actions `31233301408`: independently queried as completed/success for
  XT-022; the workflow runs native tests on Linux, macOS, and Windows.
- GitHub Actions `31237541413`: independently queried as completed/success for
  XT-023; the workflow runs the same provider tests on all three systems.
- XT-022 source/result patch ID:
  `06388a3393e55c120c45b1888cdd8799f250ef78`.
- XT-023 source/result patch ID:
  `6d7f74931613eefd36fbb00db890585ee9b00d0b`.

Repository security and verification gates are recorded in the XT-024 handoff
after execution.

## Final boundary

The protected-identity implementation, macOS and Windows adapters, fail-closed
Linux adapter boundary, canonical profile primitives, and TLS provider are
acceptable lower-level inputs to the session workstream.

This review does not authorize production pairing, Linux pairing, transfer
security, or ADR 0002 conformance. Those claims remain blocked by
XR-024-01 through XR-024-03 and by every negative-matrix row whose remaining
owner has not supplied executable evidence. XT-060 through XT-062 are the
durable owners for the prerequisites that had no task before this review.
