#include <cstddef>
#include <cstdint>
#include <span>

#include "protocol/v1_parser.hpp"

namespace {

using xnn_transfer::protocol::v1::Direction;
using xnn_transfer::protocol::v1::kMaxEncodedFrameLength;
using xnn_transfer::protocol::v1::ParseFrame;
using xnn_transfer::protocol::v1::TranscriptParser;

constexpr std::size_t kMaxFuzzInput = (2 * kMaxEncodedFrameLength) + 128;
constexpr std::size_t kMaxTranscriptFrames = 16;

std::uint32_t ReadU32(const std::span<const std::uint8_t> bytes) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value = static_cast<std::uint32_t>((value << 8U) |
                                       static_cast<std::uint32_t>(bytes[index]));
  }
  return value;
}

void FuzzTranscript(const std::span<const std::uint8_t> input) {
  TranscriptParser parser;
  std::size_t offset = 0;
  std::size_t frame_count = 0;

  // Records are direction:U8 || frame_length:U32 || complete_frame.
  while (offset < input.size() && frame_count < kMaxTranscriptFrames) {
    if (input.size() - offset < 5) {
      return;
    }
    const Direction direction = (input[offset] & 1U) == 0U
                                    ? Direction::kInitiatorToResponder
                                    : Direction::kResponderToInitiator;
    ++offset;
    const std::uint32_t declared_length = ReadU32(input.subspan(offset, 4));
    offset += 4;
    if (declared_length > kMaxEncodedFrameLength ||
        declared_length > input.size() - offset) {
      return;
    }

    const std::size_t frame_length = static_cast<std::size_t>(declared_length);
    const auto frame = input.subspan(offset, frame_length);
    offset += frame_length;
    ++frame_count;
    if (!parser.Process(direction, frame).ok()) {
      return;
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data,
                                      const std::size_t size) {
  if (size == 0 || size > kMaxFuzzInput) {
    return 0;
  }

  const std::span<const std::uint8_t> input(data, size);
  switch (input[0] % 3U) {
    case 0:
      if (input.size() - 1 <= kMaxEncodedFrameLength) {
        static_cast<void>(ParseFrame(input.subspan(1)));
      }
      break;
    case 1:
      FuzzTranscript(input.subspan(1));
      break;
    case 2: {
      if (input.size() - 1 > kMaxEncodedFrameLength) {
        break;
      }
      TranscriptParser parser;
      const Direction direction = (input[0] & 1U) == 0U
                                      ? Direction::kInitiatorToResponder
                                      : Direction::kResponderToInitiator;
      static_cast<void>(parser.Process(direction, input.subspan(1)));
      break;
    }
  }
  return 0;
}
