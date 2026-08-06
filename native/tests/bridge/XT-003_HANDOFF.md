# XT-003 agent handoff

## Delivered

- Task: XT-003, Add the asynchronous native event ABI
- From owner: native-bridge-agent
- To owner or reviewer: integration owner
- Branch: `task/XT-003`
- Worktree: `/Users/bytedance/XnnTransfer/XnnTransfer-XT-003`
- Base SHA: `a1870f9d91ef8f518782b1b307e7f213640e3771`
- Head SHA: this commit
- Owned paths: `native/include/xnn_transfer/c_api.h`,
  `native/src/bridge/**`, `native/tests/bridge/**`, and
  `apps/desktop/lib/core/native/**`
- Observable behavior: engine creation, running, stopping, and stopped states
  are queued as bounded native lifecycle events. This does not implement or
  claim discovery, networking, security, persistence, or file transfer.

## Contracts

- `XNN_TRANSFER_ABI_VERSION` remains `1`. The change is additive: existing
  values and symbols are unchanged, while old ABI v1 clients continue to work
  with the new library.
- Added `xnn_transfer_engine_set_event_callback` and
  `xnn_transfer_engine_poll_event`.
- Added extensible callback, event, and state-payload structs with
  `struct_size` and `abi_version`. Larger structs are accepted; undersized or
  incompatible structs fail before queue mutation.
- Payloads are inline and capped at 256 bytes. The native queue is capped at 64
  events. Overflow drops the oldest event and marks the new event with
  `XNN_TRANSFER_EVENT_FLAG_EVENTS_DROPPED_BEFORE`.
- The callback has no event or payload argument. It is only a serialized wakeup
  to drain the queue. Native owns queued data; `poll_event` copies it into
  caller-owned storage.
- Callback `user_data` is borrowed until callback clearing returns. Clearing
  and stop wait for callbacks in flight. No native callback begins after stop
  or destroy returns.
- Callback calls may occur synchronously on the event-producing native thread.
  Callback reentry is limited to state query, event polling, and callback
  clearing. Reentrant start and stop return `INVALID_STATE`; reentrant destroy
  is forbidden. Destroy must be externally serialized against API calls.
- Stop now exposes `STOPPING` before `STOPPED`. Events remain pollable after
  stop until destroy.
- `NativeCallable.listener` receives no ephemeral pointer. Its later Dart
  handler allocates a Dart-owned FFI event struct and drains copied payloads.
  Dispose clears the native callback before closing `NativeCallable`.
- ADR 0001 supports opaque handles, explicit ownership, ABI versioning, and
  extensible structs, so this uses an additive ABI v1 extension. ADR 0001 also
  requires coordinated contract review. Integration-owner review and a new
  asynchronous-event ADR are required before acceptance.

## Verification evidence

- Command: direct AppleClang build and run of
  `native/tests/bridge/c_api_event_test.cpp` with C++20 and repository warning
  flags.
- Result: passed. Covers ABI compatibility, caller-owned payload copies,
  lifecycle ordering, callback serialization, shutdown barrier, and documented
  reentry. Deterministic lost-wakeup tests cover both normal enqueue and stop
  barrier redispatch after an active callback drains to empty; the suite passed
  in 100 consecutive runs.
- Command: the same focused C++ test with
  `-fsanitize=address,undefined`.
- Result: passed.
- Command:
  `XNN_TRANSFER_LIBRARY_PATH=.../out/build/dev/native/libxnn_transfer_core.dylib
  tool/harness/sdk.sh flutter test
  lib/core/native/test/native_engine_event_test.dart`
- Result: 2 tests passed against the real dynamic library, including delayed
  `NativeCallable.listener` drain and idempotent dispose.
- Command: `tool/harness/sdk.sh flutter analyze`
- Result: no issues found.
- Command: `make verify`
- Result: all repository gates passed; existing native CTest 1/1 and existing
  Flutter tests 24/24 passed.
- Limitation: the repository gate does not discover the two new focused test
  files because shared CMake and standard Flutter test paths were outside this
  task's ownership.
- Temporary test location:
  `apps/desktop/lib/core/native/test/native_engine_event_test.dart` is under
  `lib/` only because XT-003 could not edit `apps/desktop/test/**`. It is not a
  production-code placement and must move during integration.

## Shared-file integration suggestions

1. In `native/CMakeLists.txt`, inside `if(XNN_TRANSFER_BUILD_TESTS)`, add:

   ```cmake
   add_executable(
     xnn_transfer_bridge_event_tests
     tests/bridge/c_api_event_test.cpp
   )
   target_link_libraries(
     xnn_transfer_bridge_event_tests
     PRIVATE xnn_transfer_core
   )
   if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
     xnn_transfer_enable_sanitizers(xnn_transfer_bridge_event_tests)
   endif()
   add_test(
     NAME xnn_transfer_bridge_event_tests
     COMMAND xnn_transfer_bridge_event_tests
   )
   ```

2. Move (do not copy)
   `apps/desktop/lib/core/native/test/native_engine_event_test.dart` to
   `apps/desktop/test/core/native/native_engine_event_test.dart` so normal
   Flutter discovery owns it. In `tool/harness/verify.sh`, after native build
   and before the general Flutter tests, run that test with
   `XNN_TRANSFER_LIBRARY_PATH` set to the platform library under
   `out/build/dev/native/`.
3. Add `docs/adr/0003-asynchronous-native-event-abi.md` recording the additive
   ABI v1 choice, wakeup/drain ownership, callback thread and serialization
   semantics, 256-byte payload and 64-event queue limits, overflow policy,
   shutdown barrier, and reentry/destroy exclusions. Obtain integration-owner
   approval for the public header.

## Residual risk

- ABI v1 preserves old-client/new-library compatibility, but the existing
  single integer version cannot advertise optional symbols. This new adapter
  requires a library build that exports the two event symbols and will reject
  an older ABI v1 library at symbol lookup.
- The actual FFI test ran on macOS arm64 only. Windows and Linux struct layout,
  symbol loading, and callback shutdown still need CI coverage.
- Queue overflow is implemented and signaled but cannot be driven through the
  current lifecycle-only public producer, which emits at most four events.
  Future event producers should add a 65-event overflow test.
- A queued Dart listener message may execute after the native wakeup and stop
  have returned. It carries no native pointer. Explicit stop keeps the engine
  alive for drain; dispose marks the adapter disposed before destroying it.

## Review focus

- Callback ownership and the `NativeCallable.listener` wakeup-only boundary.
- `EventChannel` lock ordering, callback self-unregister, and stop barrier.
- ABI v1 additive compatibility and the inability to feature-detect older v1
  libraries.
- Platform FFI struct layout for inline payloads.
