#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

#include "internal.hpp"

namespace xnn_transfer::core::security::tls::internal {
namespace {

constexpr std::uint32_t kBaseTransferV1 = 0x0001'0001U;

[[nodiscard]] SecurityError DecodeCapabilities(
    const std::span<const std::uint8_t> encoded, std::vector<std::uint32_t>& output) {
  if (encoded.size() < 2) {
    return SecurityError::kInvalidLength;
  }
  const std::uint16_t count = ReadU16(encoded, 0);
  if (encoded.size() != 2U + static_cast<std::size_t>(count) * 4U) {
    return SecurityError::kInvalidLength;
  }

  output.clear();
  output.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    output.push_back(ReadU32(encoded, 2 + index * 4));
  }
  if (!std::is_sorted(output.begin(), output.end()) ||
      std::adjacent_find(output.begin(), output.end()) != output.end()) {
    return SecurityError::kNonCanonicalEncoding;
  }
  for (std::size_t index = 1; index < output.size(); ++index) {
    if ((output[index - 1] >> 16U) == (output[index] >> 16U)) {
      return SecurityError::kNonCanonicalEncoding;
    }
  }
  return SecurityError::kNone;
}

using Version = std::array<std::uint8_t, 2>;

[[nodiscard]] bool VersionLess(const Version& left, const Version& right) noexcept {
  return left[0] < right[0] || (left[0] == right[0] && left[1] < right[1]);
}

struct VersionRange {
  Version minimum{};
  Version maximum{};
};

[[nodiscard]] SecurityError DecodeVersionRange(
    const std::span<const std::uint8_t> encoded, VersionRange& output) noexcept {
  output = {
      .minimum = {encoded[0], encoded[1]},
      .maximum = {encoded[2], encoded[3]},
  };
  if (output.minimum[0] == 0 || output.maximum[0] == 0 ||
      VersionLess(output.maximum, output.minimum)) {
    return SecurityError::kInvalidNegotiation;
  }
  return SecurityError::kNone;
}

using ReceiveLimits = std::array<std::uint32_t, 3>;

[[nodiscard]] ReceiveLimits DecodeLimits(
    const std::span<const std::uint8_t> encoded) noexcept {
  return {
      ReadU32(encoded, 0),
      ReadU32(encoded, 4),
      ReadU16(encoded, 8),
  };
}

[[nodiscard]] SecurityError ValidateRawTranscript(
    const std::span<const std::uint8_t> encoded) noexcept {
  constexpr std::array<std::uint8_t, 4> kExpectedTags = {1, 2, 3, 4};
  std::size_t offset = 0;
  std::size_t tag_index = 0;
  while (offset < encoded.size()) {
    if (encoded.size() - offset < 5 || tag_index >= kExpectedTags.size() ||
        encoded[offset] != kExpectedTags[tag_index]) {
      return SecurityError::kMalformedTranscript;
    }
    const std::uint32_t frame_length = ReadU32(encoded, offset + 1);
    offset += 5;
    if (frame_length == 0 ||
        static_cast<std::size_t>(frame_length) > encoded.size() - offset) {
      return SecurityError::kMalformedTranscript;
    }
    offset += static_cast<std::size_t>(frame_length);
    ++tag_index;
  }
  return tag_index == kExpectedTags.size() ? SecurityError::kNone
                                           : SecurityError::kMalformedTranscript;
}

}  // namespace

