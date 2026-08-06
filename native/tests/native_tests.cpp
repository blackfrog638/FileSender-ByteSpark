#include <cstddef>
#include <iostream>

#include "xnn_transfer/c_api.h"

namespace {

int failures = 0;

void Expect(const bool condition, const char* const message) {
  if (condition) {
    return;
  }

  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

void TestInvalidArguments() {
  xnn_transfer_engine* engine = nullptr;
  xnn_transfer_engine_config config{
      .struct_size = sizeof(xnn_transfer_engine_config),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .reserved = 0,
  };

  Expect(xnn_transfer_engine_create(nullptr, &engine) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "create rejects a null config");
  Expect(xnn_transfer_engine_create(&config, nullptr) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "create rejects a null output");

  config.struct_size = offsetof(xnn_transfer_engine_config, reserved);
  Expect(xnn_transfer_engine_create(&config, &engine) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "create rejects a short config");

  config.struct_size = sizeof(xnn_transfer_engine_config);
  config.abi_version = XNN_TRANSFER_ABI_VERSION + 1;
  Expect(xnn_transfer_engine_create(&config, &engine) ==
             XNN_TRANSFER_STATUS_INCOMPATIBLE_ABI,
         "create rejects an unsupported ABI");

  Expect(xnn_transfer_engine_start(nullptr) == XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "start rejects a null engine");
  Expect(xnn_transfer_engine_stop(nullptr) == XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "stop rejects a null engine");
  Expect(xnn_transfer_engine_get_state(nullptr, nullptr) ==
             XNN_TRANSFER_STATUS_INVALID_ARGUMENT,
         "get state rejects null arguments");
  xnn_transfer_engine_destroy(nullptr);
}

void TestLifecycle() {
  xnn_transfer_engine_config config{
      .struct_size = sizeof(xnn_transfer_engine_config),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .reserved = 0,
  };
  xnn_transfer_engine* engine = nullptr;

  Expect(xnn_transfer_engine_create(&config, &engine) == XNN_TRANSFER_STATUS_OK,
         "create succeeds");
  Expect(engine != nullptr, "create returns an engine");
  if (engine == nullptr) {
    return;
  }

  xnn_transfer_engine_state state = XNN_TRANSFER_ENGINE_STATE_STOPPED;
  Expect(xnn_transfer_engine_get_state(engine, &state) == XNN_TRANSFER_STATUS_OK &&
             state == XNN_TRANSFER_ENGINE_STATE_CREATED,
         "new engine is created");
  Expect(xnn_transfer_engine_start(engine) == XNN_TRANSFER_STATUS_OK, "start succeeds");
  Expect(xnn_transfer_engine_start(engine) == XNN_TRANSFER_STATUS_OK,
         "start is idempotent");
  Expect(xnn_transfer_engine_get_state(engine, &state) == XNN_TRANSFER_STATUS_OK &&
             state == XNN_TRANSFER_ENGINE_STATE_RUNNING,
         "started engine is running");
  Expect(xnn_transfer_engine_stop(engine) == XNN_TRANSFER_STATUS_OK, "stop succeeds");
  Expect(xnn_transfer_engine_stop(engine) == XNN_TRANSFER_STATUS_OK,
         "stop is idempotent");
  Expect(xnn_transfer_engine_start(engine) == XNN_TRANSFER_STATUS_INVALID_STATE,
         "stopped engine cannot restart");
  Expect(xnn_transfer_engine_get_state(engine, &state) == XNN_TRANSFER_STATUS_OK &&
             state == XNN_TRANSFER_ENGINE_STATE_STOPPED,
         "stopped engine stays stopped");

  xnn_transfer_engine_destroy(engine);
}

}  // namespace

int main() {
  Expect(xnn_transfer_abi_version() == XNN_TRANSFER_ABI_VERSION,
         "runtime and header ABI versions match");
  TestInvalidArguments();
  TestLifecycle();

  if (failures != 0) {
    std::cerr << failures << " native test(s) failed\n";
    return 1;
  }

  std::cout << "All native tests passed\n";
  return 0;
}
