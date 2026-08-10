#include "xnn_transfer/c_api.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "discovery_bridge.hpp"
#include "event_channel.hpp"
#include "pairing_bridge.hpp"
#include "transfer_bridge.hpp"
#include "xnn_transfer/core/discovery/discovery.hpp"
#include "xnn_transfer/core/engine.hpp"
#include "xnn_transfer/core/security/tls/security_profile.hpp"
#include "xnn_transfer/core/session/session.hpp"
#include "xnn_transfer/core/transfer/transfer.hpp"

namespace {

using xnn_transfer::bridge::DiscoveryPeerRegistry;
using xnn_transfer::bridge::EventChannel;
using xnn_transfer::bridge::PairingBackend;
using xnn_transfer::bridge::PairingBridge;
using xnn_transfer::bridge::TransferBackend;
using xnn_transfer::bridge::TransferBridge;
using xnn_transfer::bridge::TransferStartResult;
using xnn_transfer::core::discovery::CandidateEvent;
using xnn_transfer::core::discovery::DiscoveryConfig;
using xnn_transfer::core::discovery::DisplayLabelValidator;
using xnn_transfer::core::discovery::MakeUtf8procDisplayLabelValidator;
using xnn_transfer::core::discovery::SystemDiscoveryRuntime;
namespace session = xnn_transfer::core::session;
namespace tls = xnn_transfer::core::security::tls;
namespace transfer = xnn_transfer::core::transfer;

class DiscoveryController final {
 public:
  DiscoveryController() = default;

  DiscoveryController(const DiscoveryController&) = delete;
  DiscoveryController& operator=(const DiscoveryController&) = delete;

  [[nodiscard]] xnn_transfer_status Start(DiscoveryConfig config,
                                          EventChannel& events) {
    if (runtime_ != nullptr) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }

    EventChannel* const event_channel = &events;
    auto runtime = std::make_unique<SystemDiscoveryRuntime>(
        std::move(config), [this, event_channel](const CandidateEvent& event) {
          const std::optional<xnn_transfer_discovery_peer_event_payload> payload =
              registry_.Apply(event);
          if (payload.has_value()) {
            event_channel->EnqueueDiscovery(*payload);
          }
        });
    if (!runtime->Start()) {
      return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
    }
    runtime_ = std::move(runtime);
    return XNN_TRANSFER_STATUS_OK;
  }

  void Stop(EventChannel& events) {
    if (runtime_ != nullptr) {
      runtime_->Stop();
      runtime_.reset();
    }
    registry_.Clear(
        XNN_TRANSFER_DISCOVERY_EXPIRY_DISCOVERY_STOPPED,
        [&events](const xnn_transfer_discovery_peer_event_payload& payload) {
          events.EnqueueDiscovery(payload);
        });
  }

  [[nodiscard]] xnn_transfer_status Snapshot(
      const std::uint64_t expected_revision, const std::uint32_t offset,
      xnn_transfer_discovery_snapshot_page* const out_page) const {
    return registry_.Snapshot(expected_revision, offset, out_page);
  }

  [[nodiscard]] bool Contains(const std::uint64_t peer_id) const {
    return registry_.Contains(peer_id);
  }

 private:
  DiscoveryPeerRegistry registry_;
  std::unique_ptr<SystemDiscoveryRuntime> runtime_;
};

class SessionPairingBackend final : public PairingBackend {
 public:
  [[nodiscard]] xnn_transfer_status OpenWindow(
      const std::uint64_t now_ms, const std::uint64_t duration_ms) override {
    return admission_.OpenWindow(now_ms, duration_ms)
               ? XNN_TRANSFER_STATUS_OK
               : XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }

  void CloseWindow() override { admission_.CloseWindow(); }

