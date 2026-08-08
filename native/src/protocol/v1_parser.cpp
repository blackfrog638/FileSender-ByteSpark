#include "xnn_transfer/protocol/v1_parser.hpp"

#include <algorithm>
#include <limits>

namespace xnn_transfer::protocol::v1 {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'X', 'N', 'N', 'T'};
constexpr std::uint32_t kBaseTransferV1 = 0x00010001;
constexpr std::uint32_t kMaxInFlight = 67'108'864;
constexpr std::uint16_t kMaxStreams = 32;
constexpr std::uint32_t kMaxChunkData = 1'048'000;

enum class Presence {
  kRequired,
  kOptional,
  kConditional,
};

struct FieldRule {
  std::uint16_t id;
  WireType wire_type;
  Presence presence;
  bool repeated;
  std::uint32_t minimum_length;
  std::uint32_t maximum_length;
};

constexpr FieldRule Required(
    const std::uint16_t id, const WireType wire_type, const bool repeated = false,
    const std::uint32_t minimum_length = 0,
    const std::uint32_t maximum_length = std::numeric_limits<std::uint32_t>::max()) {
  return FieldRule{id,       wire_type,      Presence::kRequired,
                   repeated, minimum_length, maximum_length};
}

constexpr FieldRule Optional(
    const std::uint16_t id, const WireType wire_type,
    const std::uint32_t minimum_length = 0,
    const std::uint32_t maximum_length = std::numeric_limits<std::uint32_t>::max()) {
  return FieldRule{id,    wire_type,      Presence::kOptional,
                   false, minimum_length, maximum_length};
}

constexpr FieldRule Conditional(
    const std::uint16_t id, const WireType wire_type,
    const std::uint32_t minimum_length = 0,
    const std::uint32_t maximum_length = std::numeric_limits<std::uint32_t>::max()) {
  return FieldRule{id,    wire_type,      Presence::kConditional,
                   false, minimum_length, maximum_length};
}

constexpr FieldRule kHelloSchema[] = {
    Required(1, WireType::kU16),       Required(2, WireType::kU16),
    Required(3, WireType::kU16),       Required(4, WireType::kU16),
    Required(5, WireType::kU8),        Required(6, WireType::kU32, true),
    Required(7, WireType::kU32, true), Required(8, WireType::kU32),
    Required(9, WireType::kU32),       Required(10, WireType::kU16),
};

constexpr FieldRule kNegotiateSchema[] = {
    Required(1, WireType::kU16),       Required(2, WireType::kU16),
    Required(3, WireType::kU32, true), Required(4, WireType::kU32),
    Required(5, WireType::kU32),       Required(6, WireType::kU16),
};

constexpr FieldRule kErrorSchema[] = {
    Required(1, WireType::kU16),  Required(2, WireType::kBool),
    Optional(3, WireType::kBool), Optional(4, WireType::kU64),
    Optional(5, WireType::kU32),  Optional(6, WireType::kUtf8, 0, 256),
};

constexpr FieldRule kPingPongSchema[] = {
    Required(1, WireType::kU64),
};

constexpr FieldRule kGoAwaySchema[] = {
    Required(1, WireType::kU16),
    Optional(2, WireType::kU32),
};

constexpr FieldRule kTransportFinishedSchema[] = {
    Required(1, WireType::kU8),
    Required(2, WireType::kBytes, false, 1, 64),
};

constexpr FieldRule kTransferOfferSchema[] = {
    Required(1, WireType::kBytes, false, 16, 16),
    Required(2, WireType::kU32),
    Required(3, WireType::kU64),
    Required(4, WireType::kBytes, false, 16, 64),
    Optional(5, WireType::kUtf8, 1, 255),
};

constexpr FieldRule kManifestEntrySchema[] = {
    Required(1, WireType::kBytes, false, 16, 16),
    Required(2, WireType::kU32),
    Required(3, WireType::kU8),
    Required(4, WireType::kUtf8, false, 1, 1'024),
    Required(5, WireType::kU64),
    Conditional(6, WireType::kBytes, 16, 64),
    Optional(7, WireType::kU64),
};

constexpr FieldRule kManifestEndSchema[] = {
    Required(1, WireType::kBytes, false, 16, 16),
    Required(2, WireType::kU32),
    Required(3, WireType::kU64),
    Required(4, WireType::kBytes, false, 16, 64),
};

constexpr FieldRule kTransferAcceptSchema[] = {
    Required(1, WireType::kBytes, false, 16, 16),
    Required(2, WireType::kU32),
    Required(3, WireType::kU32),
    Conditional(4, WireType::kBytes, 16, 256),
    Conditional(5, WireType::kU32),
};

constexpr FieldRule kTransferRejectSchema[] = {
    Required(1, WireType::kBytes, false, 16, 16),
    Required(2, WireType::kU16),
    Required(3, WireType::kBool),
    Optional(4, WireType::kU32),
    Optional(5, WireType::kUtf8, 0, 256),
};

constexpr FieldRule kFileBeginSchema[] = {
    Required(1, WireType::kBytes, false, 16, 16),
    Required(2, WireType::kU32),
    Required(3, WireType::kU64),
    Required(4, WireType::kU32),
};

constexpr FieldRule kFileChunkSchema[] = {
    Required(1, WireType::kBytes, false, 16, 16),
    Required(2, WireType::kU32),
    Required(3, WireType::kU64),
    Required(4, WireType::kBytes, false, 1, kMaxChunkData),
    Required(5, WireType::kBytes, false, 16, 64),
};

constexpr FieldRule kChunkAckSchema[] = {
    Required(1, WireType::kBytes, false, 16, 16),
    Required(2, WireType::kU32),
    Required(3, WireType::kU64),
    Required(4, WireType::kU32),
};

constexpr FieldRule kFileEndSchema[] = {
    Required(1, WireType::kBytes, false, 16, 16),
    Required(2, WireType::kU32),
    Required(3, WireType::kU64),
    Required(4, WireType::kBytes, false, 16, 64),
};

constexpr FieldRule kFileCommitSchema[] = {
    Required(1, WireType::kBytes, false, 16, 16),
    Required(2, WireType::kU32),
    Required(3, WireType::kU64),
};

constexpr FieldRule kTransferCompleteSchema[] = {
    Required(1, WireType::kBytes, false, 16, 16),
    Required(2, WireType::kBytes, false, 16, 64),
};

constexpr FieldRule kCancelSchema[] = {
    Required(1, WireType::kBytes, false, 16, 16),
    Required(2, WireType::kU16),
    Optional(3, WireType::kUtf8, 0, 256),
};

constexpr FieldRule kCancelAckSchema[] = {
    Required(1, WireType::kBytes, false, 16, 16),
    Required(2, WireType::kU16),
};

constexpr FieldRule kResumeRequestSchema[] = {
    Required(1, WireType::kBytes, false, 16, 16),
    Required(2, WireType::kBytes, false, 16, 64),
    Required(3, WireType::kBytes, false, 16, 256),
};

constexpr FieldRule kResumeStateSchema[] = {
    Required(1, WireType::kBytes, false, 16, 16),
    Required(2, WireType::kU32),
    Required(3, WireType::kU64),
    Required(4, WireType::kBool),
    Conditional(5, WireType::kBytes, 16, 64),
};

constexpr FieldRule kResumeEndSchema[] = {
    Required(1, WireType::kBytes, false, 16, 16),
    Required(2, WireType::kU32),
    Required(3, WireType::kU16),
};

[[nodiscard]] constexpr Error MakeError(const ErrorCode code,
                                        const std::string_view detail) {
  return Error{code, detail};
}

[[nodiscard]] constexpr bool IsKnownWireType(const std::uint8_t value) {
  return value >= static_cast<std::uint8_t>(WireType::kU8) &&
         value <= static_cast<std::uint8_t>(WireType::kBool);
}

[[nodiscard]] constexpr std::size_t FixedWireLength(const WireType wire_type) {
  switch (wire_type) {
    case WireType::kU8:
    case WireType::kBool:
      return 1;
    case WireType::kU16:
      return 2;
    case WireType::kU32:
      return 4;
    case WireType::kU64:
      return 8;
    case WireType::kBytes:
    case WireType::kUtf8:
      return 0;
  }
  return 0;
}

[[nodiscard]] std::uint16_t ReadU16(
    const std::span<const std::uint8_t> bytes) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) |
                                    static_cast<std::uint16_t>(bytes[1]));
}

