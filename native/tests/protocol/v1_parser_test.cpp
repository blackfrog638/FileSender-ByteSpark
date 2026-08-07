#include "xnn_transfer/protocol/v1_parser.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using xnn_transfer::protocol::v1::Direction;
using xnn_transfer::protocol::v1::Error;
using xnn_transfer::protocol::v1::ErrorCode;
using xnn_transfer::protocol::v1::ErrorCodeName;
using xnn_transfer::protocol::v1::kFixedHeaderLength;
using xnn_transfer::protocol::v1::kMaxBodyLength;
using xnn_transfer::protocol::v1::kMaxHeaderLength;
using xnn_transfer::protocol::v1::MessageType;
using xnn_transfer::protocol::v1::ParseFrame;
using xnn_transfer::protocol::v1::ParseFrameBody;
using xnn_transfer::protocol::v1::ParseFrameEnvelope;
using xnn_transfer::protocol::v1::ParseFrameHeader;
using xnn_transfer::protocol::v1::TranscriptParser;
using xnn_transfer::protocol::v1::Version;
using xnn_transfer::protocol::v1::WireType;

using Bytes = std::vector<std::uint8_t>;

struct GoldenFrame {
  std::string_view name;
  Direction direction;
  std::string_view hex;
};

struct GoldenCase {
  std::string_view name;
  std::array<std::string_view, 16> frames;
  std::size_t frame_count;
  bool accepts;
  std::string_view expected_error;
};

#include "v1_golden_vectors.inc"

int failures = 0;

void Expect(const bool condition, const std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

void AppendU16(Bytes& output, const std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value));
}

void AppendU32(Bytes& output, const std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    output.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
  }
}

void AppendU64(Bytes& output, const std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    output.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
  }
}

void SetU16(Bytes& output, const std::size_t offset, const std::uint16_t value) {
  output[offset] = static_cast<std::uint8_t>(value >> 8U);
  output[offset + 1] = static_cast<std::uint8_t>(value);
}

void SetU32(Bytes& output, const std::size_t offset, const std::uint32_t value) {
  output[offset] = static_cast<std::uint8_t>(value >> 24U);
  output[offset + 1] = static_cast<std::uint8_t>(value >> 16U);
  output[offset + 2] = static_cast<std::uint8_t>(value >> 8U);
  output[offset + 3] = static_cast<std::uint8_t>(value);
}

void AppendTlv(Bytes& output, const std::uint16_t id, const WireType wire_type,
               const std::uint8_t flags, const std::span<const std::uint8_t> value) {
  AppendU16(output, id);
  output.push_back(static_cast<std::uint8_t>(wire_type));
  output.push_back(flags);
  AppendU32(output, static_cast<std::uint32_t>(value.size()));
  output.insert(output.end(), value.begin(), value.end());
}

Bytes IntegerValue(const std::uint64_t value, const std::size_t width) {
  Bytes result(width);
  for (std::size_t index = 0; index < width; ++index) {
    const std::size_t shift = (width - index - 1) * 8;
    result[index] = static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift));
  }
  return result;
}

void AppendIntegerTlv(Bytes& output, const std::uint16_t id, const WireType wire_type,
                      const std::uint64_t value, const std::size_t width,
                      const std::uint8_t flags = 0x01U) {
  const Bytes encoded = IntegerValue(value, width);
  AppendTlv(output, id, wire_type, flags, encoded);
}

Bytes MakeFrame(const MessageType message_type, const std::uint32_t stream_id,
                const std::uint64_t message_id, const Bytes& body,
                const Bytes& header_extensions = {},
                const Version version = Version{}) {
  Bytes frame;
  frame.reserve(kFixedHeaderLength + header_extensions.size() + body.size());
  frame.insert(frame.end(), {'X', 'N', 'N', 'T'});
  AppendU16(frame,
            static_cast<std::uint16_t>(kFixedHeaderLength + header_extensions.size()));
  frame.push_back(version.major);
  frame.push_back(version.minor);
  AppendU16(frame, static_cast<std::uint16_t>(message_type));
  AppendU16(frame, 0);
  AppendU32(frame, stream_id);
  AppendU64(frame, message_id);
  AppendU32(frame, static_cast<std::uint32_t>(body.size()));
  frame.insert(frame.end(), header_extensions.begin(), header_extensions.end());
  frame.insert(frame.end(), body.begin(), body.end());
  return frame;
}

Bytes MakePingFrame(const std::uint64_t token = 7, const std::uint64_t message_id = 1) {
  Bytes body;
  AppendIntegerTlv(body, 1, WireType::kU64, token, 8);
  return MakeFrame(MessageType::kPing, 0, message_id, body);
}

