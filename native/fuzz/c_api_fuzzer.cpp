#include <cstddef>
#include <cstdint>

#include "xnn_transfer/c_api.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      const std::size_t size) {
  if (size < 2) {
    return 0;
  }

  xnn_transfer_engine_config config{
      .struct_size = static_cast<std::size_t>(data[0]),
      .abi_version = static_cast<std::uint32_t>(data[1]),
      .reserved = 0,
  };
  if ((data[0] & 1U) != 0U) {
    config.struct_size = sizeof(xnn_transfer_engine_config);
  }
  if ((data[1] & 1U) != 0U) {
    config.abi_version = XNN_TRANSFER_ABI_VERSION;
  }

  xnn_transfer_engine* engine = nullptr;
  const xnn_transfer_status create_status =
      xnn_transfer_engine_create(&config, &engine);
  if (create_status != XNN_TRANSFER_STATUS_OK || engine == nullptr) {
    return 0;
  }

  for (std::size_t index = 2; index < size; ++index) {
    switch (data[index] % 3U) {
      case 0:
        static_cast<void>(xnn_transfer_engine_start(engine));
        break;
      case 1:
        static_cast<void>(xnn_transfer_engine_stop(engine));
        break;
      case 2: {
        xnn_transfer_engine_state state = XNN_TRANSFER_ENGINE_STATE_CREATED;
        static_cast<void>(xnn_transfer_engine_get_state(engine, &state));
        break;
      }
    }
  }

  xnn_transfer_engine_destroy(engine);
  return 0;
}