[[nodiscard]] std::uint32_t ReadU32(
    const std::span<const std::uint8_t> bytes) noexcept {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value = static_cast<std::uint32_t>((value << 8U) |
                                       static_cast<std::uint32_t>(bytes[index]));
  }
  return value;
}

[[nodiscard]] std::uint64_t ReadU64(
    const std::span<const std::uint8_t> bytes) noexcept {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value = (value << 8U) | static_cast<std::uint64_t>(bytes[index]);
  }
  return value;
}

[[nodiscard]] constexpr bool IsContinuation(const std::uint8_t value) {
  return (value & 0xc0U) == 0x80U;
}

[[nodiscard]] bool IsUnicodeNoncharacter(const std::uint32_t code_point) {
  return (code_point >= 0xfdd0U && code_point <= 0xfdefU) ||
         (code_point <= 0x10ffffU && (code_point & 0xffffU) >= 0xfffeU);
}

[[nodiscard]] bool IsValidUtf8(const std::span<const std::uint8_t> value) noexcept {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const std::uint8_t first = value[offset];
    std::uint32_t code_point = 0;
    std::size_t width = 0;
    if (first <= 0x7fU) {
      code_point = first;
      width = 1;
    } else if (first >= 0xc2U && first <= 0xdfU) {
      width = 2;
      if (value.size() - offset < width || !IsContinuation(value[offset + 1])) {
        return false;
      }
      code_point = (static_cast<std::uint32_t>(first & 0x1fU) << 6U) |
                   static_cast<std::uint32_t>(value[offset + 1] & 0x3fU);
    } else if (first >= 0xe0U && first <= 0xefU) {
      width = 3;
      if (value.size() - offset < width || !IsContinuation(value[offset + 1]) ||
          !IsContinuation(value[offset + 2])) {
        return false;
      }
      const std::uint8_t second = value[offset + 1];
      if ((first == 0xe0U && second < 0xa0U) || (first == 0xedU && second >= 0xa0U)) {
        return false;
      }
      code_point = (static_cast<std::uint32_t>(first & 0x0fU) << 12U) |
                   (static_cast<std::uint32_t>(second & 0x3fU) << 6U) |
                   static_cast<std::uint32_t>(value[offset + 2] & 0x3fU);
    } else if (first >= 0xf0U && first <= 0xf4U) {
      width = 4;
      if (value.size() - offset < width || !IsContinuation(value[offset + 1]) ||
          !IsContinuation(value[offset + 2]) || !IsContinuation(value[offset + 3])) {
        return false;
      }
      const std::uint8_t second = value[offset + 1];
      if ((first == 0xf0U && second < 0x90U) || (first == 0xf4U && second > 0x8fU)) {
        return false;
      }
      code_point = (static_cast<std::uint32_t>(first & 0x07U) << 18U) |
                   (static_cast<std::uint32_t>(second & 0x3fU) << 12U) |
                   (static_cast<std::uint32_t>(value[offset + 2] & 0x3fU) << 6U) |
                   static_cast<std::uint32_t>(value[offset + 3] & 0x3fU);
    } else {
      return false;
    }

    if (code_point == 0U || IsUnicodeNoncharacter(code_point)) {
      return false;
    }
    offset += width;
  }
  return true;
}

[[nodiscard]] constexpr bool IsKnownMessageType(const std::uint16_t value) {
  switch (static_cast<MessageType>(value)) {
    case MessageType::kHello:
    case MessageType::kNegotiate:
    case MessageType::kNegotiateAck:
    case MessageType::kError:
    case MessageType::kPing:
    case MessageType::kPong:
    case MessageType::kGoAway:
    case MessageType::kTransportFinished:
    case MessageType::kTransferOffer:
    case MessageType::kManifestEntry:
    case MessageType::kManifestEnd:
    case MessageType::kTransferAccept:
    case MessageType::kTransferReject:
    case MessageType::kFileBegin:
    case MessageType::kFileChunk:
    case MessageType::kChunkAck:
    case MessageType::kFileEnd:
    case MessageType::kFileCommit:
    case MessageType::kTransferComplete:
    case MessageType::kTransferCompleteAck:
    case MessageType::kCancel:
    case MessageType::kCancelAck:
    case MessageType::kResumeRequest:
    case MessageType::kResumeState:
    case MessageType::kResumeEnd:
      return true;
  }
  return false;
}