Bytes MakeErrorBody(const std::span<const std::uint8_t> detail,
                    const std::uint8_t detail_flags = 0) {
  Bytes body;
  AppendIntegerTlv(body, 1, WireType::kU16, 1, 2);
  AppendIntegerTlv(body, 2, WireType::kBool, 1, 1);
  AppendTlv(body, 6, WireType::kUtf8, detail_flags, detail);
  return body;
}

const GoldenFrame* FindGoldenFrame(const std::string_view name) {
  for (const GoldenFrame& frame : kGoldenFrames) {
    if (frame.name == name) {
      return &frame;
    }
  }
  return nullptr;
}

bool DecodeHex(const std::string_view hex, Bytes& output) {
  if ((hex.size() & 1U) != 0U) {
    return false;
  }
  auto nibble = [](const char value) -> int {
    if (value >= '0' && value <= '9') {
      return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
      return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
      return value - 'A' + 10;
    }
    return -1;
  };

  output.clear();
  output.reserve(hex.size() / 2);
  for (std::size_t offset = 0; offset < hex.size(); offset += 2) {
    const int high = nibble(hex[offset]);
    const int low = nibble(hex[offset + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    output.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return true;
}

void ExpectFrameError(const Bytes& encoded, const ErrorCode expected,
                      const std::string_view context) {
  const auto result = ParseFrame(encoded);
  if (!result.ok() && result.error.code == expected) {
    return;
  }
  std::cerr << "FAILED: " << context << " expected " << ErrorCodeName(expected)
            << ", got " << (result.ok() ? "accept" : ErrorCodeName(result.error.code))
            << '\n';
  ++failures;
}

void TestGoldenVectors() {
  static_assert(kGoldenCases.size() == 29,
                "the native suite must cover all 29 v1 golden cases");
  Expect(kMaximumGoldenCaseFrames <= GoldenCase{}.frames.size(),
         "golden case frame storage is large enough");

  std::size_t executed = 0;
  for (const GoldenCase& golden_case : kGoldenCases) {
    TranscriptParser parser;
    Error actual{};
    bool fixture_valid = true;

    for (std::size_t index = 0; index < golden_case.frame_count; ++index) {
      const GoldenFrame* const frame = FindGoldenFrame(golden_case.frames[index]);
      if (frame == nullptr) {
        std::cerr << "FAILED: " << golden_case.name
                  << " references a missing generated frame\n";
        ++failures;
        fixture_valid = false;
        break;
      }
      Bytes encoded;
      if (!DecodeHex(frame->hex, encoded)) {
        std::cerr << "FAILED: " << golden_case.name
                  << " contains invalid generated hex\n";
        ++failures;
        fixture_valid = false;
        break;
      }
      actual = parser.Process(frame->direction, encoded);
      if (!actual.ok()) {
        break;
      }
    }

    if (!fixture_valid) {
      continue;
    }
    ++executed;
    if (golden_case.accepts) {
      if (!actual.ok()) {
        std::cerr << "FAILED: golden case " << golden_case.name
                  << " expected accept, got " << ErrorCodeName(actual.code) << ": "
                  << actual.detail << '\n';
        ++failures;
      }
    } else if (actual.ok() ||
               ErrorCodeName(actual.code) != golden_case.expected_error) {
      std::cerr << "FAILED: golden case " << golden_case.name << " expected "
                << golden_case.expected_error << ", got "
                << (actual.ok() ? "accept" : ErrorCodeName(actual.code)) << '\n';
      ++failures;
    }
  }

  Expect(executed == kGoldenCases.size(), "every generated golden case executed");
}

void TestHeaderAndLengthFailures() {
  ExpectFrameError({}, ErrorCode::kMalformedFrame, "empty input is a truncated header");

  Bytes frame = MakePingFrame();
  frame.pop_back();
  ExpectFrameError(frame, ErrorCode::kMalformedFrame, "truncated declared body");

  frame = MakePingFrame();
  frame.push_back(0);
  ExpectFrameError(frame, ErrorCode::kMalformedFrame,
                   "trailing bytes after declared frame");

  frame = MakePingFrame();
  SetU16(frame, 4, static_cast<std::uint16_t>(kFixedHeaderLength - 1));
  ExpectFrameError(frame, ErrorCode::kMalformedFrame, "short header length");

  frame = MakePingFrame();
  SetU16(frame, 4, static_cast<std::uint16_t>(kMaxHeaderLength + 1));
  ExpectFrameError(frame, ErrorCode::kFrameTooLarge, "oversized header declaration");

  frame = MakePingFrame();
  SetU32(frame, 24, static_cast<std::uint32_t>(kMaxBodyLength + 1));
  ExpectFrameError(frame, ErrorCode::kFrameTooLarge, "oversized body declaration");

  frame = MakePingFrame();
  SetU32(frame, 24, std::numeric_limits<std::uint32_t>::max());
  ExpectFrameError(frame, ErrorCode::kFrameTooLarge,
                   "maximum body declaration is rejected before arithmetic");

  frame = MakePingFrame();
  frame[7] = 1;
  ExpectFrameError(frame, ErrorCode::kUnsupportedVersion, "unexpected minor version");
}

void TestHeaderPreflight() {
  const Bytes complete = MakePingFrame();
  const Bytes fixed_header(
      complete.begin(),
      complete.begin() + static_cast<std::ptrdiff_t>(kFixedHeaderLength));
  const auto preflight = ParseFrameHeader(fixed_header);
  Expect(preflight.ok(), "fixed-header-only input passes header preflight");
  Expect(preflight.frame.header.body_length == 16 &&
             preflight.frame.declared_total_length == complete.size() &&
             preflight.frame.raw.size() == kFixedHeaderLength &&
             preflight.frame.body.empty(),
         "header preflight returns declared body and total without a body view");

  Bytes oversized = fixed_header;
  SetU32(oversized, 24, static_cast<std::uint32_t>(kMaxBodyLength + 1));
  const auto oversized_result = ParseFrameHeader(oversized);
  Expect(!oversized_result.ok() &&
             oversized_result.error.code == ErrorCode::kFrameTooLarge,
         "oversized body declaration fails preflight without body bytes");

  Bytes extension;
  const std::array<std::uint8_t, 1> extension_value{0x42U};
  AppendTlv(extension, 100, WireType::kBytes, 0, extension_value);
  const Bytes extended = MakeFrame(MessageType::kPing, 0, 1, {}, extension);
  const std::size_t declared_header_length = kFixedHeaderLength + extension.size();
  const Bytes truncated_extension(
      extended.begin(),
      extended.begin() + static_cast<std::ptrdiff_t>(declared_header_length - 1));
  const auto truncated_result = ParseFrameHeader(truncated_extension);
  Expect(!truncated_result.ok() &&
             truncated_result.error.code == ErrorCode::kMalformedFrame,
         "truncated declared header extension fails preflight");

  const Bytes malformed_body{0xffU};
  const Bytes malformed = MakeFrame(MessageType::kTransferOffer, 1, 1, malformed_body);
  const auto body_ignored = ParseFrameHeader(malformed);
  Expect(body_ignored.ok() && body_ignored.frame.body.empty() &&
             body_ignored.frame.raw.size() == kFixedHeaderLength,
         "header preflight ignores supplied body bytes");
  ExpectFrameError(malformed, ErrorCode::kMalformedMessage,
                   "full parsing still rejects body ignored by preflight");
}

void TestHeaderExtensionsAndMaximumBody() {
  Bytes ping_body;
  AppendIntegerTlv(ping_body, 1, WireType::kU64, 7, 8);

  Bytes critical_extension;
  AppendTlv(critical_extension, 100, WireType::kBytes, 0x01U, {});
  ExpectFrameError(MakeFrame(MessageType::kPing, 0, 1, ping_body, critical_extension),
                   ErrorCode::kUnknownCriticalField,
                   "unknown critical header extension");

  Bytes maximum_extension_value(kMaxHeaderLength - kFixedHeaderLength - 8, 0x5aU);
  Bytes maximum_extension;
  AppendTlv(maximum_extension, 100, WireType::kBytes, 0, maximum_extension_value);
  const auto maximum_header =
      ParseFrame(MakeFrame(MessageType::kPing, 0, 1, ping_body, maximum_extension));
  Expect(maximum_header.ok(), "maximum legal header length is accepted");
  Expect(maximum_header.frame.header_fields.count == 0,
         "unknown noncritical header fields are not exposed");

  Bytes maximum_body;
  maximum_body.reserve(kMaxBodyLength);
  AppendIntegerTlv(maximum_body, 1, WireType::kU64, 7, 8);
  Bytes padding(kMaxBodyLength - maximum_body.size() - 8, 0xa5U);
  AppendTlv(maximum_body, 100, WireType::kBytes, 0, padding);
  const auto maximum_body_result =
      ParseFrame(MakeFrame(MessageType::kPing, 0, 1, maximum_body));
  Expect(maximum_body_result.ok(), "maximum legal body length is accepted");
  Expect(maximum_body_result.frame.body_fields.count == 1 &&
             maximum_body_result.frame.body_fields.Count(100) == 0,
         "unknown noncritical body fields are validated but not exposed");
}

void TestTlvHostileInputs() {
  Bytes body;
  AppendIntegerTlv(body, 1, WireType::kU64, 7, 8);
  const std::array<std::uint8_t, 3> future_value{1, 2, 3};
  AppendTlv(body, 100, WireType::kBytes, 0, future_value);
  const auto skipped_unknown = ParseFrame(MakeFrame(MessageType::kPing, 0, 1, body));
  Expect(skipped_unknown.ok() && skipped_unknown.frame.body_fields.count == 1 &&
             skipped_unknown.frame.body_fields.Count(100) == 0,
         "unknown noncritical TLV is skipped from the public collection");

  body.clear();
  AppendIntegerTlv(body, 1, WireType::kU64, 7, 8);
  for (std::size_t index = 0; index < xnn_transfer::protocol::v1::kMaxFields; ++index) {
    AppendTlv(body, 100, WireType::kBytes, 0, {});
  }
  ExpectFrameError(MakeFrame(MessageType::kPing, 0, 1, body), ErrorCode::kLimitExceeded,
                   "257 TLVs");

  body.clear();
  AppendU16(body, 1);
  body.push_back(static_cast<std::uint8_t>(WireType::kU64));
  body.push_back(0x01U);
  AppendU32(body, std::numeric_limits<std::uint32_t>::max());
  ExpectFrameError(MakeFrame(MessageType::kPing, 0, 1, body),
                   ErrorCode::kMalformedMessage,
                   "overflow-sized TLV value declaration");

  body.clear();
  const Bytes seven_bytes(7, 0);
  AppendTlv(body, 1, WireType::kU64, 0x01U, seven_bytes);
  ExpectFrameError(MakeFrame(MessageType::kPing, 0, 1, body),
                   ErrorCode::kMalformedMessage,
                   "fixed-width integer has wrong length");

  body.clear();
  AppendU16(body, 1);
  body.push_back(0xffU);
  body.push_back(0x01U);
  AppendU32(body, 0);
  ExpectFrameError(MakeFrame(MessageType::kPing, 0, 1, body),
                   ErrorCode::kMalformedMessage, "reserved wire type");

  body.clear();
  AppendIntegerTlv(body, 1, WireType::kU64, 7, 8, 0x03U);
  ExpectFrameError(MakeFrame(MessageType::kPing, 0, 1, body),
                   ErrorCode::kMalformedMessage, "reserved field flag");

  body.clear();
  AppendIntegerTlv(body, 1, WireType::kU64, 7, 8);
  AppendTlv(body, 100, WireType::kBytes, 0x01U, {});
  ExpectFrameError(MakeFrame(MessageType::kPing, 0, 1, body),
                   ErrorCode::kUnknownCriticalField, "unknown critical body field");

  body.clear();
  AppendTlv(body, 2, WireType::kBytes, 0, {});
  AppendIntegerTlv(body, 1, WireType::kU64, 7, 8);
  ExpectFrameError(MakeFrame(MessageType::kPing, 0, 1, body),
                   ErrorCode::kMalformedMessage, "out-of-order TLVs");

  body.clear();
  AppendIntegerTlv(body, 1, WireType::kU64, 7, 8);
  AppendIntegerTlv(body, 1, WireType::kU64, 8, 8);
  ExpectFrameError(MakeFrame(MessageType::kPing, 0, 1, body),
                   ErrorCode::kMalformedMessage, "duplicate scalar TLV");

  body.clear();
  AppendIntegerTlv(body, 1, WireType::kU16, 1, 2);
  AppendIntegerTlv(body, 2, WireType::kBool, 2, 1);
  ExpectFrameError(MakeFrame(MessageType::kError, 0, 1, body),
                   ErrorCode::kMalformedMessage, "noncanonical Boolean");

  const std::array<std::uint8_t, 1> detail{'x'};
  ExpectFrameError(MakeFrame(MessageType::kError, 0, 1, MakeErrorBody(detail, 0x01U)),
                   ErrorCode::kMalformedMessage,
                   "optional known field marked critical");
}

void TestUtf8HostileInputs() {
  const std::array invalid_values{
      Bytes{0x00U},
      Bytes{0xc0U, 0x80U},
      Bytes{0xedU, 0xa0U, 0x80U},
      Bytes{0xefU, 0xb7U, 0x90U},
      Bytes{0xf4U, 0x8fU, 0xbfU, 0xbfU},
      Bytes{0xe2U, 0x82U},
  };
  for (std::size_t index = 0; index < invalid_values.size(); ++index) {
    const Bytes frame =
        MakeFrame(MessageType::kError, 0, 1, MakeErrorBody(invalid_values[index]));
    const auto result = ParseFrame(frame);
    if (result.ok() || result.error.code != ErrorCode::kMalformedMessage) {
      std::cerr << "FAILED: invalid UTF-8 case " << index << " was "
                << (result.ok() ? "accepted"
                                : std::string(ErrorCodeName(result.error.code)))
                << '\n';
      ++failures;
    }
  }

  const std::array<std::uint8_t, 4> valid_maximum{0xf4U, 0x8fU, 0xbfU, 0xbdU};
  const auto valid_result =
      ParseFrame(MakeFrame(MessageType::kError, 0, 1, MakeErrorBody(valid_maximum)));
  Expect(valid_result.ok(), "valid shortest-form UTF-8 is accepted");
}

void TestScopeAndSchemaOrdering() {
  Bytes body;
  AppendIntegerTlv(body, 1, WireType::kU64, 7, 8);
  ExpectFrameError(MakeFrame(MessageType::kPing, 3, 1, body),
                   ErrorCode::kStateViolation, "connection message on transfer stream");

  ExpectFrameError(MakeFrame(MessageType::kTransferOffer, 0, 1, {}),
                   ErrorCode::kStateViolation,
                   "transfer scope is checked before required body fields");

  ExpectFrameError(MakeFrame(MessageType::kTransferOffer, 1, 1, {}),
                   ErrorCode::kMalformedMessage, "missing required transfer fields");
}

void TestPreBindingBodyGate() {
  const Bytes malformed_body{0xffU};
  const Bytes transfer = MakeFrame(MessageType::kTransferOffer, 1, 1, malformed_body);

  auto envelope = ParseFrameEnvelope(transfer);
  Expect(envelope.ok() && envelope.frame.body_fields.count == 0,
         "transfer envelope is valid without parsing its malformed body");
  if (envelope.ok()) {
    const Error body_error = ParseFrameBody(envelope.frame);
    Expect(body_error.code == ErrorCode::kMalformedMessage,
           "explicit body parsing detects malformed transfer TLV");
  }
  ExpectFrameError(transfer, ErrorCode::kMalformedMessage,
                   "full frame parsing still validates transfer body");

  TranscriptParser parser;
  const Error transcript_error =
      parser.Process(Direction::kInitiatorToResponder, transfer);
  Expect(transcript_error.code == ErrorCode::kStateViolation,
         "pre-binding transfer is rejected before malformed body parsing");
}

void TestNegotiatedBodyLimitPrecedesBodyParsing() {
  constexpr std::array<std::string_view, 6> kBindingFrames{
      "hello_initiator",
      "hello_responder",
      "negotiate",
      "negotiate_ack",
      "transport_finished_initiator",
      "transport_finished_responder",
  };
  TranscriptParser parser;
  for (const std::string_view name : kBindingFrames) {
    const GoldenFrame* const frame = FindGoldenFrame(name);
    Bytes encoded;
    if (frame == nullptr || !DecodeHex(frame->hex, encoded)) {
      Expect(false, "negotiated limit test fixture is valid");
      return;
    }
    const Error error = parser.Process(frame->direction, encoded);
    if (!error.ok()) {
      Expect(false, "negotiated limit test reaches the established state");
      return;
    }
  }

  constexpr std::size_t kNegotiatedMaxBody = 524'288;
  const Bytes malformed_oversized_body(kNegotiatedMaxBody + 1, 0xffU);
  const Bytes frame = MakeFrame(MessageType::kPing, 0, 4, malformed_oversized_body);
  const Error error = parser.Process(Direction::kInitiatorToResponder, frame);
  Expect(error.code == ErrorCode::kLimitExceeded,
         "negotiated body limit is enforced before malformed body parsing");
}

}  // namespace

int main() {
  TestGoldenVectors();
  TestHeaderAndLengthFailures();
  TestHeaderPreflight();
  TestHeaderExtensionsAndMaximumBody();
  TestTlvHostileInputs();
  TestUtf8HostileInputs();
  TestScopeAndSchemaOrdering();
  TestPreBindingBodyGate();
  TestNegotiatedBodyLimitPrecedesBodyParsing();

  if (failures != 0) {
    std::cerr << failures << " protocol parser test(s) failed\n";
    return 1;
  }

  std::cout << "Passed all " << kGoldenCases.size()
            << " golden transcript cases and hostile parser tests"
            << " (manifest " << kGoldenManifestSha256 << ")\n";
  return 0;
}
