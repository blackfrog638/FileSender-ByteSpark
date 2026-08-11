#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <new>
#include <utility>

#include "internal.hpp"

namespace xnn_transfer::core::session::internal {
namespace {

constexpr std::array<std::uint8_t, 4> kPairingMagic{'X', 'N', 'N', 'P'};
constexpr std::array<std::uint8_t, 4> kCanonicalMagic{'X', 'N', 'N', 'S'};
constexpr std::uint32_t kResumeV1 = 0x0002'0001U;
constexpr std::uint32_t kParallelFilesV1 = 0x0003'0001U;
constexpr std::uint32_t kMinimumReceiveBody = 8'192;
constexpr std::uint32_t kMaximumReceiveBody = 1'048'576;
constexpr std::uint32_t kMinimumReceiveInFlight = 1'048'576;
constexpr std::uint32_t kMaximumReceiveInFlight = 67'108'864;
constexpr std::uint16_t kMaximumReceiveStreams = 32;
constexpr std::size_t kMaximumCapabilities = 256;

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

void AppendU16(Bytes& output, const std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value));
}

void AppendU32(Bytes& output, const std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 24U));
  output.push_back(static_cast<std::uint8_t>(value >> 16U));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value));
}

[[nodiscard]] Bytes U8(const std::uint8_t value) { return Bytes{value}; }

[[nodiscard]] Bytes U16(const std::uint16_t value) {
  Bytes output;
  output.reserve(2);
  AppendU16(output, value);
  return output;
}

[[nodiscard]] Bytes EncodeVersionRange(const VersionRange& range) {
  return Bytes{
      range.minimum.major,
      range.minimum.minor,
      range.maximum.major,
      range.maximum.minor,
  };
}

[[nodiscard]] Bytes EncodeVersion(const Version version) {
  return Bytes{version.major, version.minor};
}

[[nodiscard]] Bytes EncodeCapabilities(const std::vector<std::uint32_t>& capabilities) {
  Bytes output;
  output.reserve(2 + capabilities.size() * 4);
  AppendU16(output, static_cast<std::uint16_t>(capabilities.size()));
  for (const std::uint32_t capability : capabilities) {
    AppendU32(output, capability);
  }
  return output;
}

[[nodiscard]] Bytes EncodeLimits(const ReceiveLimits& limits) {
  Bytes output;
  output.reserve(10);
  AppendU32(output, limits.max_body);
  AppendU32(output, limits.max_in_flight);
  AppendU16(output, limits.max_streams);
  return output;
}

[[nodiscard]] bool IsKnownPairingType(const std::uint16_t value) noexcept {
  return value >= static_cast<std::uint16_t>(PairingMessageType::kHello) &&
         value <= static_cast<std::uint16_t>(PairingMessageType::kAbort);
}

[[nodiscard]] bool IsKnownWireType(const std::uint8_t value) noexcept {
  return value >= static_cast<std::uint8_t>(WireType::kU8) &&
         value <= static_cast<std::uint8_t>(WireType::kBytes);
}

[[nodiscard]] std::size_t FixedLength(const WireType type) noexcept {
  switch (type) {
    case WireType::kU8:
      return 1;
    case WireType::kU16:
      return 2;
    case WireType::kU32:
      return 4;
    case WireType::kU64:
      return 8;
    case WireType::kBytes:
      return 0;
  }
  return 0;
}

