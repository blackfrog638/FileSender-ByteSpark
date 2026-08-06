# Agent handoff

## Delivered

- Task: XT-002, Specify v1 framing and protocol limits
- From owner: protocol-agent
- To owner or reviewer: integration owner and XT-006 native protocol owner
- Branch: `task/XT-002`
- Worktree: `/Users/bytedance/XnnTransfer/XnnTransfer-XT-002`
- Base SHA: `784375866646b9aa3374734ba6f02a00d11b5208`
- Head SHA: this handoff's task commit; use `git rev-parse HEAD`
- Worktree clean: yes after the task commit
- Owned paths: `protocol/spec/v1.md`, `protocol/testdata/v1/**`
- Observable behavior: defines a fail-closed v1.0 wire contract and provides 28
  machine-checked legal and malformed framing/negotiation transcripts

## Contracts

- Added or changed: fixed 28-octet big-endian header, canonical TLV encoding,
  per-direction sequence IDs, version/capability/limit negotiation, message
  registry, connection and transfer state machines, error registry, hard
  limits, flow control, timeout, cancellation, retry, idempotency, resume,
  compatibility, and hostile-input rules
- Compatibility impact: first proposed wire version; all later implementations
  must preserve the IDs and compatibility rules or introduce a reviewed major
  version
- ADR or protocol reference: `protocol/spec/v1.md`; concrete pairing,
  transport protection, commitments, and resume authorization remain blocked
  on the XT-001 security ADR

## Verification evidence

- Command: `python3 protocol/testdata/v1/validate_vectors.py`
- Result: passed all 28 legal and malformed vector cases
- Command: `python3 -m json.tool protocol/testdata/v1/vectors.json`
- Result: passed
- Command: `make verify`
- Result: passed repository layout and agent validation, native CMake/CTest
  1/1, Flutter formatting with 0 changes, Flutter analysis with 0 issues, and
  Flutter tests
- Skipped gate and reason: none

## Residual risk

- Known limitation: the security profile is intentionally abstract until
  XT-001 is approved; cleartext vectors are test-only and production v1
  conformance remains prohibited
- Known limitation: the Python validator is a fixture oracle for framing and
  negotiation, not a production parser or security implementation
- Follow-up task: XT-006 must implement and fuzz the native parser against
  these fixtures; integration owner must review this shared wire contract

## Review focus

- Files or invariants requiring close review: framing length arithmetic,
  canonical TLV rejection, highest-version and exact-capability selection,
  stream/connection failure scope, flow-control limits, resume idempotency, and
  the boundary between this specification and XT-001

## Acceptance

- Accepted by: pending integration-owner review
- Accepted at: pending
- Follow-up runtime state: `review`
