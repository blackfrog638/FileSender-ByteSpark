# ADR 0003: Asynchronous native event ABI

- Status: accepted
- Date: 2026-08-06
- Extends: ADR 0001

## Context

Native networking, protocol, and file operations will produce events after the
Flutter call that initiated an operation has returned. Dart FFI callbacks may
run later than the native callback invocation, so callback-scoped pointers
cannot safely carry event payloads across that boundary.

The event boundary must also remain bounded under a stalled UI, define shutdown
behavior, and preserve existing ABI v1 clients.

## Decision

Keep `XNN_TRANSFER_ABI_VERSION` at `1` and add the event symbols as an additive
extension. Existing ABI v1 symbols, values, and struct layouts are unchanged.
Adapters that require events must feature-detect both event symbols before use;
an ABI v1 library without them is incompatible with that adapter.

Each engine owns a queue of at most 64 events. An event contains a monotonically
increasing sequence, a versioned type, flags, and at most 256 inline payload
bytes. On overflow, native code drops the oldest event and marks the newly
queued event with `XNN_TRANSFER_EVENT_FLAG_EVENTS_DROPPED_BEFORE`.

The callback is a wakeup only:

- It receives borrowed `user_data` but no event or payload pointer.
- The caller drains events with `xnn_transfer_engine_poll_event`.
- Polling copies an event into caller-owned storage.
- Callbacks are serialized per engine and may run synchronously on any native
  thread that queues an event.
- A callback may query state, poll events, or unregister itself. It must not
  call start, stop, or destroy.

Callback registration copies the callback and `user_data` values. The caller
keeps `user_data` valid until unregister returns. Unregister and stop are
barriers for callbacks already in flight. Stop closes registration, publishes
`STOPPING` and `STOPPED`, and guarantees that no native callback starts after
stop returns. Destroy is externally serialized against all engine calls,
performs the same barrier, discards queued events, and never invokes a callback
after returning.

The Dart adapter uses `NativeCallable.listener` for cross-thread wakeups. Its
handler later polls into Dart-owned FFI storage and copies the inline payload.
Dispose unregisters the native callback before closing `NativeCallable`.

## Consequences

- No callback-scoped native pointer crosses the delayed Dart listener boundary.
- Memory use remains fixed when presentation is stalled.
- Event loss is explicit and forces higher layers to reconcile state.
- Slow native callbacks can delay the event-producing call; callbacks must only
  signal or drain bounded work.
- ABI v1 identifies the compatible base ABI, not the presence of every additive
  symbol. New adapters must check required exports.
- The current event producer reports lifecycle state only. This ADR does not
  claim discovery, networking, security, or file transfer behavior.