[[nodiscard]] bool HasCanonicalCapabilities(
    const std::vector<std::uint32_t>& capabilities) noexcept {
  if (capabilities.empty() || capabilities.size() > kMaximumCapabilities ||
      !std::is_sorted(capabilities.begin(), capabilities.end()) ||
      std::adjacent_find(capabilities.begin(), capabilities.end()) !=
          capabilities.end()) {
    return false;
  }
  for (std::size_t index = 1; index < capabilities.size(); ++index) {
    if ((capabilities[index - 1] >> 16U) == (capabilities[index] >> 16U)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] PairingError DecodeCapabilities(
    const std::span<const std::uint8_t> encoded, std::vector<std::uint32_t>& output) {
  if (encoded.size() < 2) {
    return PairingError::kMalformed;
  }
  const std::uint16_t count = ReadU16(encoded.first(2));
  if (count == 0 || count > kMaximumCapabilities ||
      encoded.size() != 2 + static_cast<std::size_t>(count) * 4) {
    return PairingError::kMalformed;
  }
  output.clear();
  output.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    output.push_back(ReadU32(encoded.subspan(2 + index * 4, 4)));
  }
  return HasCanonicalCapabilities(output) ? PairingError::kNone
                                          : PairingError::kMalformed;
}

[[nodiscard]] bool HasFields(const Frame& frame,
                             const std::span<const WireType> types) noexcept {
  if (frame.fields.size() != types.size()) {
    return false;
  }
  for (std::size_t index = 0; index < types.size(); ++index) {
    if (frame.fields[index].id != index + 1 ||
        frame.fields[index].type != types[index]) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] Bytes EncodeFrame(const PairingMessageType type,
                                const std::uint32_t sequence,
                                const std::span<const Field> fields) {
  Bytes body;
  for (const Field& field : fields) {
    AppendU16(body, field.id);
    body.push_back(static_cast<std::uint8_t>(field.type));
    body.push_back(1);
    AppendU32(body, static_cast<std::uint32_t>(field.value.size()));
    body.insert(body.end(), field.value.begin(), field.value.end());
  }
  if (body.size() > kMaxPairingBodySize) {
    return {};
  }

  Bytes output;
  output.reserve(kPairingFrameHeaderSize + body.size());
  output.insert(output.end(), kPairingMagic.begin(), kPairingMagic.end());
  AppendU16(output, static_cast<std::uint16_t>(kPairingFrameHeaderSize));
  output.push_back(1);
  output.push_back(0);
  AppendU16(output, static_cast<std::uint16_t>(type));
  AppendU16(output, 0);
  AppendU32(output, sequence);
  AppendU32(output, static_cast<std::uint32_t>(body.size()));
  output.insert(output.end(), body.begin(), body.end());
  return output;
}

struct CanonicalField {
  std::uint16_t id{};
  Bytes value{};
};

[[nodiscard]] Bytes EncodeNormalizedObject(
    const std::span<const CanonicalField> fields) {
  Bytes body;
  for (const CanonicalField& field : fields) {
    AppendU16(body, field.id);
    AppendU32(body, static_cast<std::uint32_t>(field.value.size()));
    body.insert(body.end(), field.value.begin(), field.value.end());
  }
  Bytes output;
  output.reserve(12 + body.size());
  output.insert(output.end(), kCanonicalMagic.begin(), kCanonicalMagic.end());
  output.push_back(1);
  output.push_back(static_cast<std::uint8_t>(
      security::tls::CanonicalObjectKind::kNormalizedNegotiation));
  AppendU16(output, static_cast<std::uint16_t>(fields.size()));
  AppendU32(output, static_cast<std::uint32_t>(body.size()));
  output.insert(output.end(), body.begin(), body.end());
  return output;
}

}  // namespace

bool VersionLess(const Version left, const Version right) noexcept {
  return left.major < right.major ||
         (left.major == right.major && left.minor < right.minor);
}

security::tls::Role OppositeRole(const security::tls::Role role) noexcept {
  return role == security::tls::Role::kInitiator ? security::tls::Role::kResponder
                                                 : security::tls::Role::kInitiator;
}

PairingError ValidateFrameHeader(const std::span<const std::uint8_t> encoded) noexcept {
  if (encoded.size() < kPairingFrameHeaderSize ||
      !std::equal(kPairingMagic.begin(), kPairingMagic.end(), encoded.begin()) ||
      ReadU16(encoded.subspan(4, 2)) != kPairingFrameHeaderSize) {
    return PairingError::kMalformed;
  }
  if (encoded[6] != 1 || encoded[7] != 0) {
    return PairingError::kUnsupportedVersion;
  }
  const std::uint16_t type = ReadU16(encoded.subspan(8, 2));
  if (!IsKnownPairingType(type) || ReadU16(encoded.subspan(10, 2)) != 0) {
    return PairingError::kMalformed;
  }
  return ReadU32(encoded.subspan(16, 4)) > kMaxPairingBodySize
             ? PairingError::kLimitExceeded
             : PairingError::kNone;
}

ParseResult ParseFrame(const std::span<const std::uint8_t> encoded) noexcept {
  const PairingError header_error = ValidateFrameHeader(encoded);
  if (header_error != PairingError::kNone) {
    return {.error = header_error};
  }
  const std::uint32_t body_length = ReadU32(encoded.subspan(16, 4));
  if (encoded.size() !=
      kPairingFrameHeaderSize + static_cast<std::size_t>(body_length)) {
    return {.error = PairingError::kMalformed};
  }
  const std::uint16_t type = ReadU16(encoded.subspan(8, 2));

  try {
    Frame frame{
        .type = static_cast<PairingMessageType>(type),
        .sequence = ReadU32(encoded.subspan(12, 4)),
    };
    std::size_t offset = kPairingFrameHeaderSize;
    std::uint16_t previous_id = 0;
    while (offset < encoded.size()) {
      if (frame.fields.size() == kMaxPairingFields || encoded.size() - offset < 8) {
        return {.error = PairingError::kMalformed};
      }
      const std::uint16_t id = ReadU16(encoded.subspan(offset, 2));
      const std::uint8_t raw_type = encoded[offset + 2];
      const std::uint8_t flags = encoded[offset + 3];
      const std::uint32_t value_length = ReadU32(encoded.subspan(offset + 4, 4));
      offset += 8;
      if (id == 0 || id <= previous_id || !IsKnownWireType(raw_type) || flags != 1 ||
          static_cast<std::size_t>(value_length) > encoded.size() - offset) {
        return {.error = PairingError::kMalformed};
      }
      const auto wire_type = static_cast<WireType>(raw_type);
      const std::size_t fixed_length = FixedLength(wire_type);
      if (fixed_length != 0 && value_length != fixed_length) {
        return {.error = PairingError::kMalformed};
      }
      frame.fields.push_back(Field{
          .id = id,
          .type = wire_type,
          .value = Bytes(
              encoded.begin() + static_cast<std::ptrdiff_t>(offset),
              encoded.begin() + static_cast<std::ptrdiff_t>(
                                    offset + static_cast<std::size_t>(value_length))),
      });
      previous_id = id;
      offset += static_cast<std::size_t>(value_length);
    }
    return {.frame = std::move(frame)};
  } catch (const std::bad_alloc&) {
    return {.error = PairingError::kLimitExceeded};
  }
}

PairingError ValidateOffer(const NegotiationOffer& offer) {
  if (offer.versions.minimum.major == 0 || offer.versions.maximum.major == 0 ||
      VersionLess(offer.versions.maximum, offer.versions.minimum) ||
      !HasCanonicalCapabilities(offer.offered_capabilities) ||
      !HasCanonicalCapabilities(offer.required_capabilities) ||
      !std::includes(
          offer.offered_capabilities.begin(), offer.offered_capabilities.end(),
          offer.required_capabilities.begin(), offer.required_capabilities.end()) ||
      !std::binary_search(offer.offered_capabilities.begin(),
                          offer.offered_capabilities.end(), kBaseTransferV1) ||
      !std::binary_search(offer.required_capabilities.begin(),
                          offer.required_capabilities.end(), kBaseTransferV1) ||
      offer.receive_limits.max_body < kMinimumReceiveBody ||
      offer.receive_limits.max_body > kMaximumReceiveBody ||
      offer.receive_limits.max_in_flight < kMinimumReceiveInFlight ||
      offer.receive_limits.max_in_flight > kMaximumReceiveInFlight ||
      offer.receive_limits.max_streams == 0 ||
      offer.receive_limits.max_streams > kMaximumReceiveStreams) {
    return PairingError::kMalformed;
  }
  for (const std::uint32_t required : offer.required_capabilities) {
    if (required != kBaseTransferV1 && required != kResumeV1 &&
        required != kParallelFilesV1) {
      return PairingError::kMalformed;
    }
  }
  return PairingError::kNone;
}

PairingError DecodeHello(const Frame& frame, const security::tls::Role expected_role,
                         const security::tls::ValidatedEd25519PublicKey& expected_key,
                         Hello& output) {
  constexpr std::array<WireType, 8> kTypes = {
      WireType::kU8,    WireType::kBytes, WireType::kBytes, WireType::kU16,
      WireType::kBytes, WireType::kBytes, WireType::kBytes, WireType::kBytes,
  };
  if (frame.type != PairingMessageType::kHello || !HasFields(frame, kTypes) ||
      frame.fields[1].value.size() != 32 || frame.fields[2].value.size() != 32 ||
      frame.fields[4].value.size() != 4 || frame.fields[7].value.size() != 10) {
    return PairingError::kMalformed;
  }
  if (frame.fields[0].value[0] != static_cast<std::uint8_t>(expected_role)) {
    return PairingError::kRoleMismatch;
  }
  if (ReadU16(frame.fields[3].value) != security::tls::kSecurityProfileV1) {
    return PairingError::kUnsupportedProfile;
  }
  const auto key = security::tls::ValidateEd25519PublicKey(frame.fields[2].value);
  if (!key.ok() || *key.value != expected_key) {
    return PairingError::kCertificateRejected;
  }

  Hello candidate{};
  candidate.role = expected_role;
  std::copy(frame.fields[1].value.begin(), frame.fields[1].value.end(),
            candidate.nonce.bytes.begin());
  candidate.key = *key.value;
  candidate.offer.versions = {
      .minimum = {frame.fields[4].value[0], frame.fields[4].value[1]},
      .maximum = {frame.fields[4].value[2], frame.fields[4].value[3]},
  };
  PairingError error =
      DecodeCapabilities(frame.fields[5].value, candidate.offer.offered_capabilities);
  if (error != PairingError::kNone) {
    return error;
  }
  error =
      DecodeCapabilities(frame.fields[6].value, candidate.offer.required_capabilities);
  if (error != PairingError::kNone) {
    return error;
  }
  candidate.offer.receive_limits = {
      .max_body =
          ReadU32(std::span<const std::uint8_t>(frame.fields[7].value).subspan(0, 4)),
      .max_in_flight =
          ReadU32(std::span<const std::uint8_t>(frame.fields[7].value).subspan(4, 4)),
      .max_streams =
          ReadU16(std::span<const std::uint8_t>(frame.fields[7].value).subspan(8, 2)),
  };
  error = ValidateOffer(candidate.offer);
  if (error != PairingError::kNone) {
    return error;
  }
  output = std::move(candidate);
  return PairingError::kNone;
}

PairingError Select(const NegotiationOffer& initiator,
                    const NegotiationOffer& responder, Selection& output) {
  const PairingError initiator_error = ValidateOffer(initiator);
  const PairingError responder_error = ValidateOffer(responder);
  if (initiator_error != PairingError::kNone ||
      responder_error != PairingError::kNone) {
    return PairingError::kMalformed;
  }

  const Version minimum =
      VersionLess(initiator.versions.minimum, responder.versions.minimum)
          ? responder.versions.minimum
          : initiator.versions.minimum;
  const Version maximum =
      VersionLess(initiator.versions.maximum, responder.versions.maximum)
          ? initiator.versions.maximum
          : responder.versions.maximum;
  if (VersionLess(maximum, minimum)) {
    return PairingError::kUnsupportedVersion;
  }

  Selection candidate{
      .selected_version = maximum,
      .effective_limits =
          {
              .max_body = std::min(initiator.receive_limits.max_body,
                                   responder.receive_limits.max_body),
              .max_in_flight = std::min(initiator.receive_limits.max_in_flight,
                                        responder.receive_limits.max_in_flight),
              .max_streams = std::min(initiator.receive_limits.max_streams,
                                      responder.receive_limits.max_streams),
          },
  };
  std::set_intersection(
      initiator.offered_capabilities.begin(), initiator.offered_capabilities.end(),
      responder.offered_capabilities.begin(), responder.offered_capabilities.end(),
      std::back_inserter(candidate.selected_capabilities));
  if (!std::includes(candidate.selected_capabilities.begin(),
                     candidate.selected_capabilities.end(),
                     initiator.required_capabilities.begin(),
                     initiator.required_capabilities.end()) ||
      !std::includes(candidate.selected_capabilities.begin(),
                     candidate.selected_capabilities.end(),
                     responder.required_capabilities.begin(),
                     responder.required_capabilities.end()) ||
      !std::binary_search(candidate.selected_capabilities.begin(),
                          candidate.selected_capabilities.end(), kBaseTransferV1)) {
    return PairingError::kMalformed;
  }
  output = std::move(candidate);
  return PairingError::kNone;
}

PairingError DecodeSelection(const Frame& frame, Selection& output) {
  constexpr std::array<WireType, 3> kTypes = {
      WireType::kBytes,
      WireType::kBytes,
      WireType::kBytes,
  };
  if ((frame.type != PairingMessageType::kSelect &&
       frame.type != PairingMessageType::kSelectAck) ||
      !HasFields(frame, kTypes) || frame.fields[0].value.size() != 2 ||
      frame.fields[2].value.size() != 10) {
    return PairingError::kMalformed;
  }
  Selection candidate{
      .selected_version = {frame.fields[0].value[0], frame.fields[0].value[1]},
      .effective_limits =
          {
              .max_body = ReadU32(
                  std::span<const std::uint8_t>(frame.fields[2].value).subspan(0, 4)),
              .max_in_flight = ReadU32(
                  std::span<const std::uint8_t>(frame.fields[2].value).subspan(4, 4)),
              .max_streams = ReadU16(
                  std::span<const std::uint8_t>(frame.fields[2].value).subspan(8, 2)),
          },
  };
  const PairingError error =
      DecodeCapabilities(frame.fields[1].value, candidate.selected_capabilities);
  if (error != PairingError::kNone) {
    return error;
  }
  output = std::move(candidate);
  return PairingError::kNone;
}

PairingError DecodeDecision(const Frame& frame, Decision& output) {
  constexpr std::array<WireType, 2> kTypes = {
      WireType::kBytes,
      WireType::kBytes,
  };
  if (frame.type != PairingMessageType::kDecision || !HasFields(frame, kTypes) ||
      frame.fields[0].value.size() != 34 || frame.fields[1].value.size() != 32) {
    return PairingError::kMalformed;
  }
  std::copy(frame.fields[0].value.begin(), frame.fields[0].value.end(),
            output.message.begin());
  std::copy(frame.fields[1].value.begin(), frame.fields[1].value.end(),
            output.authenticator.begin());
  return PairingError::kNone;
}

PairingError DecodeAbort(const Frame& frame) {
  constexpr std::array<WireType, 1> kTypes = {WireType::kU16};
  if (frame.type != PairingMessageType::kAbort || !HasFields(frame, kTypes)) {
    return PairingError::kMalformed;
  }
  const std::uint16_t code = ReadU16(frame.fields[0].value);
  return code >= 1 && code <= 4 ? PairingError::kNone : PairingError::kMalformed;
}

security::tls::Result<security::tls::NormalizedNegotiation> BuildNormalizedNegotiation(
    const Hello& initiator, const Hello& responder, const Selection& selection) {
  const std::array<CanonicalField, 13> fields = {{
      {1, U8(static_cast<std::uint8_t>(security::tls::Role::kInitiator))},
      {2, EncodeVersionRange(initiator.offer.versions)},
      {3, EncodeCapabilities(initiator.offer.offered_capabilities)},
      {4, EncodeCapabilities(initiator.offer.required_capabilities)},
      {5, EncodeLimits(initiator.offer.receive_limits)},
      {6, U8(static_cast<std::uint8_t>(security::tls::Role::kResponder))},
      {7, EncodeVersionRange(responder.offer.versions)},
      {8, EncodeCapabilities(responder.offer.offered_capabilities)},
      {9, EncodeCapabilities(responder.offer.required_capabilities)},
      {10, EncodeLimits(responder.offer.receive_limits)},
      {11, EncodeVersion(selection.selected_version)},
      {12, EncodeCapabilities(selection.selected_capabilities)},
      {13, EncodeLimits(selection.effective_limits)},
  }};
  return security::tls::DecodeNormalizedNegotiation(EncodeNormalizedObject(fields));
}

Bytes EncodeHello(const std::uint32_t sequence, const security::tls::Role role,
                  const security::tls::ValidatedEd25519PublicKey& key,
                  const security::tls::Nonce256& nonce, const NegotiationOffer& offer) {
  const std::array<Field, 8> fields = {{
      {1, WireType::kU8, U8(static_cast<std::uint8_t>(role))},
      {2, WireType::kBytes, Bytes(nonce.bytes.begin(), nonce.bytes.end())},
      {3, WireType::kBytes, Bytes(key.bytes().begin(), key.bytes().end())},
      {4, WireType::kU16, U16(security::tls::kSecurityProfileV1)},
      {5, WireType::kBytes, EncodeVersionRange(offer.versions)},
      {6, WireType::kBytes, EncodeCapabilities(offer.offered_capabilities)},
      {7, WireType::kBytes, EncodeCapabilities(offer.required_capabilities)},
      {8, WireType::kBytes, EncodeLimits(offer.receive_limits)},
  }};
  return EncodeFrame(PairingMessageType::kHello, sequence, fields);
}

Bytes EncodeSelection(const PairingMessageType type, const std::uint32_t sequence,
                      const Selection& selection) {
  const std::array<Field, 3> fields = {{
      {1, WireType::kBytes, EncodeVersion(selection.selected_version)},
      {2, WireType::kBytes, EncodeCapabilities(selection.selected_capabilities)},
      {3, WireType::kBytes, EncodeLimits(selection.effective_limits)},
  }};
  return EncodeFrame(type, sequence, fields);
}

Bytes EncodeDecision(const std::uint32_t sequence,
                     const security::tls::ConfirmationValue& decision) {
  const std::array<Field, 2> fields = {{
      {1, WireType::kBytes, Bytes(decision.message.begin(), decision.message.end())},
      {2, WireType::kBytes,
       Bytes(decision.authenticator.begin(), decision.authenticator.end())},
  }};
  return EncodeFrame(PairingMessageType::kDecision, sequence, fields);
}

Bytes EncodeAbort(const std::uint32_t sequence, const std::uint16_t public_code) {
  const std::array<Field, 1> fields = {{
      {1, WireType::kU16, U16(public_code)},
  }};
  return EncodeFrame(PairingMessageType::kAbort, sequence, fields);
}

}  // namespace xnn_transfer::core::session::internal
