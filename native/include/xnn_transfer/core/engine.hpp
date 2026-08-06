#ifndef XNN_TRANSFER_CORE_ENGINE_HPP_
#define XNN_TRANSFER_CORE_ENGINE_HPP_

#include <mutex>

namespace xnn_transfer::core {

enum class EngineState {
  kCreated,
  kRunning,
  kStopped,
};

class Engine final {
 public:
  Engine() = default;

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) = delete;
  Engine& operator=(Engine&&) = delete;

  [[nodiscard]] bool Start();
  void Stop();
  [[nodiscard]] EngineState state() const;

 private:
  mutable std::mutex mutex_;
  EngineState state_{EngineState::kCreated};
};

}  // namespace xnn_transfer::core

#endif  // XNN_TRANSFER_CORE_ENGINE_HPP_