[[nodiscard]] constexpr bool IsConnectionMessage(const MessageType type) {
  switch (type) {
    case MessageType::kHello:
    case MessageType::kNegotiate:
    case MessageType::kNegotiateAck:
    case MessageType::kPing:
    case MessageType::kPong:
    case MessageType::kGoAway:
    case MessageType::kTransportFinished:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] constexpr bool IsTransferMessage(const MessageType type) {
  return static_cast<std::uint16_t>(type) >=
         static_cast<std::uint16_t>(MessageType::kTransferOffer);
}

[[nodiscard]] std::span<const FieldRule> SchemaFor(const MessageType type) noexcept {
  switch (type) {
    case MessageType::kHello:
      return kHelloSchema;
    case MessageType::kNegotiate:
    case MessageType::kNegotiateAck:
      return kNegotiateSchema;
    case MessageType::kError:
      return kErrorSchema;
    case MessageType::kPing:
    case MessageType::kPong:
      return kPingPongSchema;
    case MessageType::kGoAway:
      return kGoAwaySchema;
    case MessageType::kTransportFinished:
      return kTransportFinishedSchema;
    case MessageType::kTransferOffer:
      return kTransferOfferSchema;
    case MessageType::kManifestEntry:
      return kManifestEntrySchema;
    case MessageType::kManifestEnd:
      return kManifestEndSchema;
    case MessageType::kTransferAccept:
      return kTransferAcceptSchema;
    case MessageType::kTransferReject:
      return kTransferRejectSchema;
    case MessageType::kFileBegin:
      return kFileBeginSchema;
    case MessageType::kFileChunk:
      return kFileChunkSchema;
    case MessageType::kChunkAck:
      return kChunkAckSchema;
    case MessageType::kFileEnd:
      return kFileEndSchema;
    case MessageType::kFileCommit:
      return kFileCommitSchema;
    case MessageType::kTransferComplete:
    case MessageType::kTransferCompleteAck:
      return kTransferCompleteSchema;
    case MessageType::kCancel:
      return kCancelSchema;
    case MessageType::kCancelAck:
      return kCancelAckSchema;
    case MessageType::kResumeRequest:
      return kResumeRequestSchema;
    case MessageType::kResumeState:
      return kResumeStateSchema;
    case MessageType::kResumeEnd:
      return kResumeEndSchema;
  }
  return {};
}

[[nodiscard]] const FieldRule* FindRule(const std::span<const FieldRule> schema,
                                        const std::uint16_t id) noexcept {
  for (const FieldRule& rule : schema) {
    if (rule.id == id) {
      return &rule;
    }
  }
  return nullptr;
}

[[nodiscard]] Error ValidateWireValue(
    const WireType wire_type, const std::span<const std::uint8_t> value) noexcept {
  const std::size_t fixed_length = FixedWireLength(wire_type);
  if (fixed_length != 0 && value.size() != fixed_length) {
    return MakeError(ErrorCode::kMalformedMessage,
                     "integer or Boolean TLV has the wrong length");
  }
  if (wire_type == WireType::kBool && (value[0] != 0U && value[0] != 1U)) {
    return MakeError(ErrorCode::kMalformedMessage, "Boolean TLV is not canonical");
  }
  if (wire_type == WireType::kUtf8 && !IsValidUtf8(value)) {
    return MakeError(ErrorCode::kMalformedMessage, "UTF-8 TLV is not well formed");
  }
  return {};
}

[[nodiscard]] Error ParseTlvs(const std::span<const std::uint8_t> data,
                              const std::span<const FieldRule> schema,
                              FieldCollection& output) noexcept {
  std::size_t offset = 0;
  std::size_t field_count = 0;
  bool has_previous_id = false;
  std::uint16_t previous_id = 0;

  while (offset < data.size()) {
    if (field_count == kMaxFields) {
      return MakeError(ErrorCode::kLimitExceeded,
                       "TLV field count exceeds the hard limit");
    }
    ++field_count;
    if (data.size() - offset < 8) {
      return MakeError(ErrorCode::kMalformedMessage, "trailing partial TLV header");
    }

    const std::span<const std::uint8_t> header = data.subspan(offset, 8);
    const std::uint16_t id = ReadU16(header);
    const std::uint8_t wire_value = header[2];
    const std::uint8_t flags = header[3];
    const std::uint32_t declared_length = ReadU32(header.subspan(4, 4));
    offset += 8;

    if (has_previous_id && id < previous_id) {
      return MakeError(ErrorCode::kMalformedMessage,
                       "TLV fields are not in canonical order");
    }
    if (!IsKnownWireType(wire_value)) {
      return MakeError(ErrorCode::kMalformedMessage, "TLV uses a reserved wire type");
    }
    if ((flags & 0xfeU) != 0U) {
      return MakeError(ErrorCode::kMalformedMessage, "TLV uses a reserved field flag");
    }
    if (declared_length > data.size() - offset) {
      return MakeError(ErrorCode::kMalformedMessage, "TLV value is truncated");
    }

    const std::size_t value_length = static_cast<std::size_t>(declared_length);
    const std::span<const std::uint8_t> value = data.subspan(offset, value_length);
    offset += value_length;
    const WireType wire_type = static_cast<WireType>(wire_value);
    const FieldRule* const rule = FindRule(schema, id);

    if (rule == nullptr) {
      if ((flags & 0x01U) != 0U) {
        return MakeError(ErrorCode::kUnknownCriticalField, "unknown critical TLV");
      }
    } else {
      const bool critical = (flags & 0x01U) != 0U;
      if (rule->presence == Presence::kOptional && critical) {
        return MakeError(ErrorCode::kMalformedMessage,
                         "optional TLV is marked critical");
      }
      if (rule->presence != Presence::kOptional && !critical) {
        return MakeError(ErrorCode::kMalformedMessage,
                         "required TLV is not marked critical");
      }
      if (wire_type != rule->wire_type) {
        return MakeError(ErrorCode::kMalformedMessage, "TLV has the wrong wire type");
      }
      if (declared_length < rule->minimum_length ||
          declared_length > rule->maximum_length) {
        return MakeError(ErrorCode::kMalformedMessage,
                         "TLV length violates its schema");
      }
      if (!rule->repeated && output.Count(id) != 0) {
        return MakeError(ErrorCode::kMalformedMessage, "scalar TLV is duplicated");
      }
    }

    const Error value_error = ValidateWireValue(wire_type, value);
    if (!value_error.ok()) {
      return value_error;
    }

    if (rule != nullptr) {
      output.fields[output.count] = FieldView{id, wire_type, flags, value};
      ++output.count;
    }
    previous_id = id;
    has_previous_id = true;
  }

  for (const FieldRule& rule : schema) {
    if (rule.presence == Presence::kRequired && output.Count(rule.id) == 0) {
      return MakeError(ErrorCode::kMalformedMessage, "required TLV is absent");
    }
  }
  return {};
}

[[nodiscard]] bool DirectionIndex(const Direction direction,
                                  std::size_t& index) noexcept {
  switch (direction) {
    case Direction::kInitiatorToResponder:
      index = 0;
      return true;
    case Direction::kResponderToInitiator:
      index = 1;
      return true;
  }
  return false;
}

[[nodiscard]] constexpr bool VersionLess(const Version left, const Version right) {
  return left.major < right.major ||
         (left.major == right.major && left.minor < right.minor);
}

[[nodiscard]] constexpr Version VersionMaximum(const Version left,
                                               const Version right) {
  return VersionLess(left, right) ? right : left;
}

[[nodiscard]] constexpr Version VersionMinimum(const Version left,
                                               const Version right) {
  return VersionLess(left, right) ? left : right;
}

template <std::size_t Size>
[[nodiscard]] bool Contains(const std::array<std::uint32_t, Size>& values,
                            const std::size_t count,
                            const std::uint32_t wanted) noexcept {
  return std::binary_search(
      values.begin(), values.begin() + static_cast<std::ptrdiff_t>(count), wanted);
}

[[nodiscard]] bool EqualCapabilities(const std::array<std::uint32_t, kMaxFields>& left,
                                     const std::size_t left_count,
                                     const std::array<std::uint32_t, kMaxFields>& right,
                                     const std::size_t right_count) noexcept {
  return left_count == right_count &&
         std::equal(left.begin(),
                    left.begin() + static_cast<std::ptrdiff_t>(left_count),
                    right.begin());
}

}  // namespace

