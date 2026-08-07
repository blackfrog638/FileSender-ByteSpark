#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../src/bridge/discovery_bridge.hpp"
#include "../../src/bridge/event_channel.hpp"
#include "xnn_transfer/c_api.h"
#include "xnn_transfer/core/discovery/discovery.hpp"

namespace {

using namespace std::chrono_literals;

static_assert(sizeof(std::size_t) == 8);
static_assert(sizeof(xnn_transfer_discovery_config) == 120);
static_assert(sizeof(xnn_transfer_discovery_peer) == 144);
static_assert(sizeof(xnn_transfer_discovery_peer_event_payload) == 176);
static_assert(sizeof(xnn_transfer_discovery_snapshot_page) == 1'192);
static_assert(sizeof(xnn_transfer_discovery_peer_event_payload) <=
              XNN_TRANSFER_EVENT_PAYLOAD_MAX_SIZE);

int failures = 0;

void Expect(const bool condition, const char* const message) {
  if (condition) {
    return;
  }

  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

xnn_transfer_engine* CreateEngine() {
  xnn_transfer_engine_config config{
      .struct_size = sizeof(xnn_transfer_engine_config),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .reserved = 0,
  };
  xnn_transfer_engine* engine = nullptr;
  Expect(xnn_transfer_engine_create(&config, &engine) == XNN_TRANSFER_STATUS_OK,
         "create succeeds");
  Expect(engine != nullptr, "create returns an engine");
  return engine;
}

xnn_transfer_event EmptyEvent() {
  return xnn_transfer_event{
      .struct_size = sizeof(xnn_transfer_event),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
  };
}

bool ReadStateEvent(const xnn_transfer_event& event,
                    xnn_transfer_engine_state* const out_state) {
  if (event.type != XNN_TRANSFER_EVENT_TYPE_ENGINE_STATE_CHANGED ||
      event.payload_version != XNN_TRANSFER_ENGINE_STATE_EVENT_PAYLOAD_VERSION ||
      event.payload_size != sizeof(xnn_transfer_engine_state_event_payload)) {
    return false;
  }

  xnn_transfer_engine_state_event_payload payload{};
  std::memcpy(&payload, event.payload, sizeof(payload));
  if (payload.struct_size != sizeof(payload) ||
      payload.abi_version != XNN_TRANSFER_ABI_VERSION) {
    return false;
  }

  *out_state = static_cast<xnn_transfer_engine_state>(payload.state);
  return true;
}

xnn_transfer_discovery_config DiscoveryConfig() {
  return xnn_transfer_discovery_config{
      .struct_size = sizeof(xnn_transfer_discovery_config),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .service_port = 45'879,
  };
}

xnn_transfer_discovery_snapshot_page EmptySnapshotPage() {
  return xnn_transfer_discovery_snapshot_page{
      .struct_size = sizeof(xnn_transfer_discovery_snapshot_page),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
  };
}

xnn_transfer::core::discovery::CandidateEvent MakeCandidateEvent(
    const std::uint8_t suffix, const xnn_transfer::core::discovery::EventKind kind) {
  using xnn_transfer::core::discovery::AddressFamily;
  using xnn_transfer::core::discovery::Candidate;
  using xnn_transfer::core::discovery::IpAddress;

  Candidate candidate;
  candidate.key.observer.generation = 11;
  candidate.key.observer.family = AddressFamily::kIpv4;
  candidate.key.source = IpAddress::V4({192, 0, 2, suffix});
  candidate.key.token.fill(suffix);
  candidate.service_port = static_cast<std::uint16_t>(50'000U + suffix);
  candidate.display_label = "peer-" + std::to_string(suffix);
  candidate.highest_sequence = suffix;
  return xnn_transfer::core::discovery::CandidateEvent{
      .kind = kind,
      .candidate = std::move(candidate),
  };
}

struct DrainingCallbackContext {
  xnn_transfer_engine* engine = nullptr;
  std::vector<xnn_transfer_engine_state> states;
  std::vector<uint64_t> sequences;
  int calls = 0;
};

void DrainOnWakeup(void* const user_data) {
  auto* const context = static_cast<DrainingCallbackContext*>(user_data);
  ++context->calls;

  for (;;) {
    xnn_transfer_event event = EmptyEvent();
    const xnn_transfer_status status =
        xnn_transfer_engine_poll_event(context->engine, &event);
    if (status == XNN_TRANSFER_STATUS_EVENT_QUEUE_EMPTY) {
      return;
    }
    if (status != XNN_TRANSFER_STATUS_OK) {
      ++failures;
      return;
    }

    xnn_transfer_engine_state state = XNN_TRANSFER_ENGINE_STATE_CREATED;
    if (!ReadStateEvent(event, &state)) {
      ++failures;
      return;
    }
    context->states.push_back(state);
    context->sequences.push_back(event.sequence);
  }
}

void TestStructCompatibilityAndCallerOwnedPayload() {
  struct ExtendedConfig {
    xnn_transfer_engine_config base;
    uint64_t future_field;
  };
  ExtendedConfig config{
      .base =
          {
              .struct_size = sizeof(ExtendedConfig),
              .abi_version = XNN_TRANSFER_ABI_VERSION,
              .reserved = 0,
          },
      .future_field = UINT64_C(0x123456789abcdef0),
  };
  xnn_transfer_engine* engine = nullptr;
  Expect(xnn_transfer_engine_create(&config.base, &engine) == XNN_TRANSFER_STATUS_OK,
         "create accepts a larger compatible config");
  if (engine == nullptr) {
    return;
  }

  xnn_transfer_event event = EmptyEvent();
  event.struct_size = offsetof(xnn_transfer_event, payload);
  Expect(xnn_transfer_engine_poll_event(engine, &event) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "poll rejects a short event output");

  event = EmptyEvent();
  event.abi_version = XNN_TRANSFER_ABI_VERSION + 1;
  Expect(xnn_transfer_engine_poll_event(engine, &event) ==
             XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI,
         "poll rejects an unsupported event ABI");

  event = EmptyEvent();
  Expect(xnn_transfer_engine_poll_event(engine, &event) == XNN_TRANSFER_STATUS_OK,
         "poll copies the initial event into caller-owned storage");
  Expect(event.payload_size <= XNN_TRANSFER_EVENT_PAYLOAD_MAX_SIZE,
         "event payload is bounded");

  xnn_transfer_engine_state state = XNN_TRANSFER_ENGINE_STATE_STOPPED;
  Expect(ReadStateEvent(event, &state), "initial event payload is versioned");
  Expect(state == XNN_TRANSFER_ENGINE_STATE_CREATED,
         "initial event reports the created state");
  Expect(xnn_transfer_engine_poll_event(engine, &event) ==
             XNN_TRANSFER_STATUS_EVENT_QUEUE_EMPTY,
         "poll reports an empty queue without borrowing memory");

  xnn_transfer_event_callback_config callback_config{
      .struct_size = offsetof(xnn_transfer_event_callback_config, user_data),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .reserved = 0,
      .callback = DrainOnWakeup,
      .user_data = nullptr,
  };
  Expect(xnn_transfer_engine_set_event_callback(engine, &callback_config) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "callback registration rejects a short config");

  callback_config.struct_size = sizeof(callback_config);
  callback_config.abi_version = XNN_TRANSFER_ABI_VERSION + 1;
  Expect(xnn_transfer_engine_set_event_callback(engine, &callback_config) ==
             XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI,
         "callback registration rejects an unsupported ABI");

  xnn_transfer_engine_destroy(engine);
}

void TestWakeupDrainsLifecycleEventsInOrder() {
  xnn_transfer_engine* const engine = CreateEngine();
  if (engine == nullptr) {
    return;
  }

  DrainingCallbackContext context{.engine = engine};
  xnn_transfer_event_callback_config callback_config{
      .struct_size = sizeof(xnn_transfer_event_callback_config),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .reserved = 0,
      .callback = DrainOnWakeup,
      .user_data = &context,
  };
  Expect(xnn_transfer_engine_set_event_callback(engine, &callback_config) ==
             XNN_TRANSFER_STATUS_OK,
         "callback registration succeeds");
  Expect(xnn_transfer_engine_start(engine) == XNN_TRANSFER_STATUS_OK, "start succeeds");
  Expect(xnn_transfer_engine_stop(engine) == XNN_TRANSFER_STATUS_OK, "stop succeeds");

  const std::vector<xnn_transfer_engine_state> expected_states{
      XNN_TRANSFER_ENGINE_STATE_CREATED,
      XNN_TRANSFER_ENGINE_STATE_RUNNING,
      XNN_TRANSFER_ENGINE_STATE_STOPPING,
      XNN_TRANSFER_ENGINE_STATE_STOPPED,
  };
  Expect(context.states == expected_states,
         "callback wakeups drain lifecycle events in sequence");
  Expect(context.sequences == std::vector<uint64_t>({1, 2, 3, 4}),
         "event sequence numbers are monotonic");

  const int calls_after_stop = context.calls;
  Expect(xnn_transfer_engine_set_event_callback(engine, &callback_config) ==
             XNN_TRANSFER_STATUS_INVALID_STATE,
         "shutdown rejects callback re-registration");
  std::this_thread::sleep_for(10ms);
  Expect(context.calls == calls_after_stop, "no callback starts after stop returns");

  Expect(
      xnn_transfer_engine_set_event_callback(engine, nullptr) == XNN_TRANSFER_STATUS_OK,
      "callback clearing is idempotent after shutdown");
  xnn_transfer_engine_destroy(engine);
}

struct BlockingCallbackContext {
  xnn_transfer_engine* engine = nullptr;
  std::atomic<int> calls{0};
  std::atomic<int> active{0};
  std::atomic<int> max_active{0};
  std::mutex mutex;
  std::condition_variable condition;
  bool block = false;
  bool entered = false;
  bool release = false;
};

void BlockingWakeup(void* const user_data) {
  auto* const context = static_cast<BlockingCallbackContext*>(user_data);
  context->calls.fetch_add(1);
  const int active = context->active.fetch_add(1) + 1;
  int observed_max = context->max_active.load();
  while (active > observed_max &&
         !context->max_active.compare_exchange_weak(observed_max, active)) {
  }

  xnn_transfer_event event = EmptyEvent();
  static_cast<void>(xnn_transfer_engine_poll_event(context->engine, &event));

  std::unique_lock lock(context->mutex);
  if (context->block) {
    context->entered = true;
    context->condition.notify_all();
    context->condition.wait(lock, [context] { return context->release; });
  }
  context->active.fetch_sub(1);
}

void TestStopWaitsForInFlightCallback() {
  xnn_transfer_engine* const engine = CreateEngine();
  if (engine == nullptr) {
    return;
  }

  BlockingCallbackContext context{.engine = engine};
  xnn_transfer_event_callback_config callback_config{
      .struct_size = sizeof(xnn_transfer_event_callback_config),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .reserved = 0,
      .callback = BlockingWakeup,
      .user_data = &context,
  };
  Expect(xnn_transfer_engine_set_event_callback(engine, &callback_config) ==
             XNN_TRANSFER_STATUS_OK,
         "blocking callback registration succeeds");

  {
    const std::scoped_lock lock(context.mutex);
    context.block = true;
  }
  std::thread start_thread([engine] {
    Expect(xnn_transfer_engine_start(engine) == XNN_TRANSFER_STATUS_OK,
           "start succeeds while exercising callback serialization");
  });

  {
    std::unique_lock lock(context.mutex);
    context.condition.wait(lock, [&context] { return context.entered; });
  }

  std::atomic<bool> stop_returned{false};
  std::thread stop_thread([engine, &stop_returned] {
    Expect(xnn_transfer_engine_stop(engine) == XNN_TRANSFER_STATUS_OK,
           "stop succeeds after callback barrier");
    stop_returned.store(true);
  });

  std::this_thread::sleep_for(20ms);
  Expect(!stop_returned.load(), "stop does not return while a callback is in flight");
  {
    const std::scoped_lock lock(context.mutex);
    context.release = true;
  }
  context.condition.notify_all();

  start_thread.join();
  stop_thread.join();
  Expect(stop_returned.load(), "stop returns after the callback completes");
  Expect(context.max_active.load() == 1, "callbacks are serialized");

  const int calls_after_stop = context.calls.load();
  std::this_thread::sleep_for(10ms);
  Expect(context.calls.load() == calls_after_stop,
         "the stop barrier prevents later callbacks");

  xnn_transfer_engine_destroy(engine);
}

struct LostWakeupContext {
  xnn_transfer_engine* engine = nullptr;
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<xnn_transfer_engine_state> states;
  int calls = 0;
  bool first_event_drained = false;
  bool release_first_callback = false;
};

void DrainThenBlockFirstWakeup(void* const user_data) {
  auto* const context = static_cast<LostWakeupContext*>(user_data);
  std::vector<xnn_transfer_engine_state> drained_states;
  for (;;) {
    xnn_transfer_event event = EmptyEvent();
    const xnn_transfer_status status =
        xnn_transfer_engine_poll_event(context->engine, &event);
    if (status == XNN_TRANSFER_STATUS_EVENT_QUEUE_EMPTY) {
      break;
    }
    if (status != XNN_TRANSFER_STATUS_OK) {
      ++failures;
      return;
    }

    xnn_transfer_engine_state state = XNN_TRANSFER_ENGINE_STATE_STOPPED;
    if (!ReadStateEvent(event, &state)) {
      ++failures;
      return;
    }
    drained_states.push_back(state);
  }

  std::unique_lock lock(context->mutex);
  ++context->calls;
  context->states.insert(context->states.end(), drained_states.begin(),
                         drained_states.end());
  if (context->calls == 1) {
    context->first_event_drained = true;
    context->condition.notify_all();
    context->condition.wait(lock,
                            [context] { return context->release_first_callback; });
  }
}

void TestEnqueueAfterDrainBeforeCallbackReturnWakesAgain() {
  xnn_transfer_engine* const engine = CreateEngine();
  if (engine == nullptr) {
    return;
  }

  LostWakeupContext context{.engine = engine};
  xnn_transfer_event_callback_config callback_config{
      .struct_size = sizeof(xnn_transfer_event_callback_config),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .reserved = 0,
      .callback = DrainThenBlockFirstWakeup,
      .user_data = &context,
  };
  std::atomic<xnn_transfer_status> registration_status{
      XNN_TRANSFER_STATUS_INTERNAL_ERROR};
  std::thread registration_thread([engine, &callback_config, &registration_status] {
    registration_status.store(
        xnn_transfer_engine_set_event_callback(engine, &callback_config));
  });

  {
    std::unique_lock lock(context.mutex);
    context.condition.wait(lock, [&context] { return context.first_event_drained; });
  }

  Expect(xnn_transfer_engine_start(engine) == XNN_TRANSFER_STATUS_OK,
         "enqueue succeeds after the active callback drains to empty");
  {
    const std::scoped_lock lock(context.mutex);
    context.release_first_callback = true;
  }
  context.condition.notify_all();
  registration_thread.join();

  Expect(registration_status.load() == XNN_TRANSFER_STATUS_OK,
         "callback registration completes after pending redispatch");
  {
    const std::scoped_lock lock(context.mutex);
    const std::vector<xnn_transfer_engine_state> expected_states{
        XNN_TRANSFER_ENGINE_STATE_CREATED,
        XNN_TRANSFER_ENGINE_STATE_RUNNING,
    };
    Expect(context.calls == 2, "empty-to-nonempty race schedules a second wakeup");
    Expect(context.states == expected_states,
           "redispatched callback drains the concurrently queued event");
  }

  Expect(
      xnn_transfer_engine_set_event_callback(engine, nullptr) == XNN_TRANSFER_STATUS_OK,
      "callback clears after pending redispatch");
  Expect(xnn_transfer_engine_stop(engine) == XNN_TRANSFER_STATUS_OK,
         "stop succeeds after lost-wakeup regression");
  xnn_transfer_engine_destroy(engine);
}

void TestStopDispatchesPendingWakeupBeforeBarrier() {
  xnn_transfer_engine* const engine = CreateEngine();
  if (engine == nullptr) {
    return;
  }

  LostWakeupContext context{.engine = engine};
  xnn_transfer_event_callback_config callback_config{
      .struct_size = sizeof(xnn_transfer_event_callback_config),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .reserved = 0,
      .callback = DrainThenBlockFirstWakeup,
      .user_data = &context,
  };
  std::atomic<xnn_transfer_status> registration_status{
      XNN_TRANSFER_STATUS_INTERNAL_ERROR};
  std::thread registration_thread([engine, &callback_config, &registration_status] {
    registration_status.store(
        xnn_transfer_engine_set_event_callback(engine, &callback_config));
  });

  {
    std::unique_lock lock(context.mutex);
    context.condition.wait(lock, [&context] { return context.first_event_drained; });
  }

  std::atomic<bool> stop_returned{false};
  std::thread stop_thread([engine, &stop_returned] {
    Expect(xnn_transfer_engine_stop(engine) == XNN_TRANSFER_STATUS_OK,
           "stop succeeds after pending callback redispatch");
    stop_returned.store(true);
  });

  xnn_transfer_engine_state state = XNN_TRANSFER_ENGINE_STATE_CREATED;
  while (state != XNN_TRANSFER_ENGINE_STATE_STOPPED) {
    Expect(xnn_transfer_engine_get_state(engine, &state) == XNN_TRANSFER_STATUS_OK,
           "state remains queryable during stop");
    std::this_thread::yield();
  }
  Expect(!stop_returned.load(),
         "stop barrier waits for the callback that drained to empty");

  {
    const std::scoped_lock lock(context.mutex);
    context.release_first_callback = true;
  }
  context.condition.notify_all();
  registration_thread.join();
  stop_thread.join();

  Expect(registration_status.load() == XNN_TRANSFER_STATUS_OK,
         "registration completes before stop releases callback ownership");
  {
    const std::scoped_lock lock(context.mutex);
    const std::vector<xnn_transfer_engine_state> expected_states{
        XNN_TRANSFER_ENGINE_STATE_CREATED,
        XNN_TRANSFER_ENGINE_STATE_STOPPING,
        XNN_TRANSFER_ENGINE_STATE_STOPPED,
    };
    Expect(context.calls == 2,
           "stop redispatches the pending wakeup before closing callbacks");
    Expect(context.states == expected_states,
           "pending stop wakeup drains stopping and stopped events");
  }

  xnn_transfer_engine_destroy(engine);
}

struct ReentrantUnregisterContext {
  xnn_transfer_engine* engine = nullptr;
  xnn_transfer_status poll_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  xnn_transfer_status start_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  xnn_transfer_status stop_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  xnn_transfer_status discovery_start_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  xnn_transfer_status discovery_stop_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  xnn_transfer_status snapshot_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  xnn_transfer_status unregister_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  int calls = 0;
};

void PollAndUnregister(void* const user_data) {
  auto* const context = static_cast<ReentrantUnregisterContext*>(user_data);
  ++context->calls;
  xnn_transfer_event event = EmptyEvent();
  context->poll_status = xnn_transfer_engine_poll_event(context->engine, &event);
  context->start_status = xnn_transfer_engine_start(context->engine);
  context->stop_status = xnn_transfer_engine_stop(context->engine);
  xnn_transfer_discovery_config config = DiscoveryConfig();
  context->discovery_start_status =
      xnn_transfer_discovery_start(context->engine, &config);
  context->discovery_stop_status = xnn_transfer_discovery_stop(context->engine);
  xnn_transfer_discovery_snapshot_page page = EmptySnapshotPage();
  context->snapshot_status =
      xnn_transfer_discovery_get_snapshot(context->engine, 0, 0, &page);
  context->unregister_status =
      xnn_transfer_engine_set_event_callback(context->engine, nullptr);
}

void TestDocumentedCallbackReentrancy() {
  xnn_transfer_engine* const engine = CreateEngine();
  if (engine == nullptr) {
    return;
  }
  Expect(xnn_transfer_engine_start(engine) == XNN_TRANSFER_STATUS_OK,
         "engine starts before reentrant operation checks");

  ReentrantUnregisterContext context{.engine = engine};
  xnn_transfer_event_callback_config callback_config{
      .struct_size = sizeof(xnn_transfer_event_callback_config),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .reserved = 0,
      .callback = PollAndUnregister,
      .user_data = &context,
  };
  Expect(xnn_transfer_engine_set_event_callback(engine, &callback_config) ==
             XNN_TRANSFER_STATUS_OK,
         "callback may unregister itself");
  Expect(context.poll_status == XNN_TRANSFER_STATUS_OK, "callback may reenter poll");
  Expect(context.start_status == XNN_TRANSFER_STATUS_INVALID_STATE,
         "callback may not reenter start");
  Expect(context.stop_status == XNN_TRANSFER_STATUS_INVALID_STATE,
         "callback may not reenter stop");
  Expect(context.discovery_start_status == XNN_TRANSFER_STATUS_INVALID_STATE,
         "callback may not reenter discovery start");
  Expect(context.discovery_stop_status == XNN_TRANSFER_STATUS_INVALID_STATE,
         "callback may not reenter discovery stop");
  Expect(context.snapshot_status == XNN_TRANSFER_STATUS_INVALID_STATE,
         "callback may not reenter discovery snapshot");
  Expect(context.unregister_status == XNN_TRANSFER_STATUS_OK,
         "reentrant unregister does not deadlock");

  Expect(xnn_transfer_engine_start(engine) == XNN_TRANSFER_STATUS_OK,
         "idempotent start succeeds after unregister");
  Expect(context.calls == 1, "unregistered callback is not invoked again");
  Expect(xnn_transfer_engine_stop(engine) == XNN_TRANSFER_STATUS_OK,
         "stop succeeds without a callback");
  xnn_transfer_engine_destroy(engine);
}

void TestDiscoveryStructsAndLifecycleStates() {
  xnn_transfer_engine* const engine = CreateEngine();
  if (engine == nullptr) {
    return;
  }

  xnn_transfer_discovery_config config = DiscoveryConfig();
  Expect(xnn_transfer_discovery_start(engine, &config) ==
             XNN_TRANSFER_STATUS_INVALID_STATE,
         "discovery cannot start before the engine");

  config.struct_size = offsetof(xnn_transfer_discovery_config, display_label);
  Expect(xnn_transfer_discovery_start(engine, &config) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "discovery rejects a short config");

  config = DiscoveryConfig();
  config.abi_version = XNN_TRANSFER_ABI_VERSION + 1;
  Expect(xnn_transfer_discovery_start(engine, &config) ==
             XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI,
         "discovery rejects an unsupported config ABI");

  config = DiscoveryConfig();
  config.display_label_size = XNN_TRANSFER_DISCOVERY_DISPLAY_LABEL_MAX_SIZE + 1;
  Expect(xnn_transfer_discovery_start(engine, &config) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "discovery rejects an oversized display label");
  config = DiscoveryConfig();
  config.display_label_size = 1;
  config.display_label[0] = 0xff;
  Expect(xnn_transfer_discovery_start(engine, &config) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "discovery rejects a malformed UTF-8 display label");
  config = DiscoveryConfig();
  config.reserved = 1;
  Expect(xnn_transfer_discovery_start(engine, &config) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "discovery rejects nonzero reserved input");

  Expect(xnn_transfer_engine_start(engine) == XNN_TRANSFER_STATUS_OK,
         "engine starts before discovery");
  config = DiscoveryConfig();
  Expect(xnn_transfer_discovery_start(engine, &config) == XNN_TRANSFER_STATUS_OK,
         "discovery starts with a valid copied config");
  Expect(xnn_transfer_discovery_start(engine, &config) ==
             XNN_TRANSFER_STATUS_INVALID_STATE,
         "discovery rejects a duplicate start");

  xnn_transfer_discovery_snapshot_page page = EmptySnapshotPage();
  page.struct_size = offsetof(xnn_transfer_discovery_snapshot_page, peers);
  Expect(xnn_transfer_discovery_get_snapshot(engine, 0, 0, &page) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "snapshot rejects a short output page");

  page = EmptySnapshotPage();
  page.abi_version = XNN_TRANSFER_ABI_VERSION + 1;
  Expect(xnn_transfer_discovery_get_snapshot(engine, 0, 0, &page) ==
             XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI,
         "snapshot rejects an unsupported ABI");

  page = EmptySnapshotPage();
  Expect(xnn_transfer_discovery_get_snapshot(engine, 0, 0, &page) ==
             XNN_TRANSFER_STATUS_OK,
         "snapshot returns an empty bounded page");
  Expect(page.snapshot_revision != 0, "snapshot revision is nonzero");
  Expect(page.count == 0 && page.total_count == 0,
         "initial discovery snapshot is empty");

  Expect(xnn_transfer_discovery_stop(engine) == XNN_TRANSFER_STATUS_OK,
         "discovery stop succeeds");
  Expect(xnn_transfer_discovery_stop(engine) == XNN_TRANSFER_STATUS_OK,
         "discovery stop is idempotent");
  Expect(xnn_transfer_engine_stop(engine) == XNN_TRANSFER_STATUS_OK,
         "engine stops after discovery");
  config = DiscoveryConfig();
  Expect(xnn_transfer_discovery_start(engine, &config) ==
             XNN_TRANSFER_STATUS_INVALID_STATE,
         "discovery cannot restart after engine shutdown");
  page = EmptySnapshotPage();
  Expect(xnn_transfer_discovery_get_snapshot(engine, 0, 0, &page) ==
             XNN_TRANSFER_STATUS_OK,
         "stopped engine retains bounded empty snapshot recovery");
  Expect(page.count == 0 && page.total_count == 0,
         "stopped discovery snapshot is empty");
  xnn_transfer_engine_destroy(engine);
}

void TestDiscoveryRegistryPaginationAndStaleRevision() {
  using xnn_transfer::bridge::DiscoveryPeerRegistry;
  using xnn_transfer::core::discovery::EventKind;

  DiscoveryPeerRegistry registry;
  std::vector<xnn_transfer_discovery_peer_event_payload> events;
  for (std::uint8_t suffix = 1;
       suffix <= XNN_TRANSFER_DISCOVERY_SNAPSHOT_PAGE_CAPACITY + 2; ++suffix) {
    std::optional<xnn_transfer_discovery_peer_event_payload> event =
        registry.Apply(MakeCandidateEvent(suffix, EventKind::kAppeared));
    Expect(event.has_value(), "appeared candidate produces an ABI event");
    if (event.has_value()) {
      events.push_back(*event);
    }
  }

  Expect(events.size() == XNN_TRANSFER_DISCOVERY_SNAPSHOT_PAGE_CAPACITY + 2,
         "registry retains all bounded candidates");
  if (!events.empty()) {
    Expect(events.front().peer.peer_id != 0, "peer IDs are opaque and nonzero");
    Expect(events.front().peer.address_size == 4,
           "IPv4 peers copy exactly four address bytes");
    Expect(events.front().peer.display_label_size == 6,
           "peer labels carry an explicit bounded length");
  }

  xnn_transfer_discovery_snapshot_page first = EmptySnapshotPage();
  Expect(registry.Snapshot(0, 0, &first) == XNN_TRANSFER_STATUS_OK,
         "first snapshot page succeeds");
  Expect(first.count == XNN_TRANSFER_DISCOVERY_SNAPSHOT_PAGE_CAPACITY,
         "first snapshot page is capacity bounded");
  Expect(first.total_count == events.size(),
         "snapshot reports total peer count separately from page count");

  const std::uint64_t first_revision = first.snapshot_revision;
  auto updated = MakeCandidateEvent(1, EventKind::kUpdated);
  updated.candidate.display_label = "updated";
  Expect(registry.Apply(updated).has_value(), "updated candidate produces an event");

  xnn_transfer_discovery_snapshot_page stale = EmptySnapshotPage();
  Expect(
      registry.Snapshot(first_revision, XNN_TRANSFER_DISCOVERY_SNAPSHOT_PAGE_CAPACITY,
                        &stale) == XNN_TRANSFER_STATUS_STALE_SNAPSHOT,
      "pagination rejects a revision changed between pages");

  xnn_transfer_discovery_snapshot_page refreshed_first = EmptySnapshotPage();
  Expect(registry.Snapshot(0, 0, &refreshed_first) == XNN_TRANSFER_STATUS_OK,
         "snapshot recovery restarts at offset zero");
  xnn_transfer_discovery_snapshot_page refreshed = EmptySnapshotPage();
  Expect(registry.Snapshot(refreshed_first.snapshot_revision,
                           XNN_TRANSFER_DISCOVERY_SNAPSHOT_PAGE_CAPACITY,
                           &refreshed) == XNN_TRANSFER_STATUS_OK,
         "snapshot recovery continues with the current revision");
  Expect(refreshed.count == 2, "final snapshot page contains the remaining peers");

  std::optional<xnn_transfer_discovery_peer_event_payload> expired =
      registry.Apply(MakeCandidateEvent(1, EventKind::kExpired));
  Expect(expired.has_value(), "expiry produces a final copied peer payload");
  if (expired.has_value()) {
    Expect(expired->change == XNN_TRANSFER_DISCOVERY_PEER_EXPIRED,
           "expiry event has the expired change kind");
    Expect(expired->peer.peer_id == events.front().peer.peer_id,
           "expiry retains the appeared peer ID");
  }
}

void TestDiscoveryQueueOverflowIsObservable() {
  using xnn_transfer::bridge::DiscoveryPeerRegistry;
  using xnn_transfer::bridge::EventChannel;
  using xnn_transfer::core::discovery::EventKind;

  EventChannel channel;
  DiscoveryPeerRegistry registry;
  for (std::uint8_t suffix = 1; suffix <= XNN_TRANSFER_EVENT_QUEUE_CAPACITY + 1;
       ++suffix) {
    std::optional<xnn_transfer_discovery_peer_event_payload> payload =
        registry.Apply(MakeCandidateEvent(suffix, EventKind::kAppeared));
    Expect(payload.has_value(), "overflow fixture produces a peer payload");
    if (payload.has_value()) {
      channel.EnqueueDiscovery(*payload);
    }
  }

  std::size_t count = 0;
  bool observed_drop = false;
  std::uint64_t first_sequence = 0;
  for (;;) {
    xnn_transfer_event event = EmptyEvent();
    const xnn_transfer_status status = channel.Poll(&event);
    if (status == XNN_TRANSFER_STATUS_EVENT_QUEUE_EMPTY) {
      break;
    }
    Expect(status == XNN_TRANSFER_STATUS_OK, "overflow queue remains pollable");
    if (count == 0) {
      first_sequence = event.sequence;
    }
    observed_drop = observed_drop ||
                    (event.flags & XNN_TRANSFER_EVENT_FLAG_EVENTS_DROPPED_BEFORE) != 0;
    ++count;
  }

  Expect(count == XNN_TRANSFER_EVENT_QUEUE_CAPACITY,
         "event queue retains exactly its fixed capacity");
  Expect(first_sequence == 2, "event queue drops the oldest event");
  Expect(observed_drop, "event queue exposes overflow to the caller");
}

void TestDiscoveryShutdownRaceIsBounded() {
  xnn_transfer_engine* const engine = CreateEngine();
  if (engine == nullptr) {
    return;
  }
  Expect(xnn_transfer_engine_start(engine) == XNN_TRANSFER_STATUS_OK,
         "engine starts for discovery shutdown race");
  xnn_transfer_discovery_config config = DiscoveryConfig();
  Expect(xnn_transfer_discovery_start(engine, &config) == XNN_TRANSFER_STATUS_OK,
         "discovery starts for shutdown race");

  std::atomic<bool> stop{false};
  std::atomic<int> unexpected{0};
  std::thread snapshot_thread([engine, &stop, &unexpected] {
    while (!stop.load()) {
      xnn_transfer_discovery_snapshot_page page = EmptySnapshotPage();
      const xnn_transfer_status status =
          xnn_transfer_discovery_get_snapshot(engine, 0, 0, &page);
      if (status != XNN_TRANSFER_STATUS_OK &&
          status != XNN_TRANSFER_STATUS_INVALID_STATE) {
        unexpected.fetch_add(1);
      }
    }
  });

  Expect(xnn_transfer_engine_stop(engine) == XNN_TRANSFER_STATUS_OK,
         "engine stop is a discovery callback barrier");
  stop.store(true);
  snapshot_thread.join();
  Expect(unexpected.load() == 0,
         "snapshot race has only documented success or stopped outcomes");
  xnn_transfer_engine_destroy(engine);
}

}  // namespace

int main() {
  TestStructCompatibilityAndCallerOwnedPayload();
  TestWakeupDrainsLifecycleEventsInOrder();
  TestStopWaitsForInFlightCallback();
  TestEnqueueAfterDrainBeforeCallbackReturnWakesAgain();
  TestStopDispatchesPendingWakeupBeforeBarrier();
  TestDocumentedCallbackReentrancy();
  TestDiscoveryStructsAndLifecycleStates();
  TestDiscoveryRegistryPaginationAndStaleRevision();
  TestDiscoveryQueueOverflowIsObservable();
  TestDiscoveryShutdownRaceIsBounded();

  if (failures != 0) {
    std::cerr << failures << " bridge event test(s) failed\n";
    return 1;
  }

  std::cout << "All bridge event tests passed\n";
  return 0;
}
