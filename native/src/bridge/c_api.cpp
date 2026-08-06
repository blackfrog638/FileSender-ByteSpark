#include "xnn_transfer/c_api.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>

#include "xnn_transfer/core/engine.hpp"

namespace {

struct QueuedEvent {
  uint64_t sequence = 0;
  uint32_t type = 0;
  uint32_t payload_version = 0;
  uint32_t payload_size = 0;
  uint32_t flags = XNN_TRANSFER_EVENT_FLAG_NONE;
  std::array<uint8_t, XNN_TRANSFER_EVENT_PAYLOAD_MAX_SIZE> payload{};
};

struct CallbackTarget {
  xnn_transfer_event_wakeup_callback callback = nullptr;
  void* user_data = nullptr;
};

class EventChannel final {
 public:
  EventChannel() = default;

  EventChannel(const EventChannel&) = delete;
  EventChannel& operator=(const EventChannel&) = delete;

  void EnqueueState(const xnn_transfer_engine_state state) {
    static_assert(sizeof(xnn_transfer_engine_state_event_payload) <=
                  XNN_TRANSFER_EVENT_PAYLOAD_MAX_SIZE);

    xnn_transfer_engine_state_event_payload payload;
    std::memset(&payload, 0, sizeof(payload));
    payload.struct_size = sizeof(xnn_transfer_engine_state_event_payload);
    payload.abi_version = XNN_TRANSFER_ABI_VERSION;
    payload.state = static_cast<uint32_t>(state);
    QueuedEvent event{
        .type = XNN_TRANSFER_EVENT_TYPE_ENGINE_STATE_CHANGED,
        .payload_version = XNN_TRANSFER_ENGINE_STATE_EVENT_PAYLOAD_VERSION,
        .payload_size =
            static_cast<uint32_t>(sizeof(xnn_transfer_engine_state_event_payload)),
    };
    std::memcpy(event.payload.data(), &payload, sizeof(payload));
    Enqueue(event);
  }

  xnn_transfer_status SetCallback(
      const xnn_transfer_event_callback_config* const config) {
    CallbackTarget target;
    {
      std::unique_lock lock(mutex_);
      if (config == nullptr) {
        callback_ = nullptr;
        callback_user_data_ = nullptr;
        callback_pending_ = false;
        if (callback_active_ && callback_thread_ != std::this_thread::get_id()) {
          callback_finished_.wait(lock, [this] { return !callback_active_; });
        }
        return XNN_TRANSFER_STATUS_OK;
      }

      if (!callback_registration_open_ || callback_ != nullptr || callback_active_) {
        return XNN_TRANSFER_STATUS_INVALID_STATE;
      }

      callback_ = config->callback;
      callback_user_data_ = config->user_data;
      if (size_ != 0) {
        target = PrepareCallbackLocked();
      }
    }

    InvokeCallback(target);
    return XNN_TRANSFER_STATUS_OK;
  }

  xnn_transfer_status Poll(xnn_transfer_event* const out_event) {
    QueuedEvent event;
    {
      const std::scoped_lock lock(mutex_);
      if (size_ == 0) {
        return XNN_TRANSFER_STATUS_EVENT_QUEUE_EMPTY;
      }

      event = queue_[head_];
      head_ = (head_ + 1) % queue_.size();
      --size_;
    }

    const xnn_transfer_event output{
        .struct_size = sizeof(xnn_transfer_event),
        .abi_version = XNN_TRANSFER_ABI_VERSION,
        .type = event.type,
        .sequence = event.sequence,
        .payload_version = event.payload_version,
        .payload_size = event.payload_size,
        .flags = event.flags,
        .reserved = 0,
        .payload = {},
    };
    *out_event = output;
    std::memcpy(out_event->payload, event.payload.data(), event.payload_size);
    return XNN_TRANSFER_STATUS_OK;
  }

  void BeginShutdown() {
    const std::scoped_lock lock(mutex_);
    callback_registration_open_ = false;
  }