std::string_view ErrorCodeName(const ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kNone:
      return "NONE";
    case ErrorCode::kMalformedFrame:
      return "MALFORMED_FRAME";
    case ErrorCode::kFrameTooLarge:
      return "FRAME_TOO_LARGE";
    case ErrorCode::kMalformedMessage:
      return "MALFORMED_MESSAGE";
    case ErrorCode::kUnsupportedVersion:
      return "UNSUPPORTED_VERSION";
    case ErrorCode::kDowngradeDetected:
      return "DOWNGRADE_DETECTED";
    case ErrorCode::kUnsupportedCapability:
      return "UNSUPPORTED_CAPABILITY";
    case ErrorCode::kUnsupportedMessage:
      return "UNSUPPORTED_MESSAGE";
    case ErrorCode::kUnknownCriticalField:
      return "UNKNOWN_CRITICAL_FIELD";
    case ErrorCode::kStateViolation:
      return "STATE_VIOLATION";
    case ErrorCode::kMessageIdViolation:
      return "MESSAGE_ID_VIOLATION";
    case ErrorCode::kLimitExceeded:
      return "LIMIT_EXCEEDED";
  }
  return "UNKNOWN_ERROR";
}

const FieldView* FieldCollection::FindFirst(const std::uint16_t id) const noexcept {
  for (std::size_t index = 0; index < count; ++index) {
    if (fields[index].id == id) {
      return &fields[index];
    }
  }
  return nullptr;
}

std::size_t FieldCollection::Count(const std::uint16_t id) const noexcept {
  std::size_t result = 0;
  for (std::size_t index = 0; index < count; ++index) {
    if (fields[index].id == id) {
      ++result;
    }
  }
  return result;
}

std::uint64_t DecodeUnsigned(const FieldView& field) noexcept {
  std::uint64_t result = 0;
  for (const std::uint8_t byte : field.value) {
    result = (result << 8U) | static_cast<std::uint64_t>(byte);
  }
  return result;
}

ParseResult ParseFrameHeader(const std::span<const std::uint8_t> encoded,
                             const Version expected_version) noexcept {
  ParseResult result{};
  if (encoded.size() < kFixedHeaderLength) {
    result.error = MakeError(ErrorCode::kMalformedFrame, "fixed header is truncated");
    return result;
  }
  if (!std::equal(kMagic.begin(), kMagic.end(), encoded.begin())) {
    result.error = MakeError(ErrorCode::kMalformedFrame, "invalid frame magic");
    return result;
  }

  const std::uint16_t header_length = ReadU16(encoded.subspan(4, 2));
  const Version version{encoded[6], encoded[7]};
  const std::uint16_t raw_message_type = ReadU16(encoded.subspan(8, 2));
  const std::uint16_t flags = ReadU16(encoded.subspan(10, 2));
  const std::uint32_t stream_id = ReadU32(encoded.subspan(12, 4));
  const std::uint64_t message_id = ReadU64(encoded.subspan(16, 8));
  const std::uint32_t body_length = ReadU32(encoded.subspan(24, 4));

  if (header_length < kFixedHeaderLength) {
    result.error = MakeError(ErrorCode::kMalformedFrame,
                             "header length is below the fixed header");
    return result;
  }
  if (header_length > kMaxHeaderLength) {
    result.error =
        MakeError(ErrorCode::kFrameTooLarge, "header length exceeds the hard limit");
    return result;
  }
  if (body_length > kMaxBodyLength) {
    result.error =
        MakeError(ErrorCode::kFrameTooLarge, "body length exceeds the hard limit");
    return result;
  }
  if (flags != 0U) {
    result.error =
        MakeError(ErrorCode::kMalformedFrame, "reserved frame flags are set");
    return result;
  }
  if (message_id == 0U) {
    result.error =
        MakeError(ErrorCode::kMessageIdViolation, "message ID zero is invalid");
    return result;
  }
  if (version != expected_version) {
    result.error =
        MakeError(ErrorCode::kUnsupportedVersion, "frame uses an unexpected version");
    return result;
  }
  if (!IsKnownMessageType(raw_message_type)) {
    result.error =
        MakeError(ErrorCode::kUnsupportedMessage, "frame uses an unknown message type");
    return result;
  }

  const std::size_t header_size = static_cast<std::size_t>(header_length);
  const std::size_t body_size = static_cast<std::size_t>(body_length);
  if (body_size > std::numeric_limits<std::size_t>::max() - header_size) {
    result.error = MakeError(ErrorCode::kMalformedFrame, "frame length overflows");
    return result;
  }
  const std::size_t total_size = header_size + body_size;
  if (encoded.size() < header_size) {
    result.error =
        MakeError(ErrorCode::kMalformedFrame, "header extensions are truncated");
    return result;
  }

  const MessageType message_type = static_cast<MessageType>(raw_message_type);
  result.frame.header = FrameHeader{header_length, version,    message_type, flags,
                                    stream_id,     message_id, body_length};
  result.frame.declared_total_length = total_size;
  result.frame.raw = encoded.first(header_size);
  result.frame.header_extensions =
      encoded.subspan(kFixedHeaderLength, header_size - kFixedHeaderLength);

  result.error =
      ParseTlvs(result.frame.header_extensions, {}, result.frame.header_fields);
  if (!result.error.ok()) {
    return result;
  }
  if (IsConnectionMessage(message_type) && stream_id != 0U) {
    result.error = MakeError(ErrorCode::kStateViolation,
                             "connection message uses a transfer stream");
    return result;
  }
  if (IsTransferMessage(message_type) && stream_id == 0U) {
    result.error =
        MakeError(ErrorCode::kStateViolation, "transfer message uses stream zero");
    return result;
  }
  return result;
}

