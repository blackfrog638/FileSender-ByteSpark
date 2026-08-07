#include <openssl/rand.h>

#include <asio/error_code.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <utility>

#include "runtime.hpp"

namespace xnn_transfer::core::discovery {
namespace {

class SteadyMonotonicClock final : public MonotonicClock {
 public:
  [[nodiscard]] std::uint64_t NowMs() const noexcept override {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    return static_cast<std::uint64_t>(elapsed.count());
  }
};

class OpenSslEntropySource final : public EntropySource {
 public:
  [[nodiscard]] bool Fill(const std::span<std::uint8_t> output) noexcept override {
    if (output.empty()) {
      return true;
    }
    if (output.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return false;
    }
    return RAND_bytes(output.data(), static_cast<int>(output.size())) == 1;
  }
};

struct TimerState {
  explicit TimerState(asio::any_io_executor executor) : timer(std::move(executor)) {}

  asio::steady_timer timer;
  std::mutex mutex;
  bool active{true};
  std::uint64_t generation{};
  DiscoveryTimer::Handler handler{};
};

class AsioDiscoveryTimer final : public DiscoveryTimer {
 public:
  explicit AsioDiscoveryTimer(asio::any_io_executor executor)
      : state_(std::make_shared<TimerState>(std::move(executor))) {}

  ~AsioDiscoveryTimer() override { Stop(); }

  [[nodiscard]] bool ScheduleAt(const std::uint64_t deadline_ms,
                                Handler handler) override {
    const std::scoped_lock lock(state_->mutex);
    if (!state_->active || !handler) {
      return false;
    }
    try {
      (void)state_->timer.cancel();
      ++state_->generation;
      const std::uint64_t generation = state_->generation;
      state_->handler = std::move(handler);
      (void)state_->timer.expires_at(std::chrono::steady_clock::time_point(
          std::chrono::milliseconds(deadline_ms)));
      const std::weak_ptr<TimerState> weak_state = state_;
      state_->timer.async_wait([weak_state,
                                generation](const asio::error_code& wait_error) {
        const std::shared_ptr<TimerState> state = weak_state.lock();
        if (state == nullptr || wait_error) {
          return;
        }
        DiscoveryTimer::Handler handler;
        {
          const std::scoped_lock lock(state->mutex);
          if (!state->active || state->generation != generation || !state->handler) {
            return;
          }
          handler = std::move(state->handler);
        }
        handler();
      });
    } catch (...) {
      ++state_->generation;
      state_->handler = {};
      try {
        (void)state_->timer.cancel();
      } catch (...) {
      }
      return false;
    }
    return true;
  }

  void Cancel() override {
    const std::scoped_lock lock(state_->mutex);
    ++state_->generation;
    state_->handler = {};
    try {
      (void)state_->timer.cancel();
    } catch (...) {
    }
  }

  void Stop() override {
    const std::scoped_lock lock(state_->mutex);
    if (!state_->active) {
      return;
    }
    state_->active = false;
    ++state_->generation;
    state_->handler = {};
    try {
      (void)state_->timer.cancel();
    } catch (...) {
    }
  }

 private:
  std::shared_ptr<TimerState> state_;
};

}  // namespace

std::unique_ptr<MonotonicClock> MakeSteadyMonotonicClock() {
  return std::make_unique<SteadyMonotonicClock>();
}

std::unique_ptr<EntropySource> MakeOpenSslEntropySource() {
  return std::make_unique<OpenSslEntropySource>();
}

std::unique_ptr<DiscoveryTimer> MakeAsioDiscoveryTimer(asio::any_io_executor executor) {
  return std::make_unique<AsioDiscoveryTimer>(std::move(executor));
}

}  // namespace xnn_transfer::core::discovery
