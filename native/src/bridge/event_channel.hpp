#ifndef XNN_TRANSFER_BRIDGE_EVENT_CHANNEL_HPP_
#define XNN_TRANSFER_BRIDGE_EVENT_CHANNEL_HPP_

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>

#include "xnn_transfer/c_api.h"

namespace xnn_transfer::bridge {

struct QueuedEvent {
  std::uint64_t sequence{};
  std::uint32_t type{};
  std::uint32_t payload_version{};
  std::uint32_t payload_size{};
  std::uint32_t flags{XNN_TRANSFER_EVENT_FLAG_NONE};
  std::array<std::uint8_t, XNN_TRANSFER_EVENT_PAYLOAD_MAX_SIZE> payload{};
};

struct CallbackTarget {
  xnn_transfer_event_wakeup_callback callback{};
  void* user_data{};
};

class EventChannel final {
 public:
  EventChannel() = default;

  EventChannel(const EventChannel&) = delete;
  EventChannel& operator=(const EventChannel&) = delete;

  void EnqueueState(const xnn_transfer_engine_state state) {
    static_assert(sizeof(xnn_transfer_engine_state_event_payload) <=
                  XNN_TRANSFER_EVENT_PAYLOAD_MAX_SIZE);

    xnn_transfer_engine_state_event_payload payload{};
    payload.struct_size = sizeof(payload);
    payload.abi_version = XNN_TRANSFER_ABI_VERSION;
    payload.state = static_cast<std::uint32_t>(state);
    QueuedEvent event{
        .type = XNN_TRANSFER_EVENT_TYPE_ENGINE_STATE_CHANGED,
        .payload_version = XNN_TRANSFER_ENGINE_STATE_EVENT_PAYLOAD_VERSION,
        .payload_size = static_cast<std::uint32_t>(sizeof(payload)),
    };
    std::memcpy(event.payload.data(), &payload, sizeof(payload));
    Enqueue(event);
  }

  void EnqueueDiscovery(const xnn_transfer_discovery_peer_event_payload& payload) {
    static_assert(sizeof(payload) <= XNN_TRANSFER_EVENT_PAYLOAD_MAX_SIZE);

    QueuedEvent event{
        .type = XNN_TRANSFER_EVENT_TYPE_DISCOVERY_PEER_CHANGED,
        .payload_version = XNN_TRANSFER_DISCOVERY_PEER_EVENT_PAYLOAD_VERSION,
        .payload_size = static_cast<std::uint32_t>(sizeof(payload)),
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
      const std::size_t tail = (head_ + size_) % queue_.size();
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
  std::size_t head_{};
  std::size_t size_{};
  std::uint64_t next_sequence_{1};
  xnn_transfer_event_wakeup_callback callback_{};
  void* callback_user_data_{};
  std::thread::id callback_thread_;
  bool callback_registration_open_{true};
  bool events_open_{true};
  bool callback_active_{};
  bool callback_pending_{};
};

}  // namespace xnn_transfer::bridge

#endif  // XNN_TRANSFER_BRIDGE_EVENT_CHANNEL_HPP_