ParseResult ParseFrameEnvelope(const std::span<const std::uint8_t> encoded,
                               const Version expected_version) noexcept {
  ParseResult result = ParseFrameHeader(encoded, expected_version);
  if (!result.ok()) {
    return result;
  }
  if (encoded.size() != result.frame.declared_total_length) {
    result.error = MakeError(ErrorCode::kMalformedFrame,
                             "encoded length differs from declared length");
    return result;
  }

  const std::size_t header_size =
      static_cast<std::size_t>(result.frame.header.header_length);
  const std::size_t body_size =
      static_cast<std::size_t>(result.frame.header.body_length);
  result.frame.raw = encoded;
  result.frame.body = encoded.subspan(header_size, body_size);
  return result;
}

Error ParseFrameBody(ParsedFrame& frame) noexcept {
  if (frame.body.size() != frame.header.body_length) {
    return MakeError(ErrorCode::kMalformedFrame,
                     "body view differs from the declared length");
  }
  frame.body_fields = {};
  return ParseTlvs(frame.body, SchemaFor(frame.header.message_type), frame.body_fields);
}

ParseResult ParseFrame(const std::span<const std::uint8_t> encoded,
                       const Version expected_version) noexcept {
  ParseResult result = ParseFrameEnvelope(encoded, expected_version);
  if (!result.ok()) {
    return result;
  }
  result.error = ParseFrameBody(result.frame);
  return result;
}

Error TranscriptParser::Complete(const Error error,
                                 const std::uint32_t stream_id) noexcept {
  if (error.ok()) {
    return error;
  }
  switch (error.code) {
    case ErrorCode::kMalformedFrame:
    case ErrorCode::kFrameTooLarge:
    case ErrorCode::kUnsupportedVersion:
    case ErrorCode::kDowngradeDetected:
    case ErrorCode::kUnsupportedCapability:
    case ErrorCode::kUnsupportedMessage:
    case ErrorCode::kMessageIdViolation:
      terminal_ = true;
      break;
    case ErrorCode::kMalformedMessage:
    case ErrorCode::kUnknownCriticalField:
    case ErrorCode::kStateViolation:
    case ErrorCode::kLimitExceeded:
      terminal_ = !binding_frames_complete_ || stream_id == 0U;
      break;
    case ErrorCode::kNone:
      break;
  }
  return error;
}

Error TranscriptParser::Process(const Direction direction,
                                const std::span<const std::uint8_t> encoded) noexcept {
  if (terminal_) {
    return MakeError(ErrorCode::kStateViolation,
                     "transcript is terminal after a fatal error");
  }
  std::size_t direction_index = 0;
  if (!DirectionIndex(direction, direction_index)) {
    return Complete(
        MakeError(ErrorCode::kStateViolation, "invalid transport direction"), 0);
  }

  const Version expected_version =
      negotiation_acknowledged_ ? negotiation_.selected_version : Version{};
  ParseResult parsed = ParseFrameEnvelope(encoded, expected_version);
  if (!parsed.ok()) {
    return Complete(parsed.error, 0);
  }

  if (message_id_exhausted_[direction_index] ||
      parsed.frame.header.message_id != next_message_id_[direction_index]) {
    return Complete(MakeError(ErrorCode::kMessageIdViolation,
                              "message ID is not the next directional value"),
                    parsed.frame.header.stream_id);
  }
  if (next_message_id_[direction_index] == std::numeric_limits<std::uint64_t>::max()) {
    message_id_exhausted_[direction_index] = true;
  } else {
    ++next_message_id_[direction_index];
  }

  if (negotiation_acknowledged_ &&
      parsed.frame.header.body_length > negotiation_.effective_max_body) {
    return Complete(MakeError(ErrorCode::kLimitExceeded,
                              "body length exceeds the negotiated limit"),
                    parsed.frame.header.stream_id);
  }

  const Error state_error = ValidateEnvelopeState(direction, parsed.frame);
  if (!state_error.ok()) {
    return Complete(state_error, parsed.frame.header.stream_id);
  }
  const Error body_error = ParseFrameBody(parsed.frame);
  if (!body_error.ok()) {
    return Complete(body_error, parsed.frame.header.stream_id);
  }

  switch (parsed.frame.header.message_type) {
    case MessageType::kHello:
      return Complete(ProcessHello(direction, parsed.frame),
                      parsed.frame.header.stream_id);
    case MessageType::kNegotiate:
      return Complete(ProcessNegotiate(direction, parsed.frame),
                      parsed.frame.header.stream_id);
    case MessageType::kNegotiateAck:
      return Complete(ProcessNegotiateAck(direction, parsed.frame),
                      parsed.frame.header.stream_id);
    case MessageType::kTransportFinished:
      return Complete(ProcessTransportFinished(direction, parsed.frame),
                      parsed.frame.header.stream_id);
    case MessageType::kError:
      if (!binding_frames_complete_ && parsed.frame.header.stream_id != 0U) {
        return Complete(MakeError(ErrorCode::kStateViolation,
                                  "pre-establishment ERROR is not connection scoped"),
                        parsed.frame.header.stream_id);
      }
      return {};
    case MessageType::kPing:
      return Complete(ProcessPing(direction, parsed.frame),
                      parsed.frame.header.stream_id);
    case MessageType::kPong:
      return Complete(ProcessPong(direction, parsed.frame),
                      parsed.frame.header.stream_id);
    default:
      if (!binding_frames_complete_) {
        return Complete(
            MakeError(ErrorCode::kStateViolation,
                      "message is not allowed before transport binding completes"),
            parsed.frame.header.stream_id);
      }
      return {};
  }
}

