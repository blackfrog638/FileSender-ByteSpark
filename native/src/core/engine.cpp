#include "xnn_transfer/core/engine.hpp"

#include <mutex>

namespace xnn_transfer::core {

bool Engine::Start() {
  const std::scoped_lock lock(mutex_);
  if (state_ == EngineState::kStopped) {
    return false;
  }

  state_ = EngineState::kRunning;
  return true;
}

void Engine::Stop() {
  const std::scoped_lock lock(mutex_);
  state_ = EngineState::kStopped;
}

EngineState Engine::state() const {
  const std::scoped_lock lock(mutex_);
  return state_;
}

}  // namespace xnn_transfer::core