  void FinishShutdown() {
    std::unique_lock lock(mutex_);
    callback_registration_open_ = false;
    events_open_ = false;
    if (callback_active_ && callback_thread_ != std::this_thread::get_id()) {
      callback_finished_.wait(lock, [this] { return !callback_active_; });
    }
    callback_ = nullptr;
    callback_user_data_ = nullptr;
    callback_pending_ = false;
  }

  [[nodiscard]] bool IsCallbackThread() const {
    const std::scoped_lock lock(mutex_);
    return callback_active_ && callback_thread_ == std::this_thread::get_id();
  }

 private:
  void Enqueue(QueuedEvent event) {
    CallbackTarget target;
    {
      const std::scoped_lock lock(mutex_);
      if (!events_open_) {
        return;
      }

      event.sequence = next_sequence_++;
      if (size_ == queue_.size()) {
        head_ = (head_ + 1) % queue_.size();
        --size_;
        event.flags |= XNN_TRANSFER_EVENT_FLAG_EVENTS_DROPPED_BEFORE;
      }

      const bool was_empty = size_ == 0;
      const size_t tail = (head_ + size_) % queue_.size();
      queue_[tail] = event;
      ++size_;
      if (was_empty) {
        if (callback_active_) {
          callback_pending_ = callback_ != nullptr;
        } else {
          target = PrepareCallbackLocked();
        }
      }
    }

    InvokeCallback(target);
  }

  CallbackTarget PrepareCallbackLocked() {
    if (callback_ == nullptr || callback_active_) {
      return {};
    }

    callback_active_ = true;
    callback_thread_ = std::this_thread::get_id();
    return CallbackTarget{
        .callback = callback_,
        .user_data = callback_user_data_,
    };
  }

  void InvokeCallback(CallbackTarget target) {
    while (target.callback != nullptr) {
      try {
        target.callback(target.user_data);
      } catch (...) {
        // A foreign callback must not unwind through the bridge.
      }

      {
        const std::scoped_lock lock(mutex_);
        callback_active_ = false;
        callback_thread_ = {};
        target = {};
        if (callback_pending_) {
          callback_pending_ = false;
          target = PrepareCallbackLocked();
        }
      }
    }
    callback_finished_.notify_all();
  }

  mutable std::mutex mutex_;
  std::condition_variable callback_finished_;
  std::array<QueuedEvent, XNN_TRANSFER_EVENT_QUEUE_CAPACITY> queue_{};
  size_t head_ = 0;
  size_t size_ = 0;
  uint64_t next_sequence_ = 1;
  xnn_transfer_event_wakeup_callback callback_ = nullptr;
  void* callback_user_data_ = nullptr;
  std::thread::id callback_thread_;
  bool callback_registration_open_ = true;
  bool events_open_ = true;
  bool callback_active_ = false;
  bool callback_pending_ = false;
};

}  // namespace

struct xnn_transfer_engine {
  xnn_transfer_engine() { events.EnqueueState(XNN_TRANSFER_ENGINE_STATE_CREATED); }

  xnn_transfer::core::Engine implementation;
  EventChannel events;
  std::mutex lifecycle_mutex;
  std::atomic<xnn_transfer_engine_state> state{XNN_TRANSFER_ENGINE_STATE_CREATED};
};

uint32_t xnn_transfer_abi_version(void) { return XNN_TRANSFER_ABI_VERSION; }

xnn_transfer_status xnn_transfer_engine_create(const xnn_transfer_engine_config* config,
                                               xnn_transfer_engine** out_engine) {
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

void xnn_transfer_engine_destroy(xnn_transfer_engine* engine) {
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

xnn_transfer_status xnn_transfer_engine_start(xnn_transfer_engine* engine) {
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

xnn_transfer_status xnn_transfer_engine_stop(xnn_transfer_engine* engine) {
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
    const xnn_transfer_engine* engine, xnn_transfer_engine_state* out_state) {
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
    xnn_transfer_engine* engine, const xnn_transfer_event_callback_config* config) {
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

xnn_transfer_status xnn_transfer_engine_poll_event(xnn_transfer_engine* engine,
                                                   xnn_transfer_event* out_event) {
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