Error TranscriptParser::ValidateEnvelopeState(const Direction direction,
                                              const ParsedFrame& frame) const noexcept {
  std::size_t direction_index = 0;
  if (!DirectionIndex(direction, direction_index)) {
    return MakeError(ErrorCode::kStateViolation, "invalid transport direction");
  }

  switch (frame.header.message_type) {
    case MessageType::kHello:
      if (hellos_[direction_index].present || negotiation_.present ||
          negotiation_acknowledged_ || binding_frames_complete_) {
        return MakeError(ErrorCode::kStateViolation, "HELLO is repeated or late");
      }
      return {};
    case MessageType::kNegotiate:
      if (direction != Direction::kInitiatorToResponder) {
        return MakeError(ErrorCode::kStateViolation, "responder cannot send NEGOTIATE");
      }
      if (!hellos_[0].present || !hellos_[1].present) {
        return MakeError(ErrorCode::kStateViolation,
                         "NEGOTIATE arrived before both HELLO messages");
      }
      if (negotiation_.present) {
        return MakeError(ErrorCode::kStateViolation, "NEGOTIATE is repeated");
      }
      return {};
    case MessageType::kNegotiateAck:
      if (direction != Direction::kResponderToInitiator) {
        return MakeError(ErrorCode::kStateViolation,
                         "initiator cannot send NEGOTIATE_ACK");
      }
      if (!negotiation_.present) {
        return MakeError(ErrorCode::kStateViolation,
                         "NEGOTIATE_ACK arrived before NEGOTIATE");
      }
      if (negotiation_acknowledged_) {
        return MakeError(ErrorCode::kStateViolation, "NEGOTIATE_ACK is repeated");
      }
      return {};
    case MessageType::kTransportFinished:
      if (!negotiation_acknowledged_) {
        return MakeError(ErrorCode::kStateViolation,
                         "TRANSPORT_FINISHED arrived before negotiation ACK");
      }
      if (transport_finished_[direction_index]) {
        return MakeError(ErrorCode::kStateViolation, "TRANSPORT_FINISHED is repeated");
      }
      return {};
    case MessageType::kError:
      if (!binding_frames_complete_ && frame.header.stream_id != 0U) {
        return MakeError(ErrorCode::kStateViolation,
                         "pre-binding ERROR is not connection scoped");
      }
      return {};
    case MessageType::kPing:
    case MessageType::kPong:
    case MessageType::kGoAway:
      if (!binding_frames_complete_) {
        return MakeError(ErrorCode::kStateViolation,
                         "control message arrived before both binding frames");
      }
      return {};
    default:
      if (!binding_frames_complete_) {
        return MakeError(ErrorCode::kStateViolation,
                         "transfer message arrived before both binding frames");
      }
      return {};
  }
}

Error TranscriptParser::ProcessHello(const Direction direction,
                                     const ParsedFrame& frame) noexcept {
  std::size_t index = 0;
  if (!DirectionIndex(direction, index)) {
    return MakeError(ErrorCode::kStateViolation, "invalid HELLO direction");
  }
  if (hellos_[index].present || negotiation_.present || negotiation_acknowledged_ ||
      binding_frames_complete_) {
    return MakeError(ErrorCode::kStateViolation, "HELLO is repeated or late");
  }

  const std::uint64_t minimum_major = DecodeUnsigned(*frame.body_fields.FindFirst(1));
  const std::uint64_t minimum_minor = DecodeUnsigned(*frame.body_fields.FindFirst(2));
  const std::uint64_t maximum_major = DecodeUnsigned(*frame.body_fields.FindFirst(3));
  const std::uint64_t maximum_minor = DecodeUnsigned(*frame.body_fields.FindFirst(4));
  if (minimum_major == 0U || minimum_major > 255U || minimum_minor > 255U ||
      maximum_major > 255U || maximum_minor > 255U) {
    return MakeError(ErrorCode::kMalformedMessage,
                     "HELLO version component is outside the header range");
  }

  HelloData candidate{};
  candidate.minimum = Version{static_cast<std::uint8_t>(minimum_major),
                              static_cast<std::uint8_t>(minimum_minor)};
  candidate.maximum = Version{static_cast<std::uint8_t>(maximum_major),
                              static_cast<std::uint8_t>(maximum_minor)};
  if (VersionLess(candidate.maximum, candidate.minimum)) {
    return MakeError(ErrorCode::kMalformedMessage, "HELLO version range is reversed");
  }

  candidate.role =
      static_cast<std::uint8_t>(DecodeUnsigned(*frame.body_fields.FindFirst(5)));
  const std::uint8_t expected_role = index == 0 ? 1U : 2U;
  if (candidate.role != expected_role) {
    return MakeError(ErrorCode::kStateViolation,
                     "HELLO role conflicts with transport direction");
  }

  for (std::size_t field_index = 0; field_index < frame.body_fields.count;
       ++field_index) {
    const FieldView& field = frame.body_fields.fields[field_index];
    if (field.id == 6U) {
      candidate.capabilities[candidate.capability_count] =
          static_cast<std::uint32_t>(DecodeUnsigned(field));
      ++candidate.capability_count;
    } else if (field.id == 7U) {
      candidate.required_capabilities[candidate.required_capability_count] =
          static_cast<std::uint32_t>(DecodeUnsigned(field));
      ++candidate.required_capability_count;
    }
  }

  for (std::size_t capability_index = 1; capability_index < candidate.capability_count;
       ++capability_index) {
    const std::uint32_t previous = candidate.capabilities[capability_index - 1];
    const std::uint32_t current = candidate.capabilities[capability_index];
    if (current <= previous) {
      return MakeError(ErrorCode::kMalformedMessage,
                       "HELLO capabilities are not sorted and unique");
    }
    if ((current >> 16U) == (previous >> 16U)) {
      return MakeError(ErrorCode::kMalformedMessage,
                       "HELLO advertises multiple capability versions");
    }
  }
  for (std::size_t required_index = 1;
       required_index < candidate.required_capability_count; ++required_index) {
    if (candidate.required_capabilities[required_index] <=
        candidate.required_capabilities[required_index - 1]) {
      return MakeError(ErrorCode::kMalformedMessage,
                       "HELLO required capabilities are not sorted and unique");
    }
  }
  for (std::size_t required_index = 0;
       required_index < candidate.required_capability_count; ++required_index) {
    if (!Contains(candidate.capabilities, candidate.capability_count,
                  candidate.required_capabilities[required_index])) {
      return MakeError(ErrorCode::kUnsupportedCapability,
                       "required capability was not advertised");
    }
  }
  if (!Contains(candidate.capabilities, candidate.capability_count, kBaseTransferV1) ||
      !Contains(candidate.required_capabilities, candidate.required_capability_count,
                kBaseTransferV1)) {
    return MakeError(ErrorCode::kUnsupportedCapability,
                     "BASE_TRANSFER_V1 is not advertised and required");
  }

  candidate.receive_max_body =
      static_cast<std::uint32_t>(DecodeUnsigned(*frame.body_fields.FindFirst(8)));
  candidate.receive_max_in_flight =
      static_cast<std::uint32_t>(DecodeUnsigned(*frame.body_fields.FindFirst(9)));
  candidate.receive_max_streams =
      static_cast<std::uint16_t>(DecodeUnsigned(*frame.body_fields.FindFirst(10)));
  if (candidate.receive_max_body < 8'192U ||
      candidate.receive_max_body > kMaxBodyLength) {
    return MakeError(ErrorCode::kLimitExceeded,
                     "HELLO receive_max_body is outside v1 limits");
  }
  if (candidate.receive_max_in_flight < 1'048'576U ||
      candidate.receive_max_in_flight > kMaxInFlight) {
    return MakeError(ErrorCode::kLimitExceeded,
                     "HELLO receive_max_in_flight is outside v1 limits");
  }
  if (candidate.receive_max_streams == 0U ||
      candidate.receive_max_streams > kMaxStreams) {
    return MakeError(ErrorCode::kLimitExceeded,
                     "HELLO receive_max_streams is outside v1 limits");
  }

  candidate.present = true;
  hellos_[index] = candidate;
  return {};
}

