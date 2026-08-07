#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "xnn_transfer/core/discovery/discovery.hpp"

namespace xnn_transfer::core::discovery {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'X', 'N', 'N', 'D'};
constexpr std::uint8_t kVersionMajor = 1;
constexpr std::uint8_t kVersionMinor = 0;
constexpr std::uint16_t kMinimumTtlSeconds = 5;
constexpr std::uint16_t kMaximumTtlSeconds = 60;
constexpr std::size_t kMaximumTlvs = 32;
constexpr std::uint16_t kDisplayLabelField = 1;
constexpr std::uint16_t kCriticalBit = 0x8000;
constexpr std::uint16_t kFieldIdMask = 0x7fff;

[[nodiscard]] std::uint16_t ReadU16(
    const std::span<const std::uint8_t> bytes) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) |
                                    static_cast<std::uint16_t>(bytes[1]));
}

[[nodiscard]] std::uint64_t ReadU64(
    const std::span<const std::uint8_t> bytes) noexcept {
  std::uint64_t value = 0;
  for (const std::uint8_t byte : bytes.first<8>()) {
    value = (value << 8U) | static_cast<std::uint64_t>(byte);
  }
  return value;
}

void WriteU16(std::array<std::uint8_t, kMaxDatagramSize>& bytes,
              const std::size_t offset, const std::uint16_t value) noexcept {
  bytes[offset] = static_cast<std::uint8_t>(value >> 8U);
  bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

void WriteU64(std::array<std::uint8_t, kMaxDatagramSize>& bytes,
              const std::size_t offset, const std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < 8; ++index) {
    const unsigned shift = static_cast<unsigned>((7U - index) * 8U);
    bytes[offset + index] = static_cast<std::uint8_t>(value >> shift);
  }
}

[[nodiscard]] bool IsAllZero(const InstanceToken& token) noexcept {
  return std::all_of(token.begin(), token.end(),
                     [](const std::uint8_t value) { return value == 0; });
}

[[nodiscard]] ParseResult Fail(const ParseError error) noexcept {
  return ParseResult{.advertisement = {}, .error = error};
}

}  // namespace

IpAddress IpAddress::V4(const std::array<std::uint8_t, 4>& value) noexcept {
  IpAddress result{.family = AddressFamily::kIpv4};
  std::copy(value.begin(), value.end(), result.bytes.begin());
  return result;
}

IpAddress IpAddress::V6(const std::array<std::uint8_t, 16>& value) noexcept {
  return IpAddress{.family = AddressFamily::kIpv6, .bytes = value};
}

std::span<const std::uint8_t> IpAddress::encoded() const noexcept {
  const std::size_t size = family == AddressFamily::kIpv4 ? 4U : 16U;
  return std::span<const std::uint8_t>(bytes).first(size);
}

bool operator==(const IpAddress& left, const IpAddress& right) noexcept {
  return left.family == right.family &&
         std::equal(left.encoded().begin(), left.encoded().end(),
                    right.encoded().begin());
}

bool operator<(const IpAddress& left, const IpAddress& right) noexcept {
  if (left.family != right.family) {
    return left.family < right.family;
  }
  return std::lexicographical_compare(left.encoded().begin(), left.encoded().end(),
                                      right.encoded().begin(), right.encoded().end());
}

bool operator<(const InterfaceScope& left, const InterfaceScope& right) noexcept {
  if (left.generation != right.generation) {
    return left.generation < right.generation;
  }
  return left.family < right.family;
}

bool operator<(const NetworkInterface& left, const NetworkInterface& right) noexcept {
  if (left.scope != right.scope) {
    return left.scope < right.scope;
  }
  if (left.system_index != right.system_index) {
    return left.system_index < right.system_index;
  }
  if (left.local_address != right.local_address) {
    return left.local_address < right.local_address;
  }
  return left.prefix_length < right.prefix_length;
}

