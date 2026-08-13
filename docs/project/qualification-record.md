# Project Semantic Baseline Qualification

## Accepted Baseline

- Bootstrap candidate: `7825fde20cf189e1e1a91bc44b69cb397f0a78f1`
- Candidate tree: `8e9187ce5053f51bb37cecd2815632e82a5a35f1`
- Project Model digest:
  `dab46b133fda64269c9857faa782e36b0e6b72b0a99083f93828f0670a004d6a`
- Delivery Plan digest:
  `7de38aba1b58d4dc878c356936329b339abc72c614ace4534b717b6eaad5aea7`
- Hosted workflow run: `31676207687`
- Bootstrap attestation:
  `refs/heads/attest/bootstrap/7825fde20cf189e1e1a91bc44b69cb397f0a78f1`
- Bootstrap attestation digest:
  `1c8f8668303bf1bc8246944a2b8b2f37702c891ad76ec2971a9a9444d093b984`

The hosted run completed `Candidate plan`, `Harness V2`, Linux, macOS,
Windows, `Cutover security`, and `Candidate accepted` successfully against the
exact candidate. The local `verify` aggregate also completed all ten leaf
Gates with `skipped=false`.

## Model Coverage

The accepted model contains 2 goals, 5 milestones, 11 capabilities, 24
outcomes, 12 invariants, 5 quality budgets, 14 implementation units, and 3
composed Blueprint Change Sets. Every file under the declared production roots
has exactly one implementation-unit owner.

Asset disposition at the baseline is:

| Classification | Units |
| --- | ---: |
| `preserve` | 2 |
| `requalify` | 12 |
| `replace` | 0 |
| `remove` | 0 |

No product implementation was deleted or replaced by the re-baseline.
Historical Harness V1 acceptance remains evidence input and does not become
current Blueprint qualification automatically.

## Residual Gaps

The generated roadmap is the authoritative gap projection. Product outcomes
that remain below `qualified` require future owner-approved Change Sets in
dependency order:

1. protocol and core contracts;
2. storage and identity;
3. TLS and session;
4. transfer;
5. C ABI bridge;
6. Flutter application and presentation;
7. cross-platform end-to-end qualification.

Transfer throughput, transfer memory, and cancellation-latency budgets remain
explicitly deferred. The current benchmark is informational and is not
represented as blocking performance evidence.