Error TranscriptParser::ProcessNegotiate(const Direction direction,
                                         const ParsedFrame& frame) noexcept {
  if (direction != Direction::kInitiatorToResponder) {
    return MakeError(ErrorCode::kStateViolation, "responder cannot send NEGOTIATE");
  }
  if (!hellos_[0].present || !hellos_[1].present) {
    return MakeError(ErrorCode::kStateViolation,
                     "NEGOTIATE arrived before both HELLO messages");
  }
  if (negotiation_.present) {
    return MakeError(ErrorCode::kStateViolation, "NEGOTIATE is repeated");
  }

  const std::uint64_t selected_major = DecodeUnsigned(*frame.body_fields.FindFirst(1));
  const std::uint64_t selected_minor = DecodeUnsigned(*frame.body_fields.FindFirst(2));
  if (selected_major == 0U || selected_major > 255U || selected_minor > 255U) {
    return MakeError(ErrorCode::kMalformedMessage,
                     "selected version is outside the header range");
  }

  NegotiationData candidate{};
  candidate.selected_version = Version{static_cast<std::uint8_t>(selected_major),
                                       static_cast<std::uint8_t>(selected_minor)};
  for (std::size_t field_index = 0; field_index < frame.body_fields.count;
       ++field_index) {
    const FieldView& field = frame.body_fields.fields[field_index];
    if (field.id == 3U) {
      candidate.capabilities[candidate.capability_count] =
          static_cast<std::uint32_t>(DecodeUnsigned(field));
      ++candidate.capability_count;
    }
  }
  for (std::size_t capability_index = 1; capability_index < candidate.capability_count;
       ++capability_index) {
    if (candidate.capabilities[capability_index] <=
        candidate.capabilities[capability_index - 1]) {
      return MakeError(ErrorCode::kMalformedMessage,
                       "selected capabilities are not sorted and unique");
    }
  }

  const Version common_minimum = VersionMaximum(hellos_[0].minimum, hellos_[1].minimum);
  const Version common_maximum = VersionMinimum(hellos_[0].maximum, hellos_[1].maximum);
  if (VersionLess(common_maximum, common_minimum)) {
    return MakeError(ErrorCode::kUnsupportedVersion,
                     "HELLO version ranges do not intersect");
  }
  if (candidate.selected_version != common_maximum) {
    return MakeError(ErrorCode::kDowngradeDetected,
                     "highest common version was not selected");
  }

  std::array<std::uint32_t, kMaxFields> intersection{};
  std::size_t intersection_count = 0;
  std::size_t initiator_index = 0;
  std::size_t responder_index = 0;
  while (initiator_index < hellos_[0].capability_count &&
         responder_index < hellos_[1].capability_count) {
    const std::uint32_t initiator_value = hellos_[0].capabilities[initiator_index];
    const std::uint32_t responder_value = hellos_[1].capabilities[responder_index];
    if (initiator_value == responder_value) {
      intersection[intersection_count] = initiator_value;
      ++intersection_count;
      ++initiator_index;
      ++responder_index;
    } else if (initiator_value < responder_value) {
      ++initiator_index;
    } else {
      ++responder_index;
    }
  }

  for (const HelloData& hello : hellos_) {
    for (std::size_t required_index = 0;
         required_index < hello.required_capability_count; ++required_index) {
      if (!Contains(intersection, intersection_count,
                    hello.required_capabilities[required_index])) {
        return MakeError(ErrorCode::kUnsupportedCapability,
                         "required capability is unavailable");
      }
    }
  }
  if (!EqualCapabilities(candidate.capabilities, candidate.capability_count,
                         intersection, intersection_count)) {
    return MakeError(ErrorCode::kUnsupportedCapability,
                     "selected capability set is not the intersection");
  }

  candidate.effective_max_body =
      static_cast<std::uint32_t>(DecodeUnsigned(*frame.body_fields.FindFirst(4)));
  candidate.effective_max_in_flight =
      static_cast<std::uint32_t>(DecodeUnsigned(*frame.body_fields.FindFirst(5)));
  candidate.effective_max_streams =
      static_cast<std::uint16_t>(DecodeUnsigned(*frame.body_fields.FindFirst(6)));
  if (candidate.effective_max_body !=
          std::min(hellos_[0].receive_max_body, hellos_[1].receive_max_body) ||
      candidate.effective_max_in_flight != std::min(hellos_[0].receive_max_in_flight,
                                                    hellos_[1].receive_max_in_flight) ||
      candidate.effective_max_streams !=
          std::min(hellos_[0].receive_max_streams, hellos_[1].receive_max_streams)) {
    return MakeError(ErrorCode::kMalformedMessage,
                     "selected limits are not the advertised minima");
  }

  candidate.present = true;
  negotiation_ = candidate;
  return {};
}