bool operator<(const CandidateKey& left, const CandidateKey& right) noexcept {
  if (left.observer != right.observer) {
    return left.observer < right.observer;
  }
  if (left.source != right.source) {
    return left.source < right.source;
  }
  return left.token < right.token;
}

std::span<const std::uint8_t> Advertisement::display_label() const noexcept {
  return std::span<const std::uint8_t>(label).first(label_size);
}

std::span<const std::uint8_t> Advertisement::raw_datagram() const noexcept {
  return std::span<const std::uint8_t>(raw).first(raw_size);
}

std::span<const std::uint8_t> EncodedDatagram::payload() const noexcept {
  return std::span<const std::uint8_t>(bytes).first(size);
}

ParseResult ParseAdvertisement(const std::span<const std::uint8_t> payload,
                               const DisplayLabelValidator& label_validator) noexcept {
  if (payload.size() < kDiscoveryHeaderSize) {
    return Fail(ParseError::kTooShort);
  }
  if (payload.size() > kMaxDatagramSize) {
    return Fail(ParseError::kTooLarge);
  }
  if (!std::equal(kMagic.begin(), kMagic.end(), payload.begin())) {
    return Fail(ParseError::kBadMagic);
  }
  if (payload[4] != kVersionMajor || payload[5] != kVersionMinor) {
    return Fail(ParseError::kUnsupportedVersion);
  }

  const std::uint16_t total_length = ReadU16(payload.subspan(8, 2));
  const std::uint16_t header_length = ReadU16(payload.subspan(10, 2));
  if (total_length != payload.size()) {
    return Fail(ParseError::kLengthMismatch);
  }
  if (header_length != kDiscoveryHeaderSize) {
    return Fail(ParseError::kBadHeaderLength);
  }
  if (payload[7] != 0) {
    return Fail(ParseError::kNonzeroFlags);
  }
  if (ReadU16(payload.subspan(42, 2)) != 0) {
    return Fail(ParseError::kNonzeroReserved);
  }

  const std::uint16_t tlv_length = ReadU16(payload.subspan(40, 2));
  if (tlv_length != total_length - header_length) {
    return Fail(ParseError::kLengthMismatch);
  }

  Advertisement advertisement{};
  switch (payload[6]) {
    case static_cast<std::uint8_t>(MessageType::kAnnounce):
      advertisement.type = MessageType::kAnnounce;
      break;
    case static_cast<std::uint8_t>(MessageType::kWithdraw):
      advertisement.type = MessageType::kWithdraw;
      break;
    default:
      return Fail(ParseError::kUnknownMessage);
  }

  advertisement.sequence = ReadU64(payload.subspan(12, 8));
  if (advertisement.sequence == 0) {
    return Fail(ParseError::kZeroSequence);
  }
  std::copy_n(payload.begin() + 20, advertisement.token.size(),
              advertisement.token.begin());
  if (IsAllZero(advertisement.token)) {
    return Fail(ParseError::kZeroToken);
  }
  advertisement.service_port = ReadU16(payload.subspan(36, 2));
  advertisement.ttl_seconds = ReadU16(payload.subspan(38, 2));

  if (advertisement.type == MessageType::kAnnounce) {
    if (advertisement.service_port == 0 ||
        advertisement.ttl_seconds < kMinimumTtlSeconds ||
        advertisement.ttl_seconds > kMaximumTtlSeconds) {
      return Fail(ParseError::kInvalidAnnounce);
    }
  } else if (advertisement.service_port != 0 || advertisement.ttl_seconds != 0 ||
             tlv_length != 0) {
    return Fail(ParseError::kInvalidWithdraw);
  }

  std::size_t offset = header_length;
  std::uint16_t previous_field_id = 0;
  std::size_t tlv_count = 0;
  while (offset < total_length) {
    if (total_length - offset < 4) {
      return Fail(ParseError::kTruncatedTlv);
    }
    const std::uint16_t wire_type = ReadU16(payload.subspan(offset, 2));
    const std::uint16_t value_length = ReadU16(payload.subspan(offset + 2, 2));
    offset += 4;
    if (value_length > total_length - offset) {
      return Fail(ParseError::kTruncatedTlv);
    }

    const std::uint16_t field_id = wire_type & kFieldIdMask;
    const bool critical = (wire_type & kCriticalBit) != 0;
    if (field_id == 0) {
      return Fail(ParseError::kReservedTlv);
    }
    if (field_id < previous_field_id) {
      return Fail(ParseError::kTlvOrder);
    }
    if (field_id == previous_field_id) {
      return Fail(ParseError::kDuplicateTlv);
    }
    previous_field_id = field_id;
    ++tlv_count;
    if (tlv_count > kMaximumTlvs) {
      return Fail(ParseError::kTooManyTlvs);
    }

    const std::span<const std::uint8_t> value = payload.subspan(offset, value_length);
    offset += value_length;
    if (field_id == kDisplayLabelField && !critical) {
      if (value.empty() || value.size() > kMaxDisplayLabelBytes ||
          !label_validator.IsCanonical(value)) {
        return Fail(ParseError::kInvalidLabel);
      }
      advertisement.label_size = static_cast<std::uint8_t>(value.size());
      std::copy(value.begin(), value.end(), advertisement.label.begin());
    } else if (critical) {
      return Fail(ParseError::kUnknownCriticalTlv);
    }
  }

  advertisement.raw_size = static_cast<std::uint16_t>(payload.size());
  std::copy(payload.begin(), payload.end(), advertisement.raw.begin());
  return ParseResult{.advertisement = advertisement, .error = ParseError::kNone};
}

