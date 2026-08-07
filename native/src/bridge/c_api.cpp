#include "xnn_transfer/c_api.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "discovery_bridge.hpp"
#include "event_channel.hpp"
#include "xnn_transfer/core/discovery/discovery.hpp"
#include "xnn_transfer/core/engine.hpp"

namespace {

using xnn_transfer::bridge::DiscoveryPeerRegistry;
using xnn_transfer::bridge::EventChannel;
using xnn_transfer::core::discovery::CandidateEvent;
using xnn_transfer::core::discovery::DiscoveryConfig;
using xnn_transfer::core::discovery::DisplayLabelValidator;
using xnn_transfer::core::discovery::MakeUtf8procDisplayLabelValidator;
using xnn_transfer::core::discovery::SystemDiscoveryRuntime;

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

 private:
  DiscoveryPeerRegistry registry_;
  std::unique_ptr<SystemDiscoveryRuntime> runtime_;
};

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