Error TranscriptParser::ProcessNegotiateAck(const Direction direction,
                                            const ParsedFrame& frame) noexcept {
  if (direction != Direction::kResponderToInitiator) {
    return MakeError(ErrorCode::kStateViolation, "initiator cannot send NEGOTIATE_ACK");
  }
  if (!negotiation_.present) {
    return MakeError(ErrorCode::kStateViolation,
                     "NEGOTIATE_ACK arrived before NEGOTIATE");
  }
  if (negotiation_acknowledged_) {
    return MakeError(ErrorCode::kStateViolation, "NEGOTIATE_ACK is repeated");
  }

  const std::uint64_t selected_major = DecodeUnsigned(*frame.body_fields.FindFirst(1));
  const std::uint64_t selected_minor = DecodeUnsigned(*frame.body_fields.FindFirst(2));
  if (selected_major > 255U || selected_minor > 255U) {
    return MakeError(ErrorCode::kMalformedMessage,
                     "ACK version is outside the header range");
  }
  const Version selected_version{static_cast<std::uint8_t>(selected_major),
                                 static_cast<std::uint8_t>(selected_minor)};

  std::array<std::uint32_t, kMaxFields> capabilities{};
  std::size_t capability_count = 0;
  for (std::size_t field_index = 0; field_index < frame.body_fields.count;
       ++field_index) {
    const FieldView& field = frame.body_fields.fields[field_index];
    if (field.id == 3U) {
      capabilities[capability_count] =
          static_cast<std::uint32_t>(DecodeUnsigned(field));
      ++capability_count;
    }
  }

  const std::uint32_t effective_max_body =
      static_cast<std::uint32_t>(DecodeUnsigned(*frame.body_fields.FindFirst(4)));
  const std::uint32_t effective_max_in_flight =
      static_cast<std::uint32_t>(DecodeUnsigned(*frame.body_fields.FindFirst(5)));
  const std::uint16_t effective_max_streams =
      static_cast<std::uint16_t>(DecodeUnsigned(*frame.body_fields.FindFirst(6)));
  if (selected_version != negotiation_.selected_version ||
      !EqualCapabilities(capabilities, capability_count, negotiation_.capabilities,
                         negotiation_.capability_count) ||
      effective_max_body != negotiation_.effective_max_body ||
      effective_max_in_flight != negotiation_.effective_max_in_flight ||
      effective_max_streams != negotiation_.effective_max_streams) {
    return MakeError(ErrorCode::kMalformedMessage,
                     "NEGOTIATE_ACK does not match NEGOTIATE");
  }

  negotiation_acknowledged_ = true;
  return {};
}

Error TranscriptParser::ProcessTransportFinished(const Direction direction,
                                                 const ParsedFrame& frame) noexcept {
  std::size_t index = 0;
  if (!DirectionIndex(direction, index)) {
    return MakeError(ErrorCode::kStateViolation,
                     "invalid TRANSPORT_FINISHED direction");
  }
  if (!negotiation_acknowledged_) {
    return MakeError(ErrorCode::kStateViolation,
                     "TRANSPORT_FINISHED arrived before negotiation ACK");
  }
  if (transport_finished_[index]) {
    return MakeError(ErrorCode::kStateViolation, "TRANSPORT_FINISHED is repeated");
  }

  const std::uint8_t sender_role =
      static_cast<std::uint8_t>(DecodeUnsigned(*frame.body_fields.FindFirst(1)));
  const std::uint8_t expected_role = index == 0 ? 1U : 2U;
  if (sender_role != expected_role) {
    return MakeError(ErrorCode::kStateViolation,
                     "TRANSPORT_FINISHED role conflicts with transport direction");
  }

  // Framing validates only the 1..64 byte envelope. Cryptographic verification
  // belongs to the separately approved SecurityProfile.
  transport_finished_[index] = true;
  binding_frames_complete_ = transport_finished_[0] && transport_finished_[1];
  return {};
}

Error TranscriptParser::ProcessPing(const Direction direction,
                                    const ParsedFrame& frame) noexcept {
  if (!binding_frames_complete_) {
    return MakeError(ErrorCode::kStateViolation,
                     "PING arrived before both binding frames");
  }
  std::size_t index = 0;
  if (!DirectionIndex(direction, index)) {
    return MakeError(ErrorCode::kStateViolation, "invalid PING direction");
  }
  const std::size_t response_index = index ^ 1U;
  if (pong_expected_[response_index]) {
    return MakeError(ErrorCode::kStateViolation, "a PING is already outstanding");
  }
  pong_expected_[response_index] = true;
  expected_pong_token_[response_index] =
      DecodeUnsigned(*frame.body_fields.FindFirst(1));
  return {};
}

Error TranscriptParser::ProcessPong(const Direction direction,
                                    const ParsedFrame& frame) noexcept {
  if (!binding_frames_complete_) {
    return MakeError(ErrorCode::kStateViolation,
                     "PONG arrived before both binding frames");
  }
  std::size_t index = 0;
  if (!DirectionIndex(direction, index)) {
    return MakeError(ErrorCode::kStateViolation, "invalid PONG direction");
  }
  const std::uint64_t token = DecodeUnsigned(*frame.body_fields.FindFirst(1));
  if (!pong_expected_[index] || expected_pong_token_[index] != token) {
    return MakeError(ErrorCode::kStateViolation,
                     "PONG does not match an outstanding PING");
  }
  pong_expected_[index] = false;
  return {};
}

}  // namespace xnn_transfer::protocol::v1
