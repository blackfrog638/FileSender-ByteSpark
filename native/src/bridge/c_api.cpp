#include "xnn_transfer/c_api.h"

#include <new>

#include "xnn_transfer/core/engine.hpp"

struct xnn_transfer_engine {
  xnn_transfer::core::Engine implementation;
};

namespace {

xnn_transfer_engine_state ToCState(const xnn_transfer::core::EngineState state) {
  switch (state) {
    case xnn_transfer::core::EngineState::kCreated:
      return XNN_TRANSFER_ENGINE_STATE_CREATED;
    case xnn_transfer::core::EngineState::kRunning:
      return XNN_TRANSFER_ENGINE_STATE_RUNNING;
    case xnn_transfer::core::EngineState::kStopped:
      return XNN_TRANSFER_ENGINE_STATE_STOPPED;
  }

  return XNN_TRANSFER_ENGINE_STATE_STOPPED;
}

}  // namespace

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
    engine->implementation.Stop();
  } catch (...) {
    // Destruction must not leak a C++ exception through the C ABI.
  }
  delete engine;
}

xnn_transfer_status xnn_transfer_engine_start(xnn_transfer_engine* engine) {
  if (engine == nullptr) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }

  try {
    return engine->implementation.Start() ? XNN_TRANSFER_STATUS_OK
                                          : XNN_TRANSFER_STATUS_INVALID_STATE;
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}

xnn_transfer_status xnn_transfer_engine_stop(xnn_transfer_engine* engine) {
  if (engine == nullptr) {
    return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
  }

  try {
    engine->implementation.Stop();
    return XNN_TRANSFER_STATUS_OK;
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
    *out_state = ToCState(engine->implementation.state());
    return XNN_TRANSFER_STATUS_OK;
  } catch (...) {
    return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
  }
}
