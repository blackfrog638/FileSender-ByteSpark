#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "internal.hpp"

namespace {

namespace protocol = xnn_transfer::protocol::v1;
namespace transfer = xnn_transfer::core::transfer;
namespace internal = xnn_transfer::core::transfer::internal;

constexpr std::uint64_t kIterations = 256;
constexpr std::size_t kChunkBytes = transfer::kMaximumChunkSize;

}  // namespace

int main() {
  transfer::Bytes chunk(kChunkBytes, 0xa5);
  transfer::Bytes commitment(32, 0x5a);
  transfer::TransferId transfer_id{};
  transfer_id.front() = 1;
  transfer::TransferContext context{
      .stream_id = 1,
  };
  transfer::ConnectionMessageSequence message_ids;
  std::size_t maximum_frame_bytes = 0;

  const auto started_at = std::chrono::steady_clock::now();
  for (std::uint64_t iteration = 0; iteration < kIterations; ++iteration) {
    transfer::Bytes encoded;
    if (!internal::EncodeFileChunkFrame(context, message_ids, transfer_id,
                                        iteration * kChunkBytes, chunk, commitment,
                                        encoded)) {
      std::cerr << "Transfer frame benchmark failed at iteration " << iteration << '\n';
      return EXIT_FAILURE;
    }
    const protocol::ParseResult parsed = protocol::ParseFrame(encoded);
    if (!parsed.ok() ||
        parsed.frame.header.message_type != protocol::MessageType::kFileChunk) {
      std::cerr << "Transfer frame benchmark parse failed at iteration " << iteration
                << '\n';
      return EXIT_FAILURE;
    }
    maximum_frame_bytes = std::max(maximum_frame_bytes, encoded.size());
  }
  const double elapsed_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at)
          .count();
  const double payload_mebibytes =
      static_cast<double>(kIterations * kChunkBytes) / (1024.0 * 1024.0);

  std::cout << "{\n"
            << "  \"benchmark\": \"transfer_chunk_encode_parse\",\n"
            << "  \"iterations\": " << kIterations << ",\n"
            << "  \"chunk_bytes\": " << kChunkBytes << ",\n"
            << "  \"maximum_frame_bytes\": " << maximum_frame_bytes << ",\n"
            << "  \"elapsed_seconds\": " << elapsed_seconds << ",\n"
            << "  \"payload_mebibytes_per_second\": "
            << payload_mebibytes / elapsed_seconds << '\n'
            << "}\n";
  return EXIT_SUCCESS;
}