  [[nodiscard]] xnn_transfer_status Start(const std::uint64_t peer_id,
                                          const std::uint64_t now_ms) override {
    static_cast<void>(peer_id);
    if (!admission_.window_open(now_ms)) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }
    // The production TLS connection dispatcher is a governed follow-up.
    return XNN_TRANSFER_STATUS_UNAVAILABLE;
  }

  [[nodiscard]] session::PairingUpdate Decide(const session::AttemptHandle& attempt,
                                              const tls::ConfirmationDecision decision,
                                              const std::uint64_t now_ms) override {
    static_cast<void>(attempt);
    static_cast<void>(decision);
    static_cast<void>(now_ms);
    return session::PairingUpdate{
        .state = session::PairingState::kClosed,
        .error = session::PairingError::kStateViolation,
        .terminal = true,
    };
  }

  [[nodiscard]] xnn_transfer_status Revoke(
      const xnn_transfer::core::security::identity::DeviceId& device_id) override {
    static_cast<void>(device_id);
    return XNN_TRANSFER_STATUS_UNAVAILABLE;
  }

  void Shutdown() override { admission_.CloseWindow(); }

 private:
  session::PairingAdmissionController admission_;
};

class SessionTransferBackend final : public TransferBackend {
 public:
  [[nodiscard]] TransferStartResult Send(
      const xnn_transfer::core::security::identity::DeviceId& device_id,
      const std::span<const std::uint8_t> path, const std::uint64_t now_ms) override {
    static_cast<void>(device_id);
    static_cast<void>(path);
    static_cast<void>(now_ms);
    // XT-037 owns the production authenticated transport and file adapter.
    return TransferStartResult{.status = XNN_TRANSFER_STATUS_UNAVAILABLE};
  }

  [[nodiscard]] xnn_transfer_status Accept(const transfer::TransferId& transfer_id,
                                           const std::uint64_t now_ms) override {
    static_cast<void>(transfer_id);
    static_cast<void>(now_ms);
    return XNN_TRANSFER_STATUS_UNAVAILABLE;
  }

  [[nodiscard]] xnn_transfer_status Reject(const transfer::TransferId& transfer_id,
                                           const std::uint64_t now_ms) override {
    static_cast<void>(transfer_id);
    static_cast<void>(now_ms);
    return XNN_TRANSFER_STATUS_UNAVAILABLE;
  }

  [[nodiscard]] xnn_transfer_status Cancel(const transfer::TransferId& transfer_id,
                                           const std::uint64_t now_ms) override {
    static_cast<void>(transfer_id);
    static_cast<void>(now_ms);
    return XNN_TRANSFER_STATUS_UNAVAILABLE;
  }

  void Shutdown() override {}
};

[[nodiscard]] std::uint64_t MonotonicMilliseconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

[[nodiscard]] bool IsZeroAttempt(
    const std::uint8_t (&attempt)[XNN_TRANSFER_PAIRING_ATTEMPT_ID_SIZE]) {
  return std::all_of(std::begin(attempt), std::end(attempt),
                     [](const std::uint8_t value) { return value == 0; });
}

[[nodiscard]] bool IsZeroTransfer(
    const std::uint8_t (&transfer_id)[XNN_TRANSFER_TRANSFER_ID_SIZE]) {
  return std::all_of(std::begin(transfer_id), std::end(transfer_id),
                     [](const std::uint8_t value) { return value == 0; });
}

[[nodiscard]] transfer::TransferId CopyTransferId(
    const std::uint8_t (&transfer_id)[XNN_TRANSFER_TRANSFER_ID_SIZE]) {
  transfer::TransferId output{};
  std::copy(std::begin(transfer_id), std::end(transfer_id), output.begin());
  return output;
}

[[nodiscard]] bool IsDiscoveryConfigValid(const xnn_transfer_discovery_config& config) {
  if (config.service_port == 0 || config.reserved != 0 ||
      config.display_label_size > XNN_TRANSFER_DISCOVERY_DISPLAY_LABEL_MAX_SIZE) {
    return false;
  }
  if (config.display_label_size == 0) {
    return true;
  }

  const std::shared_ptr<const DisplayLabelValidator> validator =
      MakeUtf8procDisplayLabelValidator();
  return validator != nullptr && validator->IsCanonical(std::span<const std::uint8_t>(
                                     config.display_label, config.display_label_size));
}

