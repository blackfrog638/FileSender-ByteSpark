# Agent handoff

## Delivered

- Task: XT-006, Add protocol parser security test fixtures
- From owner: protocol-parser-agent
- To owner or reviewer: integration owner
- Branch: `task/XT-006`
- Worktree: `/Users/bytedance/XnnTransfer/XnnTransfer-XT-006`
- Base SHA: `a1870f9a0f7f0d2f21387359ade15b0f819ffe7e`
- Head SHA: use the delivered task commit SHA
- Worktree clean: yes after the task commit
- Owned paths: `native/src/protocol/**`, `native/tests/protocol/**`,
  `native/fuzz/protocol/**`, `protocol/testdata/v1/**`
- Observable behavior: parses bounded v1 frames and TLVs, validates the
  parser-level negotiation transcript, and maps all 29 golden cases to accept
  or their stable expected protocol error

## Contracts

- Added or changed: internal C++20 `v1_parser` API, zero-copy frame/field
  views, fixed-capacity TLV collections, stable parser error codes, and a
  parser-level transcript validator
- Compatibility impact: no C ABI, wire specification, security profile, or
  public cross-module contract changed
- ADR or protocol reference: `protocol/spec/v1.md`; pairing, TLS, exporter,
  commitment, and `TRANSPORT_FINISHED` verification remain unimplemented

## Verification evidence

- Command: `python3 protocol/testdata/v1/validate_vectors.py`
- Result: passed all 29 fixture-oracle cases
- Command:
  `python3 protocol/testdata/v1/generate_native_vectors.py --check`
- Result: generated C++ fixture matched `vectors.json`, SHA-256
  `61dc0811e282b474c362c44503029ec667e9a5d7bf7abba4b115caed2f5564c7`
- Command: Apple Clang C++20 compile with `-Wall -Wextra -Wpedantic
  -Wconversion -Wsign-conversion -fsanitize=address,undefined`, followed by
  `/tmp/xnn_transfer_v1_parser_test_sanitized`
- Result: warning-clean build; all 29 transcript cases and hostile parser tests
  passed under ASan/UBSan
- Command: Homebrew LLVM C++20 build with
  `-fsanitize=fuzzer,address,undefined`, followed by
  `/tmp/xnn_transfer_v1_parser_fuzzer -runs=20000 -max_len=2097792 -timeout=5`
- Result: 20,000 runs completed without crash or sanitizer finding
- Command: `make verify`
- Result: all repository gates passed: layout/backlog, existing native CTest
  1/1, Flutter format/analyze, and 24 Flutter tests
- Skipped gate and reason: repository CMake did not execute the new protocol
  targets because editing shared `native/CMakeLists.txt` is outside XT-006
  ownership; equivalent targets were compiled and run directly

## Shared-file integration patch

Apply the following three hooks to `native/CMakeLists.txt`:

```diff
@@
 endfunction()
+
+add_subdirectory(src/protocol)
@@
 if(XNN_TRANSFER_BUILD_TESTS)
+  add_subdirectory(tests/protocol)
   add_executable(
@@
 if(XNN_TRANSFER_BUILD_FUZZERS)
@@
   if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
     message(FATAL_ERROR "XNN_TRANSFER_BUILD_FUZZERS requires Clang")
   endif()
+
+  add_subdirectory(fuzz/protocol)
```

Place `add_subdirectory(src/protocol)` after
`xnn_transfer_enable_sanitizers()` is defined. Then run `make native-test` and
configure `XNN_TRANSFER_BUILD_FUZZERS=ON` with a Clang distribution that ships
the libFuzzer runtime. Apple Clang 21 on this host lacked
`libclang_rt.fuzzer_osx.a`; Homebrew LLVM 22.1.8 was used successfully.

## Residual risk

- Known limitation: `TranscriptParser` validates framing-level role/order and
  only the 1..64-byte finished envelope. It deliberately does not authenticate
  a peer, verify finished bytes, or establish a protected transport.
- Known limitation: message-specific transfer state, conditional field
  presence, path normalization, commitments, and flow control are outside this
  frame/TLV parser and require later state-machine/security work.
- Specification ambiguity: section 6 says the selected capability set is the
  exact sorted intersection but also says unknown advertised capabilities may
  be ignored. This implementation follows the golden oracle's exact
  intersection behavior.
- Specification ambiguity: v1.0 senders must use a 28-byte header while
  receivers apply future-field rules. The parser accepts structurally valid
  unknown noncritical header TLVs and rejects critical ones.
- Follow-up task: integration owner applies the shared CMake hooks and reviews
  how the parser is consumed by the future authenticated session state machine.

## Review focus

- Files or invariants requiring close review: checked total/TLV length
  arithmetic, UTF-8 rejection, schema criticality/duplicate/order handling,
  independent directional message IDs, capability intersection and exact
  negotiated minima, and the explicit non-security boundary around binding
  frames

## Acceptance

- Accepted by: pending integration-owner review
- Accepted at: pending
- Follow-up runtime state: `review`