SecurityError ValidateNegotiationObject(const ParsedObject& object) {
  const auto field = [&object](const std::uint16_t id) {
    return std::span<const std::uint8_t>(FindField(object, id)->value);
  };
  if (field(1)[0] != static_cast<std::uint8_t>(Role::kInitiator) ||
      field(6)[0] != static_cast<std::uint8_t>(Role::kResponder)) {
    return SecurityError::kRoleMismatch;
  }

  VersionRange initiator_range{};
  VersionRange responder_range{};
  SecurityError error = DecodeVersionRange(field(2), initiator_range);
  if (error != SecurityError::kNone) {
    return error;
  }
  error = DecodeVersionRange(field(7), responder_range);
  if (error != SecurityError::kNone) {
    return error;
  }
  const Version common_minimum =
      VersionLess(initiator_range.minimum, responder_range.minimum)
          ? responder_range.minimum
          : initiator_range.minimum;
  const Version common_maximum =
      VersionLess(initiator_range.maximum, responder_range.maximum)
          ? initiator_range.maximum
          : responder_range.maximum;
  if (VersionLess(common_maximum, common_minimum)) {
    return SecurityError::kUnsupportedVersion;
  }
  const Version selected = {field(11)[0], field(11)[1]};
  if (selected != common_maximum) {
    return SecurityError::kDowngradeDetected;
  }

  std::vector<std::uint32_t> initiator_capabilities;
  std::vector<std::uint32_t> initiator_required;
  std::vector<std::uint32_t> responder_capabilities;
  std::vector<std::uint32_t> responder_required;
  std::vector<std::uint32_t> selected_capabilities;
  for (const auto [id, output] :
       std::array<std::pair<std::uint16_t, std::vector<std::uint32_t>*>, 5>{{
           {3, &initiator_capabilities},
           {4, &initiator_required},
           {8, &responder_capabilities},
           {9, &responder_required},
           {12, &selected_capabilities},
       }}) {
    error = DecodeCapabilities(field(id), *output);
    if (error != SecurityError::kNone) {
      return error;
    }
  }

  std::vector<std::uint32_t> expected_capabilities;
  std::set_intersection(initiator_capabilities.begin(), initiator_capabilities.end(),
                        responder_capabilities.begin(), responder_capabilities.end(),
                        std::back_inserter(expected_capabilities));
  const bool base_is_supported =
      std::binary_search(initiator_capabilities.begin(), initiator_capabilities.end(),
                         kBaseTransferV1) &&
      std::binary_search(responder_capabilities.begin(), responder_capabilities.end(),
                         kBaseTransferV1);
  const bool base_is_required =
      std::binary_search(initiator_required.begin(), initiator_required.end(),
                         kBaseTransferV1) ||
      std::binary_search(responder_required.begin(), responder_required.end(),
                         kBaseTransferV1);
  if (!base_is_supported || !base_is_required) {
    return SecurityError::kUnsupportedCapability;
  }
  for (const std::uint32_t required : initiator_required) {
    if (!std::binary_search(expected_capabilities.begin(), expected_capabilities.end(),
                            required)) {
      return SecurityError::kUnsupportedCapability;
    }
  }
  for (const std::uint32_t required : responder_required) {
    if (!std::binary_search(expected_capabilities.begin(), expected_capabilities.end(),
                            required)) {
      return SecurityError::kUnsupportedCapability;
    }
  }
  if (selected_capabilities != expected_capabilities) {
    return SecurityError::kDowngradeDetected;
  }

  const ReceiveLimits initiator_limits = DecodeLimits(field(5));
  const ReceiveLimits responder_limits = DecodeLimits(field(10));
  const ReceiveLimits selected_limits = DecodeLimits(field(13));
  for (std::size_t index = 0; index < selected_limits.size(); ++index) {
    if (selected_limits[index] !=
        std::min(initiator_limits[index], responder_limits[index])) {
      return SecurityError::kDowngradeDetected;
    }
  }
  return SecurityError::kNone;
}

SecurityError ValidatePairingObject(const ParsedObject& object) {
  const auto field = [&object](const std::uint16_t id) {
    return std::span<const std::uint8_t>(FindField(object, id)->value);
  };
  if (!EqualBytes(field(1), AsBytes(kPairContextLabel))) {
    return SecurityError::kDomainMismatch;
  }
  if (field(2)[0] != static_cast<std::uint8_t>(Role::kInitiator) ||
      field(3)[0] != static_cast<std::uint8_t>(Role::kResponder)) {
    return SecurityError::kRoleMismatch;
  }
  if (ReadU16(field(8), 0) != kSecurityProfileV1) {
    return SecurityError::kUnsupportedProfile;
  }
  const ParsedResult negotiation =
      ParseObject(field(9), CanonicalObjectKind::kNormalizedNegotiation);
  if (!negotiation.ok()) {
    return negotiation.error;
  }
  SecurityError error = ValidateKeyField(object, 6);
  if (error != SecurityError::kNone) {
    return error;
  }
  return ValidateKeyField(object, 7);
}

