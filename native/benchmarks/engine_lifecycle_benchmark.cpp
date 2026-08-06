#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "xnn_transfer/c_api.h"

namespace {

constexpr std::uint64_t kIterations = 100'000;

bool RunLifecycle() {
  xnn_transfer_engine_config config{
      .struct_size = sizeof(xnn_transfer_engine_config),
      .abi_version = XNN_TRANSFER_ABI_VERSION,
      .reserved = 0,
  };
  xnn_transfer_engine* engine = nullptr;

  if (xnn_transfer_engine_create(&config, &engine) != XNN_TRANSFER_STATUS_OK) {
    return false;
  }
  if (xnn_transfer_engine_start(engine) != XNN_TRANSFER_STATUS_OK) {
    xnn_transfer_engine_destroy(engine);
    return false;
  }

  xnn_transfer_engine_destroy(engine);
  return true;
}

}  // namespace

int main() {
  const auto started_at = std::chrono::steady_clock::now();
  for (std::uint64_t iteration = 0; iteration < kIterations; ++iteration) {
    if (!RunLifecycle()) {
      std::cerr << "Native lifecycle failed at iteration " << iteration << '\n';
      return EXIT_FAILURE;
    }
  }
  const auto elapsed = std::chrono::steady_clock::now() - started_at;
  const double elapsed_seconds = std::chrono::duration<double>(elapsed).count();
  const double operations_per_second =
      static_cast<double>(kIterations) / elapsed_seconds;

  std::cout << "{\n"
            << "  \"benchmark\": \"engine_create_start_destroy\",\n"
            << "  \"iterations\": " << kIterations << ",\n"
            << "  \"elapsed_seconds\": " << elapsed_seconds << ",\n"
            << "  \"operations_per_second\": " << operations_per_second << '\n'
            << "}\n";
  return EXIT_SUCCESS;
}