[[nodiscard]] DiscoveryConfig CopyDiscoveryConfig(
    const xnn_transfer_discovery_config& config) {
  return DiscoveryConfig{
      .service_port = config.service_port,
      .display_label = std::string(reinterpret_cast<const char*>(config.display_label),
                                   config.display_label_size),
  };
}

}  // namespace

struct xnn_transfer_engine {
  xnn_transfer_engine() { events.EnqueueState(XNN_TRANSFER_ENGINE_STATE_CREATED); }

  xnn_transfer::core::Engine implementation;
  EventChannel events;
  DiscoveryController discovery;
  SessionPairingBackend pairing_backend;
  PairingBridge pairing{pairing_backend};
  SessionTransferBackend transfer_backend;
  TransferBridge transfer{transfer_backend};
  std::mutex lifecycle_mutex;
  std::atomic<xnn_transfer_engine_state> state{XNN_TRANSFER_ENGINE_STATE_CREATED};
};

uint32_t xnn_transfer_abi_version(void) { return XNN_TRANSFER_ABI_VERSION; }

xnn_transfer_status xnn_transfer_engine_create(
    const xnn_transfer_engine_config* const config,
    xnn_transfer_engine** const out_engine) {
  if (out_engine == nullptr) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  *out_engine = nullptr;

  if (config == nullptr || config->struct_size < sizeof(xnn_transfer_engine_config)) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (config->abi_version != XNN_TRANSFER_ABI_VERSION) {
    return XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI;
  }

  try {
    auto* const engine = new (std::nothrow) xnn_transfer_engine();
    if (engine == nullptr) {
      return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
    }

    *out_engine = engine;
    return XNN_TRANSFER_STATUS_OK;
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

void xnn_transfer_engine_destroy(xnn_transfer_engine* const engine) {
  if (engine == nullptr) {
    return;
  }

  try {
    if (engine->events.IsCallbackThread()) {
      return;
    }

    static_cast<void>(xnn_transfer_engine_stop(engine));
    delete engine;
  } catch (...) {
    // Destruction must not leak a C++ exception through the C ABI.
  }
}

xnn_transfer_status xnn_transfer_engine_start(xnn_transfer_engine* const engine) {
  if (engine == nullptr) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (engine->events.IsCallbackThread()) {
    return XNN_TRANSFER_STATUS_INVALID_STATE;
  }

  try {
    const std::scoped_lock lock(engine->lifecycle_mutex);
    const xnn_transfer_engine_state state = engine->state.load();
    if (state == XNN_TRANSFER_ENGINE_STATE_STOPPING ||
        state == XNN_TRANSFER_ENGINE_STATE_STOPPED) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }
    if (state == XNN_TRANSFER_ENGINE_STATE_RUNNING) {
      return XNN_TRANSFER_STATUS_OK;
    }
    if (!engine->implementation.Start()) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }

    engine->state.store(XNN_TRANSFER_ENGINE_STATE_RUNNING);
    engine->events.EnqueueState(XNN_TRANSFER_ENGINE_STATE_RUNNING);
    return XNN_TRANSFER_STATUS_OK;
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

xnn_transfer_status xnn_transfer_engine_stop(xnn_transfer_engine* const engine) {
  if (engine == nullptr) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (engine->events.IsCallbackThread()) {
    return XNN_TRANSFER_STATUS_INVALID_STATE;
  }

  try {
    const std::scoped_lock lock(engine->lifecycle_mutex);
    if (engine->state.load() == XNN_TRANSFER_ENGINE_STATE_STOPPED) {
      engine->events.FinishShutdown();
      return XNN_TRANSFER_STATUS_OK;
    }

    engine->events.BeginShutdown();
    engine->state.store(XNN_TRANSFER_ENGINE_STATE_STOPPING);
    engine->events.EnqueueState(XNN_TRANSFER_ENGINE_STATE_STOPPING);
    xnn_transfer_status result = XNN_TRANSFER_STATUS_OK;
    try {
      engine->transfer.Shutdown(engine->events);
    } catch (...) {
      result = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
    }
    try {
      engine->pairing.Shutdown(engine->events);
    } catch (...) {
      result = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
    }
    try {
      engine->discovery.Stop(engine->events);
    } catch (...) {
      result = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
    }
    try {
      engine->implementation.Stop();
    } catch (...) {
      result = XNN_TRANSFER_STATUS_INTERNAL_ERROR;
    }

    engine->state.store(XNN_TRANSFER_ENGINE_STATE_STOPPED);
    engine->events.EnqueueState(XNN_TRANSFER_ENGINE_STATE_STOPPED);
    engine->events.FinishShutdown();
    return result;
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

xnn_transfer_status xnn_transfer_engine_get_state(
    const xnn_transfer_engine* const engine,
    xnn_transfer_engine_state* const out_state) {
  if (engine == nullptr || out_state == nullptr) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }

  try {
    *out_state = engine->state.load();
    return XNN_TRANSFER_STATUS_OK;
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

xnn_transfer_status xnn_transfer_engine_set_event_callback(
    xnn_transfer_engine* const engine,
    const xnn_transfer_event_callback_config* const config) {
  if (engine == nullptr) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (config != nullptr) {
    if (config->struct_size < sizeof(xnn_transfer_event_callback_config) ||
        config->callback == nullptr) {
      return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
    }
    if (config->abi_version != XNN_TRANSFER_ABI_VERSION) {
      return XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI;
    }
  }

  try {
    return engine->events.SetCallback(config);
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

xnn_transfer_status xnn_transfer_engine_poll_event(
    xnn_transfer_engine* const engine, xnn_transfer_event* const out_event) {
  if (engine == nullptr || out_event == nullptr ||
      out_event->struct_size < sizeof(xnn_transfer_event)) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (out_event->abi_version != XNN_TRANSFER_ABI_VERSION) {
    return XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI;
  }

  try {
    return engine->events.Poll(out_event);
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

xnn_transfer_status xnn_transfer_discovery_start(
    xnn_transfer_engine* const engine,
    const xnn_transfer_discovery_config* const config) {
  if (engine == nullptr || config == nullptr ||
      config->struct_size < sizeof(xnn_transfer_discovery_config)) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (config->abi_version != XNN_TRANSFER_ABI_VERSION) {
    return XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI;
  }
  if (engine->events.IsCallbackThread()) {
    return XNN_TRANSFER_STATUS_INVALID_STATE;
  }

  try {
    if (!IsDiscoveryConfigValid(*config)) {
      return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
    }
    const std::scoped_lock lock(engine->lifecycle_mutex);
    if (engine->state.load() != XNN_TRANSFER_ENGINE_STATE_RUNNING) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }
    return engine->discovery.Start(CopyDiscoveryConfig(*config), engine->events);
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

xnn_transfer_status xnn_transfer_discovery_stop(xnn_transfer_engine* const engine) {
  if (engine == nullptr) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (engine->events.IsCallbackThread()) {
    return XNN_TRANSFER_STATUS_INVALID_STATE;
  }

  try {
    const std::scoped_lock lock(engine->lifecycle_mutex);
    if (engine->state.load() != XNN_TRANSFER_ENGINE_STATE_RUNNING) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }
    engine->discovery.Stop(engine->events);
    return XNN_TRANSFER_STATUS_OK;
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

xnn_transfer_status xnn_transfer_discovery_get_snapshot(
    xnn_transfer_engine* const engine, const std::uint64_t expected_revision,
    const std::uint32_t offset, xnn_transfer_discovery_snapshot_page* const out_page) {
  if (engine == nullptr || out_page == nullptr ||
      out_page->struct_size < sizeof(xnn_transfer_discovery_snapshot_page)) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (out_page->abi_version != XNN_TRANSFER_ABI_VERSION) {
    return XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI;
  }
  if (engine->events.IsCallbackThread()) {
    return XNN_TRANSFER_STATUS_INVALID_STATE;
  }

  try {
    const std::scoped_lock lock(engine->lifecycle_mutex);
    const xnn_transfer_engine_state state = engine->state.load();
    if (state == XNN_TRANSFER_ENGINE_STATE_CREATED ||
        state == XNN_TRANSFER_ENGINE_STATE_STOPPING) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }
    return engine->discovery.Snapshot(expected_revision, offset, out_page);
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

xnn_transfer_status xnn_transfer_pairing_open_window(
    xnn_transfer_engine* const engine,
    const xnn_transfer_pairing_window_config* const config) {
  if (engine == nullptr || config == nullptr ||
      config->struct_size < sizeof(xnn_transfer_pairing_window_config)) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (config->abi_version != XNN_TRANSFER_ABI_VERSION) {
    return XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI;
  }
  if (config->reserved != 0 || config->duration_ms == 0 ||
      config->duration_ms > session::kMaximumPairingWindowMs) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (engine->events.IsCallbackThread()) {
    return XNN_TRANSFER_STATUS_INVALID_STATE;
  }

  try {
    const std::scoped_lock lock(engine->lifecycle_mutex);
    if (engine->state.load() != XNN_TRANSFER_ENGINE_STATE_RUNNING) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }
    return engine->pairing.OpenWindow(MonotonicMilliseconds(), config->duration_ms);
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

xnn_transfer_status xnn_transfer_pairing_close_window(
    xnn_transfer_engine* const engine) {
  if (engine == nullptr) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (engine->events.IsCallbackThread()) {
    return XNN_TRANSFER_STATUS_INVALID_STATE;
  }

  try {
    const std::scoped_lock lock(engine->lifecycle_mutex);
    if (engine->state.load() != XNN_TRANSFER_ENGINE_STATE_RUNNING) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }
    return engine->pairing.CloseWindow(engine->events);
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

xnn_transfer_status xnn_transfer_pairing_start(
    xnn_transfer_engine* const engine,
    const xnn_transfer_pairing_start_request* const request) {
  if (engine == nullptr || request == nullptr ||
      request->struct_size < sizeof(xnn_transfer_pairing_start_request)) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (request->abi_version != XNN_TRANSFER_ABI_VERSION) {
    return XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI;
  }
  if (request->reserved != 0 || request->peer_id == 0) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (engine->events.IsCallbackThread()) {
    return XNN_TRANSFER_STATUS_INVALID_STATE;
  }

  try {
    const std::scoped_lock lock(engine->lifecycle_mutex);
    if (engine->state.load() != XNN_TRANSFER_ENGINE_STATE_RUNNING) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }
    if (!engine->discovery.Contains(request->peer_id)) {
      return XNN_TRANSFER_STATUS_STALE_HANDLE;
    }
    return engine->pairing.Start(request->peer_id, MonotonicMilliseconds());
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

namespace {

[[nodiscard]] xnn_transfer_status PairingDecide(
    xnn_transfer_engine* const engine,
    const xnn_transfer_pairing_attempt_ref* const attempt,
    const tls::ConfirmationDecision decision) {
  if (engine == nullptr || attempt == nullptr ||
      attempt->struct_size < sizeof(xnn_transfer_pairing_attempt_ref)) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (attempt->abi_version != XNN_TRANSFER_ABI_VERSION) {
    return XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI;
  }
  if (attempt->reserved != 0 || IsZeroAttempt(attempt->attempt_id)) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (engine->events.IsCallbackThread()) {
    return XNN_TRANSFER_STATUS_INVALID_STATE;
  }

  try {
    const std::scoped_lock lock(engine->lifecycle_mutex);
    if (engine->state.load() != XNN_TRANSFER_ENGINE_STATE_RUNNING) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }
    return engine->pairing.Decide(
        std::span<const std::uint8_t, XNN_TRANSFER_PAIRING_ATTEMPT_ID_SIZE>(
            attempt->attempt_id),
        decision, MonotonicMilliseconds(), engine->events);
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

}  // namespace

xnn_transfer_status xnn_transfer_pairing_confirm(
    xnn_transfer_engine* const engine,
    const xnn_transfer_pairing_attempt_ref* const attempt) {
  return PairingDecide(engine, attempt, tls::ConfirmationDecision::kConfirm);
}

xnn_transfer_status xnn_transfer_pairing_reject(
    xnn_transfer_engine* const engine,
    const xnn_transfer_pairing_attempt_ref* const attempt) {
  return PairingDecide(engine, attempt, tls::ConfirmationDecision::kReject);
}

xnn_transfer_status xnn_transfer_pairing_revoke(
    xnn_transfer_engine* const engine, const xnn_transfer_trust_ref* const trust) {
  if (engine == nullptr || trust == nullptr ||
      trust->struct_size < sizeof(xnn_transfer_trust_ref)) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (trust->abi_version != XNN_TRANSFER_ABI_VERSION) {
    return XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI;
  }
  if (trust->reserved != 0 || trust->trust_id == 0) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (engine->events.IsCallbackThread()) {
    return XNN_TRANSFER_STATUS_INVALID_STATE;
  }

  try {
    const std::scoped_lock lock(engine->lifecycle_mutex);
    if (engine->state.load() != XNN_TRANSFER_ENGINE_STATE_RUNNING) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }
    return engine->pairing.Revoke(trust->trust_id, engine->events);
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

xnn_transfer_status xnn_transfer_pairing_get_snapshot(
    xnn_transfer_engine* const engine,
    xnn_transfer_pairing_snapshot* const out_snapshot) {
  if (engine == nullptr || out_snapshot == nullptr ||
      out_snapshot->struct_size < sizeof(xnn_transfer_pairing_snapshot)) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (out_snapshot->abi_version != XNN_TRANSFER_ABI_VERSION) {
    return XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI;
  }
  if (engine->events.IsCallbackThread()) {
    return XNN_TRANSFER_STATUS_INVALID_STATE;
  }

  try {
    const std::scoped_lock lock(engine->lifecycle_mutex);
    const xnn_transfer_engine_state state = engine->state.load();
    if (state == XNN_TRANSFER_ENGINE_STATE_CREATED ||
        state == XNN_TRANSFER_ENGINE_STATE_STOPPING) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }
    return engine->pairing.Snapshot(out_snapshot);
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

xnn_transfer_status xnn_transfer_trust_get_snapshot(
    xnn_transfer_engine* const engine, const std::uint64_t expected_revision,
    const std::uint32_t offset, xnn_transfer_trust_snapshot_page* const out_page) {
  if (engine == nullptr || out_page == nullptr ||
      out_page->struct_size < sizeof(xnn_transfer_trust_snapshot_page)) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (out_page->abi_version != XNN_TRANSFER_ABI_VERSION) {
    return XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI;
  }
  if (engine->events.IsCallbackThread()) {
    return XNN_TRANSFER_STATUS_INVALID_STATE;
  }

  try {
    const std::scoped_lock lock(engine->lifecycle_mutex);
    const xnn_transfer_engine_state state = engine->state.load();
    if (state == XNN_TRANSFER_ENGINE_STATE_CREATED ||
        state == XNN_TRANSFER_ENGINE_STATE_STOPPING) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }
    return engine->pairing.TrustSnapshot(expected_revision, offset, out_page);
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

xnn_transfer_status xnn_transfer_transfer_send(
    xnn_transfer_engine* const engine,
    const xnn_transfer_transfer_send_request* const request,
    xnn_transfer_transfer_ref* const out_transfer) {
  if (engine == nullptr || request == nullptr || out_transfer == nullptr ||
      request->struct_size < sizeof(xnn_transfer_transfer_send_request) ||
      out_transfer->struct_size < sizeof(xnn_transfer_transfer_ref)) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (request->abi_version != XNN_TRANSFER_ABI_VERSION ||
      out_transfer->abi_version != XNN_TRANSFER_ABI_VERSION) {
    return XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI;
  }
  if (request->reserved != 0 || request->reserved2 != 0 || request->trust_id == 0 ||
      request->path_size == 0 ||
      request->path_size > XNN_TRANSFER_TRANSFER_PATH_MAX_SIZE) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (engine->events.IsCallbackThread()) {
    return XNN_TRANSFER_STATUS_INVALID_STATE;
  }

  std::fill(std::begin(out_transfer->transfer_id), std::end(out_transfer->transfer_id),
            std::uint8_t{0});
  try {
    const std::scoped_lock lock(engine->lifecycle_mutex);
    if (engine->state.load() != XNN_TRANSFER_ENGINE_STATE_RUNNING) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }

    xnn_transfer::core::security::identity::DeviceId device_id{};
    const xnn_transfer_status trust_status =
        engine->pairing.ResolveActiveTrust(request->trust_id, &device_id);
    if (trust_status != XNN_TRANSFER_STATUS_OK) {
      return trust_status;
    }
    return engine->transfer.Send(
        device_id, std::span<const std::uint8_t>(request->path, request->path_size),
        MonotonicMilliseconds(), out_transfer, engine->events);
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

namespace {

enum class TransferCommand {
  kAccept,
  kReject,
  kCancel,
};

[[nodiscard]] xnn_transfer_status ExecuteTransferCommand(
    xnn_transfer_engine* const engine,
    const xnn_transfer_transfer_ref* const transfer_ref,
    const TransferCommand command) {
  if (engine == nullptr || transfer_ref == nullptr ||
      transfer_ref->struct_size < sizeof(xnn_transfer_transfer_ref)) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (transfer_ref->abi_version != XNN_TRANSFER_ABI_VERSION) {
    return XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI;
  }
  if (transfer_ref->reserved != 0 || IsZeroTransfer(transfer_ref->transfer_id)) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (engine->events.IsCallbackThread()) {
    return XNN_TRANSFER_STATUS_INVALID_STATE;
  }

  try {
    const std::scoped_lock lock(engine->lifecycle_mutex);
    if (engine->state.load() != XNN_TRANSFER_ENGINE_STATE_RUNNING) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }
    const transfer::TransferId transfer_id = CopyTransferId(transfer_ref->transfer_id);
    switch (command) {
      case TransferCommand::kAccept:
        return engine->transfer.Accept(transfer_id, MonotonicMilliseconds(),
                                       engine->events);
      case TransferCommand::kReject:
        return engine->transfer.Reject(transfer_id, MonotonicMilliseconds(),
                                       engine->events);
      case TransferCommand::kCancel:
        return engine->transfer.Cancel(transfer_id, MonotonicMilliseconds(),
                                       engine->events);
    }
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

}  // namespace

xnn_transfer_status xnn_transfer_transfer_accept(
    xnn_transfer_engine* const engine,
    const xnn_transfer_transfer_ref* const transfer_ref) {
  return ExecuteTransferCommand(engine, transfer_ref, TransferCommand::kAccept);
}

xnn_transfer_status xnn_transfer_transfer_reject(
    xnn_transfer_engine* const engine,
    const xnn_transfer_transfer_ref* const transfer_ref) {
  return ExecuteTransferCommand(engine, transfer_ref, TransferCommand::kReject);
}

xnn_transfer_status xnn_transfer_transfer_cancel(
    xnn_transfer_engine* const engine,
    const xnn_transfer_transfer_ref* const transfer_ref) {
  return ExecuteTransferCommand(engine, transfer_ref, TransferCommand::kCancel);
}

xnn_transfer_status xnn_transfer_transfer_get_snapshot(
    xnn_transfer_engine* const engine, const std::uint64_t expected_revision,
    const std::uint32_t offset, xnn_transfer_transfer_snapshot_page* const out_page) {
  if (engine == nullptr || out_page == nullptr ||
      out_page->struct_size < sizeof(xnn_transfer_transfer_snapshot_page)) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }
  if (out_page->abi_version != XNN_TRANSFER_ABI_VERSION) {
    return XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI;
  }
  if (engine->events.IsCallbackThread()) {
    return XNN_TRANSFER_STATUS_INVALID_STATE;
  }

  try {
    const std::scoped_lock lock(engine->lifecycle_mutex);
    const xnn_transfer_engine_state state = engine->state.load();
    if (state == XNN_TRANSFER_ENGINE_STATE_CREATED ||
        state == XNN_TRANSFER_ENGINE_STATE_STOPPING) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }
    return engine->transfer.Snapshot(expected_revision, offset, out_page);
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}