SecurityError ValidateTransportObject(const ParsedObject& object) {
  const auto field = [&object](const std::uint16_t id) {
    return std::span<const std::uint8_t>(FindField(object, id)->value);
  };
  if (!EqualBytes(field(1), AsBytes(kTransportContextLabel))) {
    return SecurityError::kDomainMismatch;
  }
  if (field(2)[0] != static_cast<std::uint8_t>(Role::kInitiator) ||
      field(3)[0] != static_cast<std::uint8_t>(Role::kResponder)) {
    return SecurityError::kRoleMismatch;
  }
  if (ReadU16(field(8), 0) != kSecurityProfileV1) {
    return SecurityError::kUnsupportedProfile;
  }
  const ParsedResult negotiation =
      ParseObject(field(9), CanonicalObjectKind::kNormalizedNegotiation);
  if (!negotiation.ok()) {
    return negotiation.error;
  }
  SecurityError error = ValidateRawTranscript(field(10));
  if (error != SecurityError::kNone) {
    return error;
  }
  error = ValidateKeyField(object, 4);
  if (error != SecurityError::kNone) {
    return error;
  }
  return ValidateKeyField(object, 5);
}

}  // namespace xnn_transfer::core::security::tls::internal

namespace xnn_transfer::core::security::tls {

Result<PairingContext> BuildPairingContext(const PairingContextInput& input) {
  constexpr std::array<std::uint8_t, 1> kInitiatorRole = {
      static_cast<std::uint8_t>(Role::kInitiator)};
  constexpr std::array<std::uint8_t, 1> kResponderRole = {
      static_cast<std::uint8_t>(Role::kResponder)};
  constexpr std::array<std::uint8_t, 2> kProfile = {0, 1};
  const std::array<internal::FieldInput, 9> fields = {{
      {1, internal::AsBytes(internal::kPairContextLabel)},
      {2, kInitiatorRole},
      {3, kResponderRole},
      {4, input.initiator_nonce.bytes},
      {5, input.responder_nonce.bytes},
      {6, input.initiator_key.bytes()},
      {7, input.responder_key.bytes()},
      {8, kProfile},
      {9, input.negotiation.encoded()},
  }};
  Result<Bytes> encoded =
      internal::EncodeObject(CanonicalObjectKind::kPairingContext, fields);
  if (!encoded.ok()) {
    return {.error = encoded.error};
  }
  const Result<Digest256> digest = Sha256(*encoded.value);
  if (!digest.ok()) {
    return {.error = digest.error};
  }
  return {
      .value = PairingContext(std::move(*encoded.value), *digest.value),
      .error = SecurityError::kNone,
  };
}

Result<TransportContext> BuildTransportContext(const TransportContextInput& input) {
  constexpr std::array<std::uint8_t, 1> kInitiatorRole = {
      static_cast<std::uint8_t>(Role::kInitiator)};
  constexpr std::array<std::uint8_t, 1> kResponderRole = {
      static_cast<std::uint8_t>(Role::kResponder)};
  constexpr std::array<std::uint8_t, 2> kProfile = {0, 1};
  const std::array<internal::FieldInput, 11> fields = {{
      {1, internal::AsBytes(internal::kTransportContextLabel)},
      {2, kInitiatorRole},
      {3, kResponderRole},
      {4, input.initiator_key.bytes()},
      {5, input.responder_key.bytes()},
      {6, input.initiator_nonce.bytes},
      {7, input.responder_nonce.bytes},
      {8, kProfile},
      {9, input.negotiation.encoded()},
      {10, input.raw_negotiation_transcript},
      {11, input.session_id.bytes},
  }};
  Result<Bytes> encoded =
      internal::EncodeObject(CanonicalObjectKind::kTransportContext, fields);
  if (!encoded.ok()) {
    return {.error = encoded.error};
  }
  const Result<Digest256> digest = Sha256(*encoded.value);
  if (!digest.ok()) {
    return {.error = digest.error};
  }
  return {
      .value = TransportContext(std::move(*encoded.value), *digest.value),
      .error = SecurityError::kNone,
  };
}

std::string_view ExporterLabel(PairingExporterLabel) noexcept {
  return internal::kPairExporterLabel;
}

std::string_view ExporterLabel(ConfirmationExporterLabel) noexcept {
  return internal::kConfirmationExporterLabel;
}

std::string_view ExporterLabel(TransportExporterLabel) noexcept {
  return internal::kTransportExporterLabel;
}

PairingExporterInput MakePairingExporterInput(const PairingContext& context) noexcept {
  return {.context = context.digest()};
}

ConfirmationExporterInput MakeConfirmationExporterInput(
    const PairingContext& context) noexcept {
  return {.context = context.digest()};
}

TransportExporterInput MakeTransportExporterInput(
    const TransportContext& context) noexcept {
  return {.context = context.digest()};
}

}  // namespace xnn_transfer::core::security::tls