bool EncodeAdvertisement(const MessageType type, const std::uint64_t sequence,
                         const InstanceToken& token, const std::uint16_t service_port,
                         const std::uint16_t ttl_seconds,
                         const std::span<const std::uint8_t> display_label,
                         const DisplayLabelValidator& label_validator,
                         EncodedDatagram& output) noexcept {
  output = {};
  if (sequence == 0 || IsAllZero(token)) {
    return false;
  }

  std::size_t tlv_size = 0;
  if (type == MessageType::kAnnounce) {
    if (service_port == 0 || ttl_seconds < kMinimumTtlSeconds ||
        ttl_seconds > kMaximumTtlSeconds) {
      return false;
    }
    if (!display_label.empty()) {
      if (display_label.size() > kMaxDisplayLabelBytes ||
          !label_validator.IsCanonical(display_label)) {
        return false;
      }
      tlv_size = 4 + display_label.size();
    }
  } else if (type == MessageType::kWithdraw) {
    if (service_port != 0 || ttl_seconds != 0 || !display_label.empty()) {
      return false;
    }
  } else {
    return false;
  }

  const std::size_t total_size = kDiscoveryHeaderSize + tlv_size;
  auto& bytes = output.bytes;
  std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
  bytes[4] = kVersionMajor;
  bytes[5] = kVersionMinor;
  bytes[6] = static_cast<std::uint8_t>(type);
  bytes[7] = 0;
  WriteU16(bytes, 8, static_cast<std::uint16_t>(total_size));
  WriteU16(bytes, 10, static_cast<std::uint16_t>(kDiscoveryHeaderSize));
  WriteU64(bytes, 12, sequence);
  std::copy(token.begin(), token.end(), bytes.begin() + 20);
  WriteU16(bytes, 36, service_port);
  WriteU16(bytes, 38, ttl_seconds);
  WriteU16(bytes, 40, static_cast<std::uint16_t>(tlv_size));
  WriteU16(bytes, 42, 0);

  if (!display_label.empty()) {
    WriteU16(bytes, kDiscoveryHeaderSize, kDisplayLabelField);
    WriteU16(bytes, kDiscoveryHeaderSize + 2,
             static_cast<std::uint16_t>(display_label.size()));
    std::copy(display_label.begin(), display_label.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(kDiscoveryHeaderSize + 4));
  }
  output.size = static_cast<std::uint16_t>(total_size);
  return true;
}

}  // namespace xnn_transfer::core::discovery
