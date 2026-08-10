#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../../src/bridge/discovery_bridge.hpp"
#include "../../src/bridge/event_channel.hpp"
#include "../../src/bridge/pairing_bridge.hpp"
#include "../../src/bridge/transfer_bridge.hpp"
#include "xnn_transfer/c_api.h"
#include "xnn_transfer/core/discovery/discovery.hpp"
#include "xnn_transfer/core/session/session.hpp"
#include "xnn_transfer/core/transfer/transfer.hpp"

namespace {

using namespace std::chrono_literals;
namespace identity = xnn_transfer::core::security::identity;
namespace session = xnn_transfer::core::session;
namespace tls = xnn_transfer::core::security::tls;
namespace transfer = xnn_transfer::core::transfer;

static_assert(sizeof(std::size_t) == 8);
static_assert(sizeof(xnn_transfer_discovery_config) == 120);
static_assert(sizeof(xnn_transfer_discovery_peer) == 144);
static_assert(sizeof(xnn_transfer_discovery_peer_event_payload) == 176);
static_assert(sizeof(xnn_transfer_discovery_snapshot_page) == 1'192);
static_assert(sizeof(xnn_transfer_discovery_peer_event_payload) <=
              XNN_TRANSFER_EVENT_PAYLOAD_MAX_SIZE);
static_assert(sizeof(xnn_transfer_pairing_window_config) == 24);
static_assert(sizeof(xnn_transfer_pairing_start_request) == 24);
static_assert(sizeof(xnn_transfer_pairing_attempt_ref) == 32);
static_assert(sizeof(xnn_transfer_trust_ref) == 24);
static_assert(sizeof(xnn_transfer_pairing_attempt_event_payload) == 64);
static_assert(sizeof(xnn_transfer_trust_event_payload) == 40);
static_assert(sizeof(xnn_transfer_pairing_snapshot) == 96);
static_assert(sizeof(xnn_transfer_trust_snapshot_page) == 360);
static_assert(sizeof(xnn_transfer_pairing_attempt_event_payload) <=
              XNN_TRANSFER_EVENT_PAYLOAD_MAX_SIZE);
static_assert(sizeof(xnn_transfer_trust_event_payload) <=
              XNN_TRANSFER_EVENT_PAYLOAD_MAX_SIZE);
static_assert(sizeof(xnn_transfer_transfer_send_request) == 1'056);
static_assert(sizeof(xnn_transfer_transfer_ref) == 32);
static_assert(sizeof(xnn_transfer_transfer_event_payload) == 176);
static_assert(sizeof(xnn_transfer_transfer_snapshot_page) == 744);
static_assert(sizeof(xnn_transfer_transfer_event_payload) <=
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

xnn_transfer_pairing_window_config PairingWindowConfig() {
  return xnn_transfer_pairing_window_config{
      .struct_size = sizeof(xnn_transfer_pairing_window_config),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .duration_ms = 60'000,
  };
}

xnn_transfer_pairing_start_request PairingStartRequest(const std::uint64_t peer_id) {
  return xnn_transfer_pairing_start_request{
      .struct_size = sizeof(xnn_transfer_pairing_start_request),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .peer_id = peer_id,
  };
}

xnn_transfer_pairing_attempt_ref PairingAttemptRef(const std::uint8_t value) {
  xnn_transfer_pairing_attempt_ref attempt{
      .struct_size = sizeof(xnn_transfer_pairing_attempt_ref),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
  };
  std::fill(std::begin(attempt.attempt_id), std::end(attempt.attempt_id), value);
  return attempt;
}

xnn_transfer_pairing_snapshot EmptyPairingSnapshot() {
  return xnn_transfer_pairing_snapshot{
      .struct_size = sizeof(xnn_transfer_pairing_snapshot),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
  };
}

xnn_transfer_trust_snapshot_page EmptyTrustSnapshotPage() {
  return xnn_transfer_trust_snapshot_page{
      .struct_size = sizeof(xnn_transfer_trust_snapshot_page),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
  };
}

xnn_transfer_transfer_send_request TransferSendRequest(const std::uint64_t trust_id,
                                                       const std::string_view path) {
  xnn_transfer_transfer_send_request request{
      .struct_size = sizeof(xnn_transfer_transfer_send_request),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .trust_id = trust_id,
      .path_size = static_cast<std::uint32_t>(path.size()),
  };
  std::copy(path.begin(), path.end(), request.path);
  return request;
}

xnn_transfer_transfer_ref TransferRef(const std::uint8_t value) {
  xnn_transfer_transfer_ref transfer_ref{
      .struct_size = sizeof(xnn_transfer_transfer_ref),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
  };
  std::fill(std::begin(transfer_ref.transfer_id), std::end(transfer_ref.transfer_id),
            value);
  return transfer_ref;
}

xnn_transfer_transfer_snapshot_page EmptyTransferSnapshotPage() {
  return xnn_transfer_transfer_snapshot_page{
      .struct_size = sizeof(xnn_transfer_transfer_snapshot_page),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
  };
}

transfer::TransferId TransferId(const std::uint8_t value) {
  transfer::TransferId transfer_id{};
  transfer_id.fill(value);
  return transfer_id;
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
  xnn_transfer_status pairing_open_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  xnn_transfer_status pairing_close_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  xnn_transfer_status pairing_start_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  xnn_transfer_status pairing_confirm_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  xnn_transfer_status pairing_reject_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  xnn_transfer_status pairing_revoke_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  xnn_transfer_status pairing_snapshot_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  xnn_transfer_status trust_snapshot_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  xnn_transfer_status transfer_send_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  xnn_transfer_status transfer_accept_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  xnn_transfer_status transfer_reject_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  xnn_transfer_status transfer_cancel_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  xnn_transfer_status transfer_snapshot_status = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
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
  xnn_transfer_pairing_window_config window = PairingWindowConfig();
  context->pairing_open_status =
      xnn_transfer_pairing_open_window(context->engine, &window);
  context->pairing_close_status = xnn_transfer_pairing_close_window(context->engine);
  xnn_transfer_pairing_start_request start = PairingStartRequest(1);
  context->pairing_start_status = xnn_transfer_pairing_start(context->engine, &start);
  xnn_transfer_pairing_attempt_ref attempt = PairingAttemptRef(1);
  context->pairing_confirm_status =
      xnn_transfer_pairing_confirm(context->engine, &attempt);
  context->pairing_reject_status =
      xnn_transfer_pairing_reject(context->engine, &attempt);
  xnn_transfer_trust_ref trust{
      .struct_size = sizeof(xnn_transfer_trust_ref),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .trust_id = 1,
  };
  context->pairing_revoke_status = xnn_transfer_pairing_revoke(context->engine, &trust);
  xnn_transfer_pairing_snapshot pairing = EmptyPairingSnapshot();
  context->pairing_snapshot_status =
      xnn_transfer_pairing_get_snapshot(context->engine, &pairing);
  xnn_transfer_trust_snapshot_page trust_page = EmptyTrustSnapshotPage();
  context->trust_snapshot_status =
      xnn_transfer_trust_get_snapshot(context->engine, 0, 0, &trust_page);
  xnn_transfer_transfer_send_request send = TransferSendRequest(1, "/tmp/file");
  xnn_transfer_transfer_ref transfer_ref = TransferRef(1);
  context->transfer_send_status =
      xnn_transfer_transfer_send(context->engine, &send, &transfer_ref);
  context->transfer_accept_status =
      xnn_transfer_transfer_accept(context->engine, &transfer_ref);
  context->transfer_reject_status =
      xnn_transfer_transfer_reject(context->engine, &transfer_ref);
  context->transfer_cancel_status =
      xnn_transfer_transfer_cancel(context->engine, &transfer_ref);
  xnn_transfer_transfer_snapshot_page transfer_page = EmptyTransferSnapshotPage();
  context->transfer_snapshot_status =
      xnn_transfer_transfer_get_snapshot(context->engine, 0, 0, &transfer_page);
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
  Expect(context.pairing_open_status == XNN_TRANSFER_STATUS_INVALID_STATE,
         "callback may not reenter pairing window open");
  Expect(context.pairing_close_status == XNN_TRANSFER_STATUS_INVALID_STATE,
         "callback may not reenter pairing window close");
  Expect(context.pairing_start_status == XNN_TRANSFER_STATUS_INVALID_STATE,
         "callback may not reenter pairing start");
  Expect(context.pairing_confirm_status == XNN_TRANSFER_STATUS_INVALID_STATE,
         "callback may not reenter pairing confirmation");
  Expect(context.pairing_reject_status == XNN_TRANSFER_STATUS_INVALID_STATE,
         "callback may not reenter pairing rejection");
  Expect(context.pairing_revoke_status == XNN_TRANSFER_STATUS_INVALID_STATE,
         "callback may not reenter pairing revocation");
  Expect(context.pairing_snapshot_status == XNN_TRANSFER_STATUS_INVALID_STATE,
         "callback may not reenter pairing snapshot");
  Expect(context.trust_snapshot_status == XNN_TRANSFER_STATUS_INVALID_STATE,
         "callback may not reenter trust snapshot");
  Expect(context.transfer_send_status == XNN_TRANSFER_STATUS_INVALID_STATE,
         "callback may not reenter transfer send");
  Expect(context.transfer_accept_status == XNN_TRANSFER_STATUS_INVALID_STATE,
         "callback may not reenter transfer acceptance");
  Expect(context.transfer_reject_status == XNN_TRANSFER_STATUS_INVALID_STATE,
         "callback may not reenter transfer rejection");
  Expect(context.transfer_cancel_status == XNN_TRANSFER_STATUS_INVALID_STATE,
         "callback may not reenter transfer cancellation");
  Expect(context.transfer_snapshot_status == XNN_TRANSFER_STATUS_INVALID_STATE,
         "callback may not reenter transfer snapshot");
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

void TestPairingAbiValidationAndLifecycle() {
  xnn_transfer_engine* const engine = CreateEngine();
  if (engine == nullptr) {
    return;
  }

  xnn_transfer_pairing_window_config window = PairingWindowConfig();
  Expect(xnn_transfer_pairing_open_window(engine, &window) ==
             XNN_TRANSFER_STATUS_INVALID_STATE,
         "pairing window cannot open before the engine");
  Expect(xnn_transfer_pairing_close_window(engine) == XNN_TRANSFER_STATUS_INVALID_STATE,
         "pairing window cannot close before the engine");

  window.struct_size = offsetof(xnn_transfer_pairing_window_config, duration_ms);
  Expect(xnn_transfer_pairing_open_window(engine, &window) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "pairing window rejects a short config");
  window = PairingWindowConfig();
  window.abi_version = XNN_TRANSFER_ABI_VERSION + 1;
  Expect(xnn_transfer_pairing_open_window(engine, &window) ==
             XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI,
         "pairing window rejects an unsupported ABI");
  window = PairingWindowConfig();
  window.reserved = 1;
  Expect(xnn_transfer_pairing_open_window(engine, &window) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "pairing window rejects reserved input");
  window = PairingWindowConfig();
  window.duration_ms = 120'001;
  Expect(xnn_transfer_pairing_open_window(engine, &window) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "pairing window enforces the 120 second bound");

  Expect(xnn_transfer_engine_start(engine) == XNN_TRANSFER_STATUS_OK,
         "engine starts before pairing commands");
  window = PairingWindowConfig();
  Expect(xnn_transfer_pairing_open_window(engine, &window) == XNN_TRANSFER_STATUS_OK,
         "pairing window opens through native admission");

  xnn_transfer_pairing_start_request start = PairingStartRequest(1);
  start.struct_size = offsetof(xnn_transfer_pairing_start_request, peer_id);
  Expect(xnn_transfer_pairing_start(engine, &start) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "pairing start rejects a short request");
  start = PairingStartRequest(1);
  start.abi_version = XNN_TRANSFER_ABI_VERSION + 1;
  Expect(xnn_transfer_pairing_start(engine, &start) ==
             XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI,
         "pairing start rejects an unsupported ABI");
  start = PairingStartRequest(0);
  Expect(xnn_transfer_pairing_start(engine, &start) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "pairing start rejects a zero peer observation");
  start = PairingStartRequest(1);
  Expect(xnn_transfer_pairing_start(engine, &start) == XNN_TRANSFER_STATUS_STALE_HANDLE,
         "pairing start rejects a stale discovery observation");

  xnn_transfer_pairing_attempt_ref attempt = PairingAttemptRef(1);
  attempt.struct_size = offsetof(xnn_transfer_pairing_attempt_ref, attempt_id);
  Expect(xnn_transfer_pairing_confirm(engine, &attempt) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "pairing confirmation rejects a short attempt ref");
  attempt = PairingAttemptRef(1);
  attempt.abi_version = XNN_TRANSFER_ABI_VERSION + 1;
  Expect(xnn_transfer_pairing_confirm(engine, &attempt) ==
             XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI,
         "pairing confirmation rejects an unsupported ABI");
  attempt = PairingAttemptRef(0);
  Expect(xnn_transfer_pairing_confirm(engine, &attempt) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "pairing confirmation rejects a zero attempt ID");
  attempt = PairingAttemptRef(1);
  Expect(xnn_transfer_pairing_confirm(engine, &attempt) ==
             XNN_TRANSFER_STATUS_STALE_HANDLE,
         "pairing confirmation rejects a stale attempt ID");
  Expect(
      xnn_transfer_pairing_reject(engine, &attempt) == XNN_TRANSFER_STATUS_STALE_HANDLE,
      "pairing rejection rejects the same stale attempt ID");

  xnn_transfer_trust_ref trust{
      .struct_size = sizeof(xnn_transfer_trust_ref),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .trust_id = 1,
  };
  trust.struct_size = offsetof(xnn_transfer_trust_ref, trust_id);
  Expect(xnn_transfer_pairing_revoke(engine, &trust) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "pairing revocation rejects a short trust ref");
  trust.struct_size = sizeof(xnn_transfer_trust_ref);
  trust.abi_version = XNN_TRANSFER_ABI_VERSION + 1;
  Expect(xnn_transfer_pairing_revoke(engine, &trust) ==
             XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI,
         "pairing revocation rejects an unsupported ABI");
  trust.abi_version = XNN_TRANSFER_ABI_VERSION;
  trust.trust_id = 0;
  Expect(xnn_transfer_pairing_revoke(engine, &trust) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "pairing revocation rejects a zero trust ID");
  trust.trust_id = 1;
  Expect(xnn_transfer_pairing_revoke(engine, &trust) == XNN_TRANSFER_STATUS_OK,
         "unknown trust IDs share the idempotent revoke result");

  xnn_transfer_pairing_snapshot pairing = EmptyPairingSnapshot();
  pairing.struct_size = offsetof(xnn_transfer_pairing_snapshot, attempt);
  Expect(xnn_transfer_pairing_get_snapshot(engine, &pairing) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "pairing snapshot rejects a short output");
  pairing = EmptyPairingSnapshot();
  pairing.abi_version = XNN_TRANSFER_ABI_VERSION + 1;
  Expect(xnn_transfer_pairing_get_snapshot(engine, &pairing) ==
             XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI,
         "pairing snapshot rejects an unsupported ABI");
  pairing = EmptyPairingSnapshot();
  Expect(xnn_transfer_pairing_get_snapshot(engine, &pairing) == XNN_TRANSFER_STATUS_OK,
         "pairing snapshot returns bounded empty state");
  Expect(pairing.snapshot_revision != 0 && pairing.has_attempt == 0,
         "pairing snapshot starts without a visible attempt");

  xnn_transfer_trust_snapshot_page trust_page = EmptyTrustSnapshotPage();
  trust_page.struct_size = offsetof(xnn_transfer_trust_snapshot_page, records);
  Expect(xnn_transfer_trust_get_snapshot(engine, 0, 0, &trust_page) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "trust snapshot rejects a short output");
  trust_page = EmptyTrustSnapshotPage();
  trust_page.abi_version = XNN_TRANSFER_ABI_VERSION + 1;
  Expect(xnn_transfer_trust_get_snapshot(engine, 0, 0, &trust_page) ==
             XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI,
         "trust snapshot rejects an unsupported ABI");
  trust_page = EmptyTrustSnapshotPage();
  Expect(xnn_transfer_trust_get_snapshot(engine, 0, 0, &trust_page) ==
             XNN_TRANSFER_STATUS_OK,
         "trust snapshot returns a bounded empty page");
  Expect(trust_page.snapshot_revision != 0 && trust_page.count == 0 &&
             trust_page.total_count == 0,
         "trust snapshot starts without records");

  Expect(xnn_transfer_pairing_close_window(engine) == XNN_TRANSFER_STATUS_OK,
         "pairing window closes");
  Expect(xnn_transfer_pairing_close_window(engine) == XNN_TRANSFER_STATUS_OK,
         "pairing window close is idempotent");
  Expect(xnn_transfer_engine_stop(engine) == XNN_TRANSFER_STATUS_OK,
         "engine shutdown closes pairing admission");
  window = PairingWindowConfig();
  Expect(xnn_transfer_pairing_open_window(engine, &window) ==
             XNN_TRANSFER_STATUS_INVALID_STATE,
         "pairing window cannot reopen after shutdown");
  pairing = EmptyPairingSnapshot();
  Expect(xnn_transfer_pairing_get_snapshot(engine, &pairing) == XNN_TRANSFER_STATUS_OK,
         "stopped engine retains pairing snapshot recovery");
  trust_page = EmptyTrustSnapshotPage();
  Expect(xnn_transfer_trust_get_snapshot(engine, 0, 0, &trust_page) ==
             XNN_TRANSFER_STATUS_OK,
         "stopped engine retains trust snapshot recovery");
  Expect(xnn_transfer_pairing_confirm(engine, &attempt) ==
             XNN_TRANSFER_STATUS_INVALID_STATE,
         "pairing decisions are closed after shutdown");
  xnn_transfer_engine_destroy(engine);
}

void TestTransferAbiValidationAndLifecycle() {
  xnn_transfer_engine* const engine = CreateEngine();
  if (engine == nullptr) {
    return;
  }

  xnn_transfer_transfer_send_request send = TransferSendRequest(1, "/tmp/file");
  xnn_transfer_transfer_ref output = TransferRef(0);
  Expect(xnn_transfer_transfer_send(engine, &send, &output) ==
             XNN_TRANSFER_STATUS_INVALID_STATE,
         "transfer send requires a running engine");

  send.struct_size = offsetof(xnn_transfer_transfer_send_request, path);
  Expect(xnn_transfer_transfer_send(engine, &send, &output) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "transfer send rejects a short request");
  send = TransferSendRequest(1, "/tmp/file");
  send.abi_version = XNN_TRANSFER_ABI_VERSION + 1;
  Expect(xnn_transfer_transfer_send(engine, &send, &output) ==
             XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI,
         "transfer send rejects an unsupported request ABI");
  send = TransferSendRequest(1, "/tmp/file");
  send.reserved = 1;
  Expect(xnn_transfer_transfer_send(engine, &send, &output) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "transfer send rejects reserved input");
  send = TransferSendRequest(0, "/tmp/file");
  Expect(xnn_transfer_transfer_send(engine, &send, &output) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "transfer send rejects a zero trust ID");
  send = TransferSendRequest(1, "/tmp/file");
  send.path_size = 0;
  Expect(xnn_transfer_transfer_send(engine, &send, &output) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "transfer send rejects an empty path");
  send = TransferSendRequest(1, "/tmp/file");
  send.path_size = XNN_TRANSFER_TRANSFER_PATH_MAX_SIZE + 1;
  Expect(xnn_transfer_transfer_send(engine, &send, &output) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "transfer send rejects an oversized path");

  send = TransferSendRequest(1, "/tmp/file");
  output = TransferRef(0);
  output.struct_size = offsetof(xnn_transfer_transfer_ref, transfer_id);
  Expect(xnn_transfer_transfer_send(engine, &send, &output) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "transfer send rejects a short output");
  output = TransferRef(0);
  output.abi_version = XNN_TRANSFER_ABI_VERSION + 1;
  Expect(xnn_transfer_transfer_send(engine, &send, &output) ==
             XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI,
         "transfer send rejects an unsupported output ABI");

  xnn_transfer_transfer_ref transfer_ref = TransferRef(1);
  transfer_ref.struct_size = offsetof(xnn_transfer_transfer_ref, transfer_id);
  Expect(xnn_transfer_transfer_accept(engine, &transfer_ref) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "transfer acceptance rejects a short ref");
  transfer_ref = TransferRef(1);
  transfer_ref.abi_version = XNN_TRANSFER_ABI_VERSION + 1;
  Expect(xnn_transfer_transfer_reject(engine, &transfer_ref) ==
             XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI,
         "transfer rejection rejects an unsupported ref ABI");
  transfer_ref = TransferRef(1);
  transfer_ref.reserved = 1;
  Expect(xnn_transfer_transfer_cancel(engine, &transfer_ref) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "transfer cancellation rejects reserved input");
  transfer_ref = TransferRef(0);
  Expect(xnn_transfer_transfer_accept(engine, &transfer_ref) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "transfer commands reject a zero transfer ID");

  xnn_transfer_transfer_snapshot_page page = EmptyTransferSnapshotPage();
  page.struct_size = offsetof(xnn_transfer_transfer_snapshot_page, records);
  Expect(xnn_transfer_transfer_get_snapshot(engine, 0, 0, &page) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "transfer snapshot rejects a short output");
  page = EmptyTransferSnapshotPage();
  page.abi_version = XNN_TRANSFER_ABI_VERSION + 1;
  Expect(xnn_transfer_transfer_get_snapshot(engine, 0, 0, &page) ==
             XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI,
         "transfer snapshot rejects an unsupported ABI");
  page = EmptyTransferSnapshotPage();
  Expect(xnn_transfer_transfer_get_snapshot(engine, 0, 0, &page) ==
             XNN_TRANSFER_STATUS_INVALID_STATE,
         "transfer snapshot is unavailable before engine start");

  Expect(xnn_transfer_engine_start(engine) == XNN_TRANSFER_STATUS_OK,
         "engine starts before transfer commands");
  send = TransferSendRequest(1, "/tmp/file");
  output = TransferRef(0);
  Expect(xnn_transfer_transfer_send(engine, &send, &output) ==
             XNN_TRANSFER_STATUS_STALE_HANDLE,
         "transfer send requires an active opaque trust ID");
  transfer_ref = TransferRef(1);
  Expect(xnn_transfer_transfer_accept(engine, &transfer_ref) ==
             XNN_TRANSFER_STATUS_STALE_HANDLE,
         "transfer acceptance rejects a stale ID");
  Expect(xnn_transfer_transfer_reject(engine, &transfer_ref) ==
             XNN_TRANSFER_STATUS_STALE_HANDLE,
         "transfer rejection rejects a stale ID");
  Expect(xnn_transfer_transfer_cancel(engine, &transfer_ref) ==
             XNN_TRANSFER_STATUS_STALE_HANDLE,
         "transfer cancellation rejects a stale ID");

  page = EmptyTransferSnapshotPage();
  Expect(
      xnn_transfer_transfer_get_snapshot(engine, 0, 0, &page) == XNN_TRANSFER_STATUS_OK,
      "transfer snapshot returns a bounded empty page");
  Expect(page.snapshot_revision != 0 && page.count == 0 && page.total_count == 0,
         "transfer snapshot starts without records");

  Expect(xnn_transfer_engine_stop(engine) == XNN_TRANSFER_STATUS_OK,
         "engine shutdown closes transfer commands");
  Expect(xnn_transfer_transfer_cancel(engine, &transfer_ref) ==
             XNN_TRANSFER_STATUS_INVALID_STATE,
         "transfer commands stay closed after shutdown");
  page = EmptyTransferSnapshotPage();
  Expect(
      xnn_transfer_transfer_get_snapshot(engine, 0, 0, &page) == XNN_TRANSFER_STATUS_OK,
      "stopped engine retains transfer snapshot recovery");
  xnn_transfer_engine_destroy(engine);
}

class FakePairingBackend final : public xnn_transfer::bridge::PairingBackend {
 public:
  xnn_transfer_status OpenWindow(const std::uint64_t now_ms,
                                 const std::uint64_t duration_ms) override {
    window_open = duration_ms != 0;
    opened_at_ms = now_ms;
    return window_open ? XNN_TRANSFER_STATUS_OK : XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }

  void CloseWindow() override {
    window_open = false;
    ++close_calls;
  }

  xnn_transfer_status Start(const std::uint64_t peer_id,
                            const std::uint64_t now_ms) override {
    if (!window_open || now_ms < opened_at_ms) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }
    started_peer_id = peer_id;
    return XNN_TRANSFER_STATUS_OK;
  }

  session::PairingUpdate Decide(const session::AttemptHandle& attempt,
                                const tls::ConfirmationDecision decision,
                                const std::uint64_t now_ms) override {
    static_cast<void>(now_ms);
    decided_attempt = attempt;
    decided = decision;
    if (decision == tls::ConfirmationDecision::kReject) {
      return session::PairingUpdate{
          .state = session::PairingState::kClosed,
          .error = session::PairingError::kLocalReject,
          .terminal = true,
      };
    }
    identity::DeviceId device_id{};
    device_id.fill(0x5a);
    return session::PairingUpdate{
        .state = session::PairingState::kPairedLocal,
        .paired_peer = device_id,
        .terminal = true,
    };
  }

  xnn_transfer_status Revoke(const identity::DeviceId& device_id) override {
    revoked_device = device_id;
    ++revoke_calls;
    return XNN_TRANSFER_STATUS_OK;
  }

  void Shutdown() override {
    window_open = false;
    ++shutdown_calls;
  }

  bool window_open{};
  std::uint64_t opened_at_ms{};
  std::uint64_t started_peer_id{};
  session::AttemptHandle decided_attempt{};
  tls::ConfirmationDecision decided{tls::ConfirmationDecision::kReject};
  identity::DeviceId revoked_device{};
  int close_calls{};
  int revoke_calls{};
  int shutdown_calls{};
};

session::AttemptHandle AttemptHandle(const std::uint8_t value) {
  session::AttemptHandle attempt{};
  attempt.fill(value);
  return attempt;
}

std::vector<xnn_transfer_event> DrainEvents(
    xnn_transfer::bridge::EventChannel& channel) {
  std::vector<xnn_transfer_event> events;
  for (;;) {
    xnn_transfer_event event = EmptyEvent();
    const xnn_transfer_status status = channel.Poll(&event);
    if (status == XNN_TRANSFER_STATUS_EVENT_QUEUE_EMPTY) {
      return events;
    }
    Expect(status == XNN_TRANSFER_STATUS_OK, "pairing event queue remains pollable");
    if (status != XNN_TRANSFER_STATUS_OK) {
      return events;
    }
    events.push_back(event);
  }
}

void TestPairingBridgeDecisionsTrustAndShutdown() {
  using xnn_transfer::bridge::EventChannel;
  using xnn_transfer::bridge::PairingBridge;

  FakePairingBackend backend;
  PairingBridge pairing(backend);
  EventChannel events;
  Expect(pairing.OpenWindow(1'000, 60'000) == XNN_TRANSFER_STATUS_OK,
         "pairing bridge opens the native window");
  Expect(pairing.Start(42, 1'001) == XNN_TRANSFER_STATUS_OK &&
             backend.started_peer_id == 42,
         "pairing bridge starts only through the native backend");

  const session::AttemptHandle attempt = AttemptHandle(0x11);
  Expect(pairing.Apply(42, attempt,
                       session::PairingUpdate{
                           .state = session::PairingState::kExchangingHellos,
                       },
                       events),
         "pairing bridge accepts a native starting update");
  const std::array<std::uint16_t, XNN_TRANSFER_PAIRING_SAS_WORD_COUNT> sas_words{
      1, 17, 255, 1'024, 2'047};
  Expect(pairing.Apply(42, attempt,
                       session::PairingUpdate{
                           .state = session::PairingState::kAwaitingDecisions,
                           .prompt =
                               session::PairingPrompt{
                                   .handle = attempt,
                                   .sas_word_indices = sas_words,
                                   .deadline_ms = 91'000,
                               },
                       },
                       events),
         "pairing bridge accepts a bounded native SAS prompt");

  xnn_transfer_pairing_snapshot prompt = EmptyPairingSnapshot();
  Expect(
      pairing.Snapshot(&prompt) == XNN_TRANSFER_STATUS_OK && prompt.has_attempt == 1 &&
          prompt.attempt.state == XNN_TRANSFER_PAIRING_ATTEMPT_AWAITING_CONFIRMATION &&
          prompt.attempt.peer_id == 42 && prompt.attempt.deadline_ms == 91'000 &&
          prompt.attempt.sas_word_count == XNN_TRANSFER_PAIRING_SAS_WORD_COUNT &&
          std::equal(std::begin(prompt.attempt.sas_word_indices),
                     std::end(prompt.attempt.sas_word_indices), sas_words.begin()),
      "pairing snapshot copies only the visible SAS ceremony");

  const session::AttemptHandle stale = AttemptHandle(0x22);
  Expect(pairing.Decide(
             std::span<const std::uint8_t, XNN_TRANSFER_PAIRING_ATTEMPT_ID_SIZE>(stale),
             tls::ConfirmationDecision::kConfirm, 1'002,
             events) == XNN_TRANSFER_STATUS_STALE_HANDLE,
         "pairing bridge rejects a stale opaque attempt");
  Expect(
      pairing.Decide(
          std::span<const std::uint8_t, XNN_TRANSFER_PAIRING_ATTEMPT_ID_SIZE>(attempt),
          tls::ConfirmationDecision::kConfirm, 1'003, events) == XNN_TRANSFER_STATUS_OK,
      "pairing bridge confirms the current native attempt");
  Expect(backend.decided_attempt == attempt &&
             backend.decided == tls::ConfirmationDecision::kConfirm,
         "confirmation forwards only the opaque attempt and decision");
  Expect(
      pairing.Decide(
          std::span<const std::uint8_t, XNN_TRANSFER_PAIRING_ATTEMPT_ID_SIZE>(attempt),
          tls::ConfirmationDecision::kConfirm, 1'004,
          events) == XNN_TRANSFER_STATUS_STALE_HANDLE,
      "terminal pairing invalidates the attempt immediately");

  const std::vector<xnn_transfer_event> paired_events = DrainEvents(events);
  Expect(paired_events.size() == 4,
         "start, prompt, paired, and trust events are published");
  if (paired_events.size() == 4) {
    Expect(
        paired_events[0].type == XNN_TRANSFER_EVENT_TYPE_PAIRING_ATTEMPT_CHANGED &&
            paired_events[1].type == XNN_TRANSFER_EVENT_TYPE_PAIRING_ATTEMPT_CHANGED &&
            paired_events[2].type == XNN_TRANSFER_EVENT_TYPE_PAIRING_ATTEMPT_CHANGED &&
            paired_events[3].type == XNN_TRANSFER_EVENT_TYPE_TRUST_CHANGED,
        "pairing success publishes trust after the terminal attempt");
    xnn_transfer_pairing_attempt_event_payload closed{};
    std::memcpy(&closed, paired_events[2].payload, sizeof(closed));
    Expect(closed.state == XNN_TRANSFER_PAIRING_ATTEMPT_PAIRED &&
               closed.sas_word_count == 0 &&
               closed.error == XNN_TRANSFER_PAIRING_ERROR_NONE,
           "terminal success erases SAS data before publication");
  }

  xnn_transfer_pairing_snapshot completed = EmptyPairingSnapshot();
  Expect(pairing.Snapshot(&completed) == XNN_TRANSFER_STATUS_OK &&
             completed.has_attempt == 0,
         "terminal pairing disappears from prompt recovery");
  xnn_transfer_trust_snapshot_page trust = EmptyTrustSnapshotPage();
  Expect(pairing.TrustSnapshot(0, 0, &trust) == XNN_TRANSFER_STATUS_OK &&
             trust.count == 1 && trust.total_count == 1 &&
             trust.records[0].trust_id == 1 && trust.records[0].peer_id == 42 &&
             trust.records[0].state == XNN_TRANSFER_TRUST_STATE_ACTIVE,
         "trust snapshot exposes only an opaque active record");

  Expect(pairing.Revoke(1, events) == XNN_TRANSFER_STATUS_OK &&
             pairing.Revoke(1, events) == XNN_TRANSFER_STATUS_OK &&
             pairing.Revoke(999, events) == XNN_TRANSFER_STATUS_OK &&
             backend.revoke_calls == 1,
         "known, duplicate, and unknown revocation share idempotent results");
  const std::vector<xnn_transfer_event> revoke_events = DrainEvents(events);
  Expect(revoke_events.size() == 1 &&
             revoke_events[0].type == XNN_TRANSFER_EVENT_TYPE_TRUST_CHANGED,
         "only the durable trust transition publishes a revoke event");
  trust = EmptyTrustSnapshotPage();
  Expect(pairing.TrustSnapshot(0, 0, &trust) == XNN_TRANSFER_STATUS_OK &&
             trust.records[0].state == XNN_TRANSFER_TRUST_STATE_REVOKED,
         "trust snapshot retains the revoked tombstone state");

  const session::AttemptHandle rejected = AttemptHandle(0x33);
  Expect(pairing.Apply(43, rejected,
                       session::PairingUpdate{
                           .state = session::PairingState::kAwaitingDecisions,
                           .prompt =
                               session::PairingPrompt{
                                   .handle = rejected,
                                   .sas_word_indices = sas_words,
                                   .deadline_ms = 92'000,
                               },
                       },
                       events),
         "second attempt reaches a visible prompt");
  Expect(
      pairing.Decide(
          std::span<const std::uint8_t, XNN_TRANSFER_PAIRING_ATTEMPT_ID_SIZE>(rejected),
          tls::ConfirmationDecision::kReject, 1'005, events) == XNN_TRANSFER_STATUS_OK,
      "pairing bridge rejects the current native attempt");
  const std::vector<xnn_transfer_event> rejected_events = DrainEvents(events);
  Expect(rejected_events.size() == 2,
         "local rejection publishes prompt and terminal events");
  if (rejected_events.size() == 2) {
    xnn_transfer_pairing_attempt_event_payload rejected_payload{};
    std::memcpy(&rejected_payload, rejected_events[1].payload,
                sizeof(rejected_payload));
    Expect(rejected_payload.state == XNN_TRANSFER_PAIRING_ATTEMPT_CLOSED &&
               rejected_payload.error == XNN_TRANSFER_PAIRING_ERROR_REJECTED &&
               rejected_payload.sas_word_count == 0,
           "local rejection is collapsed and erases SAS data");
  }

  const session::AttemptHandle shutdown = AttemptHandle(0x44);
  Expect(pairing.Apply(44, shutdown,
                       session::PairingUpdate{
                           .state = session::PairingState::kAwaitingDecisions,
                           .prompt =
                               session::PairingPrompt{
                                   .handle = shutdown,
                                   .sas_word_indices = sas_words,
                                   .deadline_ms = 93'000,
                               },
                       },
                       events),
         "shutdown fixture reaches a visible prompt");
  pairing.Shutdown(events);
  Expect(backend.shutdown_calls == 1, "pairing shutdown reaches the native backend");
  const std::vector<xnn_transfer_event> shutdown_events = DrainEvents(events);
  Expect(shutdown_events.size() == 2,
         "shutdown publishes prompt and terminal cancellation");
  if (shutdown_events.size() == 2) {
    xnn_transfer_pairing_attempt_event_payload cancelled{};
    std::memcpy(&cancelled, shutdown_events[1].payload, sizeof(cancelled));
    Expect(cancelled.state == XNN_TRANSFER_PAIRING_ATTEMPT_CLOSED &&
               cancelled.error == XNN_TRANSFER_PAIRING_ERROR_CANCELLED &&
               cancelled.sas_word_count == 0,
           "shutdown invalidates and clears the visible attempt");
  }
  identity::DeviceId late_device{};
  late_device.fill(0x77);
  Expect(!pairing.Apply(45, AttemptHandle(0x55),
                        session::PairingUpdate{
                            .state = session::PairingState::kPairedLocal,
                            .paired_peer = late_device,
                            .terminal = true,
                        },
                        events) &&
             DrainEvents(events).empty(),
         "pairing shutdown rejects late native updates");
}

void TestPairingTrustPaginationAndOverflow() {
  using xnn_transfer::bridge::EventChannel;
  using xnn_transfer::bridge::PairingBridge;

  FakePairingBackend backend;
  PairingBridge pairing(backend);
  EventChannel events;
  for (std::uint8_t value = 1; value <= 10; ++value) {
    identity::DeviceId device_id{};
    device_id.fill(value);
    Expect(pairing.Apply(value, AttemptHandle(value),
                         session::PairingUpdate{
                             .state = session::PairingState::kPairedLocal,
                             .paired_peer = device_id,
                             .terminal = true,
                         },
                         events),
           "trust pagination fixture accepts a terminal native update");
  }

  xnn_transfer_trust_snapshot_page first = EmptyTrustSnapshotPage();
  Expect(pairing.TrustSnapshot(0, 0, &first) == XNN_TRANSFER_STATUS_OK &&
             first.count == XNN_TRANSFER_TRUST_SNAPSHOT_PAGE_CAPACITY &&
             first.total_count == 10,
         "trust snapshot first page is fixed and bounded");
  const std::uint64_t first_revision = first.snapshot_revision;

  identity::DeviceId added_device{};
  added_device.fill(11);
  Expect(pairing.Apply(11, AttemptHandle(11),
                       session::PairingUpdate{
                           .state = session::PairingState::kPairedLocal,
                           .paired_peer = added_device,
                           .terminal = true,
                       },
                       events),
         "trust snapshot fixture mutates after its first page");
  xnn_transfer_trust_snapshot_page stale_page = EmptyTrustSnapshotPage();
  Expect(
      pairing.TrustSnapshot(first_revision, XNN_TRANSFER_TRUST_SNAPSHOT_PAGE_CAPACITY,
                            &stale_page) == XNN_TRANSFER_STATUS_STALE_SNAPSHOT,
      "trust pagination rejects a revision changed between pages");

  xnn_transfer_trust_snapshot_page refreshed = EmptyTrustSnapshotPage();
  Expect(pairing.TrustSnapshot(0, 0, &refreshed) == XNN_TRANSFER_STATUS_OK,
         "trust pagination restarts at the current revision");
  xnn_transfer_trust_snapshot_page tail = EmptyTrustSnapshotPage();
  Expect(pairing.TrustSnapshot(refreshed.snapshot_revision,
                               XNN_TRANSFER_TRUST_SNAPSHOT_PAGE_CAPACITY,
                               &tail) == XNN_TRANSFER_STATUS_OK &&
             tail.count == 3,
         "trust pagination returns the bounded final page");

  EventChannel overflow;
  xnn_transfer_pairing_attempt_event_payload payload{
      .struct_size = sizeof(xnn_transfer_pairing_attempt_event_payload),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .state = XNN_TRANSFER_PAIRING_ATTEMPT_STARTING,
      .peer_id = 1,
  };
  payload.attempt_id[0] = 1;
  for (std::size_t index = 0; index < XNN_TRANSFER_EVENT_QUEUE_CAPACITY + 1; ++index) {
    overflow.EnqueuePairing(payload);
  }
  const std::vector<xnn_transfer_event> retained = DrainEvents(overflow);
  const bool observed_drop = std::any_of(
      retained.begin(), retained.end(), [](const xnn_transfer_event& event) {
        return (event.flags & XNN_TRANSFER_EVENT_FLAG_EVENTS_DROPPED_BEFORE) != 0;
      });
  Expect(retained.size() == XNN_TRANSFER_EVENT_QUEUE_CAPACITY &&
             retained.front().sequence == 2 && observed_drop,
         "pairing overflow drops oldest and requires snapshot recovery");
}

class FakeTransferBackend final : public xnn_transfer::bridge::TransferBackend {
 public:
  xnn_transfer::bridge::TransferStartResult Send(
      const identity::DeviceId& device_id, const std::span<const std::uint8_t> path,
      const std::uint64_t now_ms) override {
    sent_device = device_id;
    sent_path.assign(path.begin(), path.end());
    sent_at_ms = now_ms;
    ++send_calls;
    return send_result;
  }

  xnn_transfer_status Accept(const transfer::TransferId& transfer_id,
                             const std::uint64_t now_ms) override {
    accepted_id = transfer_id;
    accepted_at_ms = now_ms;
    ++accept_calls;
    return accept_status;
  }

  xnn_transfer_status Reject(const transfer::TransferId& transfer_id,
                             const std::uint64_t now_ms) override {
    rejected_id = transfer_id;
    rejected_at_ms = now_ms;
    ++reject_calls;
    return reject_status;
  }

  xnn_transfer_status Cancel(const transfer::TransferId& transfer_id,
                             const std::uint64_t now_ms) override {
    cancelled_id = transfer_id;
    cancelled_at_ms = now_ms;
    ++cancel_calls;
    return cancel_status;
  }

  void Shutdown() override { ++shutdown_calls; }

  xnn_transfer::bridge::TransferStartResult send_result{};
  xnn_transfer_status accept_status{XNN_TRANSFER_STATUS_OK};
  xnn_transfer_status reject_status{XNN_TRANSFER_STATUS_OK};
  xnn_transfer_status cancel_status{XNN_TRANSFER_STATUS_OK};
  identity::DeviceId sent_device{};
  std::vector<std::uint8_t> sent_path;
  transfer::TransferId accepted_id{};
  transfer::TransferId rejected_id{};
  transfer::TransferId cancelled_id{};
  std::uint64_t sent_at_ms{};
  std::uint64_t accepted_at_ms{};
  std::uint64_t rejected_at_ms{};
  std::uint64_t cancelled_at_ms{};
  int send_calls{};
  int accept_calls{};
  int reject_calls{};
  int cancel_calls{};
  int shutdown_calls{};
};

std::optional<xnn_transfer_transfer_event_payload> ReadTransferEvent(
    const xnn_transfer_event& event) {
  if (event.type != XNN_TRANSFER_EVENT_TYPE_TRANSFER_CHANGED ||
      event.payload_version != XNN_TRANSFER_TRANSFER_EVENT_PAYLOAD_VERSION ||
      event.payload_size != sizeof(xnn_transfer_transfer_event_payload)) {
    return std::nullopt;
  }
  xnn_transfer_transfer_event_payload payload{};
  std::memcpy(&payload, event.payload, sizeof(payload));
  if (payload.struct_size != sizeof(payload) ||
      payload.abi_version != XNN_TRANSFER_ABI_VERSION) {
    return std::nullopt;
  }
  return payload;
}

struct TransferCallbackContext {
  xnn_transfer::bridge::EventChannel* channel{};
  std::vector<xnn_transfer_transfer_event_payload> payloads;
  int calls{};
};

void DrainTransferOnWakeup(void* const user_data) {
  auto* const context = static_cast<TransferCallbackContext*>(user_data);
  ++context->calls;
  for (;;) {
    xnn_transfer_event event = EmptyEvent();
    const xnn_transfer_status status = context->channel->Poll(&event);
    if (status == XNN_TRANSFER_STATUS_EVENT_QUEUE_EMPTY) {
      return;
    }
    if (status != XNN_TRANSFER_STATUS_OK) {
      ++failures;
      return;
    }
    const auto payload = ReadTransferEvent(event);
    if (!payload.has_value()) {
      ++failures;
      return;
    }
    context->payloads.push_back(*payload);
  }
}

void TestTransferCallbackDrainsCopiedEvent() {
  using xnn_transfer::bridge::EventChannel;
  using xnn_transfer::bridge::TransferBridge;

  FakeTransferBackend backend;
  TransferBridge bridge(backend);
  EventChannel events;
  TransferCallbackContext context{.channel = &events};
  xnn_transfer_event_callback_config callback{
      .struct_size = sizeof(xnn_transfer_event_callback_config),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .callback = DrainTransferOnWakeup,
      .user_data = &context,
  };
  Expect(events.SetCallback(&callback) == XNN_TRANSFER_STATUS_OK,
         "transfer callback registration succeeds");
  const transfer::TransferId offered = TransferId(0x10);
  Expect(bridge.ApplyIncomingOffer("peer-callback",
                                   transfer::IncomingOffer{
                                       .transfer_id = offered,
                                       .file_size = 10,
                                   },
                                   events),
         "incoming offer reaches the packaged callback path");
  Expect(context.calls == 1 && context.payloads.size() == 1 &&
             context.payloads[0].state == XNN_TRANSFER_TRANSFER_STATE_OFFERED &&
             context.payloads[0].total_bytes == 10 &&
             std::equal(std::begin(context.payloads[0].transfer_id),
                        std::end(context.payloads[0].transfer_id), offered.begin()),
         "callback drains a copied bounded transfer payload");
  Expect(events.SetCallback(nullptr) == XNN_TRANSFER_STATUS_OK,
         "transfer callback clears after draining");
}

void TestTransferBridgeCommandsProgressAndRecovery() {
  using xnn_transfer::bridge::EventChannel;
  using xnn_transfer::bridge::TransferBridge;
  using xnn_transfer::bridge::TransferStartResult;

  FakeTransferBackend backend;
  TransferBridge bridge(backend);
  EventChannel events;
  const transfer::TransferId outgoing = TransferId(0x11);
  backend.send_result = TransferStartResult{
      .status = XNN_TRANSFER_STATUS_OK,
      .transfer_id = outgoing,
      .total_bytes = 1'000,
      .peer_label = "peer-a",
  };
  identity::DeviceId device_id{};
  device_id.fill(0x5a);
  const std::vector<std::uint8_t> path{'/', 't', 'm', 'p', '/', 'f', 'i', 'l', 'e'};
  xnn_transfer_transfer_ref output{
      .struct_size = sizeof(xnn_transfer_transfer_ref),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
  };
  Expect(bridge.Send(device_id, path, 100, &output, events) == XNN_TRANSFER_STATUS_OK,
         "transfer bridge starts an outgoing file through the backend");
  Expect(backend.send_calls == 1 && backend.sent_device == device_id &&
             backend.sent_path == path && backend.sent_at_ms == 100 &&
             std::equal(std::begin(output.transfer_id), std::end(output.transfer_id),
                        outgoing.begin()),
         "transfer send copies the path and returns the native ID");

  std::vector<xnn_transfer_event> queued_events = DrainEvents(events);
  Expect(queued_events.size() == 1,
         "outgoing send publishes one queued transfer event");
  if (queued_events.size() == 1) {
    const auto payload = ReadTransferEvent(queued_events.front());
    Expect(payload.has_value() &&
               payload->direction == XNN_TRANSFER_TRANSFER_DIRECTION_OUTGOING &&
               payload->state == XNN_TRANSFER_TRANSFER_STATE_QUEUED &&
               payload->total_bytes == 1'000 && payload->transferred_bytes == 0 &&
               payload->peer_label_size == 6,
           "outgoing event contains bounded public metadata");
  }

  Expect(bridge.Apply(outgoing,
                      transfer::TransferUpdate{
                          .state = transfer::TransferState::kSendingFile,
                      },
                      400, events),
         "transfer bridge accepts monotonic native progress");
  Expect(!bridge.Apply(outgoing,
                       transfer::TransferUpdate{
                           .state = transfer::TransferState::kSendingFile,
                       },
                       399, events),
         "transfer bridge rejects regressing progress");
  Expect(!bridge.Apply(outgoing,
                       transfer::TransferUpdate{
                           .state = transfer::TransferState::kCompleted,
                           .terminal = true,
                       },
                       999, events),
         "transfer bridge rejects incomplete completion");
  Expect(bridge.Apply(outgoing,
                      transfer::TransferUpdate{
                          .state = transfer::TransferState::kCompleted,
                          .terminal = true,
                      },
                      1'000, events),
         "transfer bridge accepts exact terminal completion");
  Expect(bridge.Cancel(outgoing, 101, events) == XNN_TRANSFER_STATUS_STALE_HANDLE,
         "terminal outgoing transfer IDs are stale");
  Expect(!bridge.Apply(outgoing,
                       transfer::TransferUpdate{
                           .state = transfer::TransferState::kFailed,
                           .error = transfer::TransferError::kInternalFailure,
                           .terminal = true,
                       },
                       1'000, events),
         "terminal transfer state is immutable");

  const std::vector<xnn_transfer_event> progress_events = DrainEvents(events);
  Expect(progress_events.size() == 2,
         "only valid progress and completion updates are published");
  if (progress_events.size() == 2) {
    const auto progress = ReadTransferEvent(progress_events[0]);
    const auto completed = ReadTransferEvent(progress_events[1]);
    Expect(progress.has_value() && completed.has_value() &&
               progress->state == XNN_TRANSFER_TRANSFER_STATE_RUNNING &&
               progress->transferred_bytes == 400 &&
               completed->state == XNN_TRANSFER_TRANSFER_STATE_COMPLETED &&
               completed->transferred_bytes == 1'000 &&
               progress->snapshot_revision < completed->snapshot_revision,
           "progress events preserve state, bytes, and revision order");
  }

  backend.send_result.transfer_id = outgoing;
  xnn_transfer_transfer_ref duplicate_output{
      .struct_size = sizeof(xnn_transfer_transfer_ref),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
  };
  Expect(bridge.Send(device_id, path, 102, &duplicate_output, events) ==
                 XNN_TRANSFER_STATUS_INTERNAL_ERROR &&
             backend.cancel_calls == 1,
         "a reused backend transfer ID is cancelled and rejected");
  const std::vector<std::uint8_t> nul_path{'a', 0, 'b'};
  Expect(bridge.Send(device_id, nul_path, 103, &duplicate_output, events) ==
                 XNN_TRANSFER_STATUS_INVALID_ARGUMENT &&
             backend.send_calls == 2,
         "transfer bridge rejects a path containing NUL before backend use");

  const transfer::TransferId incoming = TransferId(0x22);
  const transfer::IncomingOffer incoming_offer{
      .transfer_id = incoming,
      .relative_path = "remote-private-name",
      .file_size = 200,
      .display_name = "remote-private-name",
  };
  Expect(bridge.ApplyIncomingOffer("peer-b", incoming_offer, events),
         "authenticated incoming offer becomes visible");
  Expect(bridge.ApplyIncomingOffer("peer-b", incoming_offer, events) &&
             DrainEvents(events).size() == 1,
         "duplicate identical offer is idempotent");
  Expect(bridge.Accept(incoming, 200, events) == XNN_TRANSFER_STATUS_OK &&
             backend.accept_calls == 1 && backend.accepted_id == incoming,
         "incoming acceptance routes only the opaque ID");
  Expect(bridge.Accept(incoming, 201, events) == XNN_TRANSFER_STATUS_STALE_HANDLE,
         "accepted offer ID cannot be accepted twice");
  Expect(bridge.Apply(incoming,
                      transfer::TransferUpdate{
                          .state = transfer::TransferState::kReceivingFile,
                      },
                      50, events),
         "accepted incoming transfer publishes receive progress");
  Expect(bridge.Cancel(incoming, 202, events) == XNN_TRANSFER_STATUS_OK &&
             bridge.Cancel(incoming, 203, events) == XNN_TRANSFER_STATUS_OK &&
             backend.cancel_calls == 2,
         "duplicate cancellation is idempotent at the bridge");
  Expect(bridge.Apply(incoming,
                      transfer::TransferUpdate{
                          .state = transfer::TransferState::kCancelled,
                          .error = transfer::TransferError::kCancelled,
                          .terminal = true,
                      },
                      50, events),
         "native cancellation acknowledgement becomes terminal");
  Expect(bridge.Cancel(incoming, 204, events) == XNN_TRANSFER_STATUS_STALE_HANDLE,
         "cancelled transfer ID becomes stale");

  const std::vector<xnn_transfer_event> incoming_events = DrainEvents(events);
  Expect(incoming_events.size() == 4,
         "accepted transfer publishes queued, running, cancelling, and cancelled");
  if (incoming_events.size() == 4) {
    const auto cancelled = ReadTransferEvent(incoming_events.back());
    Expect(cancelled.has_value() &&
               cancelled->state == XNN_TRANSFER_TRANSFER_STATE_CANCELLED &&
               cancelled->error == XNN_TRANSFER_TRANSFER_ERROR_CANCELLED &&
               cancelled->transferred_bytes == 50,
           "cancelled event exposes stable public cancellation");
  }

  const transfer::TransferId rejected = TransferId(0x33);
  const transfer::IncomingOffer rejected_offer{
      .transfer_id = rejected,
      .relative_path = "ignored",
      .file_size = 300,
      .display_name = "ignored",
  };
  Expect(bridge.ApplyIncomingOffer("peer-c", rejected_offer, events) &&
             bridge.Reject(rejected, 300, events) == XNN_TRANSFER_STATUS_OK &&
             backend.reject_calls == 1 && backend.rejected_id == rejected,
         "incoming rejection routes the ID and removes the offer");
  Expect(bridge.Reject(rejected, 301, events) == XNN_TRANSFER_STATUS_STALE_HANDLE,
         "rejected offer ID becomes stale");
  const std::vector<xnn_transfer_event> rejected_events = DrainEvents(events);
  Expect(rejected_events.size() == 2,
         "incoming rejection publishes offer then removal");
  if (rejected_events.size() == 2) {
    const auto removed = ReadTransferEvent(rejected_events.back());
    Expect(removed.has_value() && removed->change == XNN_TRANSFER_TRANSFER_REMOVED &&
               removed->direction == XNN_TRANSFER_TRANSFER_DIRECTION_INCOMING &&
               removed->state == XNN_TRANSFER_TRANSFER_STATE_OFFERED,
           "offer removal carries no fabricated transfer state");
  }

  xnn_transfer_transfer_snapshot_page snapshot = EmptyTransferSnapshotPage();
  Expect(bridge.Snapshot(0, 0, &snapshot) == XNN_TRANSFER_STATUS_OK &&
             snapshot.count == 2 && snapshot.total_count == 2,
         "transfer snapshot retains completed and cancelled records");

  for (std::uint8_t value = 0x40; value < 0x45; ++value) {
    Expect(bridge.ApplyIncomingOffer("peer-page",
                                     transfer::IncomingOffer{
                                         .transfer_id = TransferId(value),
                                         .file_size = static_cast<std::uint64_t>(value),
                                     },
                                     events),
           "transfer pagination fixture accepts a bounded offer");
  }
  snapshot = EmptyTransferSnapshotPage();
  Expect(bridge.Snapshot(0, 0, &snapshot) == XNN_TRANSFER_STATUS_OK &&
             snapshot.count == XNN_TRANSFER_TRANSFER_SNAPSHOT_PAGE_CAPACITY &&
             snapshot.total_count == 7,
         "transfer snapshot first page is fixed and bounded");
  const std::uint64_t stale_revision = snapshot.snapshot_revision;
  Expect(bridge.ApplyIncomingOffer("peer-page",
                                   transfer::IncomingOffer{
                                       .transfer_id = TransferId(0x45),
                                       .file_size = 0x45,
                                   },
                                   events),
         "transfer pagination fixture mutates after the first page");
  xnn_transfer_transfer_snapshot_page stale_page = EmptyTransferSnapshotPage();
  Expect(bridge.Snapshot(stale_revision, XNN_TRANSFER_TRANSFER_SNAPSHOT_PAGE_CAPACITY,
                         &stale_page) == XNN_TRANSFER_STATUS_STALE_SNAPSHOT,
         "transfer pagination rejects a stale continuation");

  EventChannel overflow;
  xnn_transfer_transfer_event_payload overflow_payload = snapshot.records[0];
  for (std::size_t index = 0; index < XNN_TRANSFER_EVENT_QUEUE_CAPACITY + 1; ++index) {
    overflow.EnqueueTransfer(overflow_payload);
  }
  const std::vector<xnn_transfer_event> retained = DrainEvents(overflow);
  const bool observed_drop = std::any_of(
      retained.begin(), retained.end(), [](const xnn_transfer_event& event) {
        return (event.flags & XNN_TRANSFER_EVENT_FLAG_EVENTS_DROPPED_BEFORE) != 0;
      });
  Expect(retained.size() == XNN_TRANSFER_EVENT_QUEUE_CAPACITY &&
             retained.front().sequence == 2 && observed_drop,
         "transfer overflow is explicit and snapshot recoverable");
}

void TestTransferBridgeShutdownBarrier() {
  using xnn_transfer::bridge::EventChannel;
  using xnn_transfer::bridge::TransferBridge;
  using xnn_transfer::bridge::TransferStartResult;

  FakeTransferBackend backend;
  TransferBridge bridge(backend);
  EventChannel events;
  const transfer::TransferId active = TransferId(0x60);
  backend.send_result = TransferStartResult{
      .status = XNN_TRANSFER_STATUS_OK,
      .transfer_id = active,
      .total_bytes = 600,
      .peer_label = "peer-active",
  };
  identity::DeviceId device_id{};
  device_id.fill(0x60);
  const std::vector<std::uint8_t> path{'f'};
  xnn_transfer_transfer_ref output{
      .struct_size = sizeof(xnn_transfer_transfer_ref),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
  };
  Expect(bridge.Send(device_id, path, 400, &output, events) == XNN_TRANSFER_STATUS_OK,
         "shutdown fixture starts an active transfer");
  const transfer::TransferId offered = TransferId(0x61);
  Expect(bridge.ApplyIncomingOffer("peer-offer",
                                   transfer::IncomingOffer{
                                       .transfer_id = offered,
                                       .file_size = 601,
                                   },
                                   events),
         "shutdown fixture adds an incoming offer");
  static_cast<void>(DrainEvents(events));

  bridge.Shutdown(events);
  Expect(backend.shutdown_calls == 1,
         "transfer shutdown reaches the native backend once");
  const std::vector<xnn_transfer_event> shutdown_events = DrainEvents(events);
  Expect(shutdown_events.size() == 2,
         "shutdown cancels active work and withdraws pending offers");
  bool saw_cancelled = false;
  bool saw_removed = false;
  for (const xnn_transfer_event& event : shutdown_events) {
    const auto payload = ReadTransferEvent(event);
    if (!payload.has_value()) {
      continue;
    }
    saw_cancelled =
        saw_cancelled || (payload->state == XNN_TRANSFER_TRANSFER_STATE_CANCELLED &&
                          payload->error == XNN_TRANSFER_TRANSFER_ERROR_CANCELLED);
    saw_removed = saw_removed || payload->change == XNN_TRANSFER_TRANSFER_REMOVED;
  }
  Expect(saw_cancelled && saw_removed,
         "shutdown events expose cancellation and offer removal");

  xnn_transfer_transfer_snapshot_page snapshot = EmptyTransferSnapshotPage();
  Expect(bridge.Snapshot(0, 0, &snapshot) == XNN_TRANSFER_STATUS_OK &&
             snapshot.count == 1 &&
             snapshot.records[0].state == XNN_TRANSFER_TRANSFER_STATE_CANCELLED,
         "shutdown snapshot retains only terminal transfer state");
  Expect(!bridge.Apply(active,
                       transfer::TransferUpdate{
                           .state = transfer::TransferState::kCompleted,
                           .terminal = true,
                       },
                       600, events) &&
             !bridge.ApplyIncomingOffer("late",
                                        transfer::IncomingOffer{
                                            .transfer_id = TransferId(0x62),
                                            .file_size = 1,
                                        },
                                        events) &&
             bridge.Send(device_id, path, 401, &output, events) ==
                 XNN_TRANSFER_STATUS_INVALID_STATE &&
             DrainEvents(events).empty(),
         "shutdown rejects late offers, progress, and sends");
  bridge.Shutdown(events);
  Expect(backend.shutdown_calls == 1, "transfer shutdown is idempotent");
}

class BlockingTransferBackend final : public xnn_transfer::bridge::TransferBackend {
 public:
  xnn_transfer::bridge::TransferStartResult Send(
      const identity::DeviceId& device_id, const std::span<const std::uint8_t> path,
      const std::uint64_t now_ms) override {
    static_cast<void>(device_id);
    static_cast<void>(path);
    static_cast<void>(now_ms);
    std::unique_lock lock(mutex);
    send_entered = true;
    condition.notify_all();
    condition.wait(lock, [this] { return release_send; });
    send_finished = true;
    return xnn_transfer::bridge::TransferStartResult{
        .status = XNN_TRANSFER_STATUS_OK,
        .transfer_id = TransferId(0x70),
        .total_bytes = 700,
        .peer_label = "peer-race",
    };
  }

  xnn_transfer_status Accept(const transfer::TransferId& transfer_id,
                             const std::uint64_t now_ms) override {
    static_cast<void>(transfer_id);
    static_cast<void>(now_ms);
    return XNN_TRANSFER_STATUS_OK;
  }

  xnn_transfer_status Reject(const transfer::TransferId& transfer_id,
                             const std::uint64_t now_ms) override {
    static_cast<void>(transfer_id);
    static_cast<void>(now_ms);
    return XNN_TRANSFER_STATUS_OK;
  }

  xnn_transfer_status Cancel(const transfer::TransferId& transfer_id,
                             const std::uint64_t now_ms) override {
    static_cast<void>(transfer_id);
    static_cast<void>(now_ms);
    return XNN_TRANSFER_STATUS_OK;
  }

  void Shutdown() override {
    const std::scoped_lock lock(mutex);
    shutdown_after_send = send_finished;
    ++shutdown_calls;
  }

  std::mutex mutex;
  std::condition_variable condition;
  bool send_entered{};
  bool release_send{};
  bool send_finished{};
  bool shutdown_after_send{};
  int shutdown_calls{};
};

void TestTransferShutdownWaitsForBackendCommand() {
  using xnn_transfer::bridge::EventChannel;
  using xnn_transfer::bridge::TransferBridge;

  BlockingTransferBackend backend;
  TransferBridge bridge(backend);
  EventChannel events;
  identity::DeviceId device_id{};
  device_id.fill(0x70);
  const std::vector<std::uint8_t> path{'f'};
  xnn_transfer_transfer_ref output{
      .struct_size = sizeof(xnn_transfer_transfer_ref),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
  };
  std::atomic<xnn_transfer_status> send_status{XNN_TRANSFER_STATUS_INTERNAL_ERROR};
  std::thread send_thread(
      [&] { send_status.store(bridge.Send(device_id, path, 500, &output, events)); });

  {
    std::unique_lock lock(backend.mutex);
    backend.condition.wait(lock, [&backend] { return backend.send_entered; });
  }
  std::atomic<bool> shutdown_returned{false};
  std::thread shutdown_thread([&] {
    bridge.Shutdown(events);
    shutdown_returned.store(true);
  });
  std::this_thread::sleep_for(20ms);
  Expect(!shutdown_returned.load(),
         "transfer shutdown waits for an in-flight backend command");

  {
    const std::scoped_lock lock(backend.mutex);
    backend.release_send = true;
  }
  backend.condition.notify_all();
  send_thread.join();
  shutdown_thread.join();

  Expect(send_status.load() == XNN_TRANSFER_STATUS_OK && shutdown_returned.load() &&
             backend.shutdown_calls == 1 && backend.shutdown_after_send,
         "backend command completes before the shutdown barrier");
  const std::vector<xnn_transfer_event> ordered = DrainEvents(events);
  Expect(ordered.size() == 2, "racing send and shutdown publish queued then cancelled");
  if (ordered.size() == 2) {
    const auto queued = ReadTransferEvent(ordered[0]);
    const auto cancelled = ReadTransferEvent(ordered[1]);
    Expect(queued.has_value() && cancelled.has_value() &&
               queued->state == XNN_TRANSFER_TRANSFER_STATE_QUEUED &&
               cancelled->state == XNN_TRANSFER_TRANSFER_STATE_CANCELLED &&
               queued->snapshot_revision < cancelled->snapshot_revision,
           "shutdown preserves transfer mutation publication order");
  }
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
  TestPairingAbiValidationAndLifecycle();
  TestTransferAbiValidationAndLifecycle();
  TestPairingBridgeDecisionsTrustAndShutdown();
  TestPairingTrustPaginationAndOverflow();
  TestTransferCallbackDrainsCopiedEvent();
  TestTransferBridgeCommandsProgressAndRecovery();
  TestTransferBridgeShutdownBarrier();
  TestTransferShutdownWaitsForBackendCommand();
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
