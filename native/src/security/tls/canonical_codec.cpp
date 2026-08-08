#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "internal.hpp"

namespace xnn_transfer::core::security::tls::internal {
namespace {

constexpr std::array<std::uint8_t, 4> kCanonicalMagic = {'X', 'N', 'N', 'S'};
constexpr std::uint8_t kCanonicalVersion = 1;

struct FieldSchema {
  std::uint16_t id;
  std::size_t fixed_size;
  bool has_fixed_size;
};

constexpr std::array<FieldSchema, 13> kNegotiationSchema = {{
    {1, 1, true},
    {2, 4, true},
    {3, 0, false},
    {4, 0, false},
    {5, 10, true},
    {6, 1, true},
    {7, 4, true},
    {8, 0, false},
    {9, 0, false},
    {10, 10, true},
    {11, 2, true},
    {12, 0, false},
    {13, 10, true},
}};

constexpr std::array<FieldSchema, 9> kPairContextSchema = {{
    {1, kPairContextLabel.size(), true},
    {2, 1, true},
    {3, 1, true},
    {4, 32, true},
    {5, 32, true},
    {6, 32, true},
    {7, 32, true},
    {8, 2, true},
    {9, 0, false},
}};

constexpr std::array<FieldSchema, 2> kSasInformationSchema = {{
    {1, kSasInformationLabel.size(), true},
    {2, 32, true},
}};

constexpr std::array<FieldSchema, 11> kTransportContextSchema = {{
    {1, kTransportContextLabel.size(), true},
    {2, 1, true},
    {3, 1, true},
    {4, 32, true},
    {5, 32, true},
    {6, 32, true},
    {7, 32, true},
    {8, 2, true},
    {9, 0, false},
    {10, 0, false},
    {11, 16, true},
}};

constexpr std::array<FieldSchema, 6> kRotationContextSchema = {{
    {1, kRotationContextLabel.size(), true},
    {2, 32, true},
    {3, 32, true},
    {4, 8, true},
    {5, 32, true},
    {6, 32, true},
}};

constexpr std::array<FieldSchema, 3> kRotationProofSchema = {{
    {1, kRotationProofLabel.size(), true},
    {2, 32, true},
    {3, 1, true},
}};

constexpr std::array<FieldSchema, 2> kDeviceIdentifierSchema = {{
    {1, kDeviceIdentifierLabel.size(), true},
    {2, 32, true},
}};

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

[[nodiscard]] std::span<const FieldSchema> SchemaForKind(
    const CanonicalObjectKind kind) noexcept {
  switch (kind) {
    case CanonicalObjectKind::kNormalizedNegotiation:
      return kNegotiationSchema;
    case CanonicalObjectKind::kPairingContext:
      return kPairContextSchema;
    case CanonicalObjectKind::kSasInformation:
      return kSasInformationSchema;
    case CanonicalObjectKind::kTransportContext:
      return kTransportContextSchema;
    case CanonicalObjectKind::kRotationContext:
      return kRotationContextSchema;
    case CanonicalObjectKind::kRotationProof:
      return kRotationProofSchema;
    case CanonicalObjectKind::kDeviceIdentifier:
      return kDeviceIdentifierSchema;
  }
  return {};
}

[[nodiscard]] SecurityError ValidateSemanticObject(const ParsedObject& object) {
  switch (object.kind) {
    case CanonicalObjectKind::kNormalizedNegotiation:
      return ValidateNegotiationObject(object);
    case CanonicalObjectKind::kPairingContext:
      return ValidatePairingObject(object);
    case CanonicalObjectKind::kSasInformation:
      return ValidateSasInformationObject(object);
    case CanonicalObjectKind::kTransportContext:
      return ValidateTransportObject(object);
    case CanonicalObjectKind::kRotationContext:
      return ValidateRotationContextObject(object);
    case CanonicalObjectKind::kRotationProof:
      return ValidateRotationProofObject(object);
    case CanonicalObjectKind::kDeviceIdentifier:
      return ValidateDeviceIdentifierObject(object);
  }
  return SecurityError::kUnknownKind;
}

}  // namespace

std::span<const std::uint8_t> AsBytes(const std::string_view value) noexcept {
  return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

bool EqualBytes(const std::span<const std::uint8_t> left,
                const std::span<const std::uint8_t> right) noexcept {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin());
}

std::uint16_t ReadU16(const std::span<const std::uint8_t> encoded,
                      const std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(encoded[offset]) << 8U) |
      static_cast<std::uint16_t>(encoded[offset + 1]));
}

std::uint32_t ReadU32(const std::span<const std::uint8_t> encoded,
                      const std::size_t offset) noexcept {
  return (static_cast<std::uint32_t>(encoded[offset]) << 24U) |
         (static_cast<std::uint32_t>(encoded[offset + 1]) << 16U) |
         (static_cast<std::uint32_t>(encoded[offset + 2]) << 8U) |
         static_cast<std::uint32_t>(encoded[offset + 3]);
}

std::uint64_t ReadU64(const std::span<const std::uint8_t> encoded) noexcept {
  std::uint64_t value = 0;
  for (const std::uint8_t byte : encoded) {
    value = (value << 8U) | byte;
  }
  return value;
}

std::array<std::uint8_t, 8> EncodeU64(const std::uint64_t value) noexcept {
  std::array<std::uint8_t, 8> encoded{};
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    const auto shift = static_cast<unsigned>((encoded.size() - index - 1) * 8);
    encoded[index] = static_cast<std::uint8_t>(value >> shift);
  }
  return encoded;
}

const ParsedField* FindField(const ParsedObject& object,
                             const std::uint16_t id) noexcept {
  const auto iterator =
      std::lower_bound(object.fields.begin(), object.fields.end(), id,
                       [](const ParsedField& field, const std::uint16_t expected) {
                         return field.id < expected;
                       });
  if (iterator == object.fields.end() || iterator->id != id) {
    return nullptr;
  }
  return &*iterator;
}

ParsedResult ParseObject(const std::span<const std::uint8_t> encoded,
                         const std::optional<CanonicalObjectKind> expected_kind) {
  if (encoded.size() < 12) {
    return {.error = SecurityError::kMalformedEncoding};
  }
  if (!EqualBytes(encoded.first(kCanonicalMagic.size()), kCanonicalMagic)) {
    return {.error = SecurityError::kMalformedEncoding};
  }
  if (encoded[4] != kCanonicalVersion) {
    return {.error = SecurityError::kUnsupportedVersion};
  }

  const std::uint8_t raw_kind = encoded[5];
  if (raw_kind <
          static_cast<std::uint8_t>(CanonicalObjectKind::kNormalizedNegotiation) ||
      raw_kind > static_cast<std::uint8_t>(CanonicalObjectKind::kDeviceIdentifier)) {
    return {.error = SecurityError::kUnknownKind};
  }
  const auto kind = static_cast<CanonicalObjectKind>(raw_kind);
  if (expected_kind.has_value() && kind != *expected_kind) {
    return {.error = SecurityError::kDomainMismatch};
  }

  const std::uint16_t field_count = ReadU16(encoded, 6);
  const std::uint32_t body_length = ReadU32(encoded, 8);
  if (field_count > kMaximumCanonicalFields ||
      body_length > kMaximumCanonicalBodySize) {
    return {.error = SecurityError::kLimitExceeded};
  }
  if (encoded.size() != 12U + static_cast<std::size_t>(body_length)) {
    return {.error = SecurityError::kMalformedEncoding};
  }

  const std::span<const FieldSchema> schema = SchemaForKind(kind);
  ParsedObject object{
      .kind = kind,
      .encoded = Bytes(encoded.begin(), encoded.end()),
      .fields = {},
  };
  object.fields.reserve(field_count);

  std::size_t offset = 12;
  std::uint16_t previous_id = 0;
  for (std::uint16_t index = 0; index < field_count; ++index) {
    if (encoded.size() - offset < 6) {
      return {.error = SecurityError::kMalformedEncoding};
    }
    const std::uint16_t field_id = ReadU16(encoded, offset);
    const std::uint32_t value_length = ReadU32(encoded, offset + 2);
    offset += 6;

    if (field_id == previous_id) {
      return {.error = SecurityError::kDuplicateField};
    }
    if (field_id < previous_id) {
      return {.error = SecurityError::kNonCanonicalEncoding};
    }
    const auto schema_field = std::find_if(
        schema.begin(), schema.end(),
        [field_id](const FieldSchema& entry) { return entry.id == field_id; });
    if (schema_field == schema.end()) {
      return {.error = SecurityError::kUnknownField};
    }
    if (static_cast<std::size_t>(value_length) > encoded.size() - offset) {
      return {.error = SecurityError::kMalformedEncoding};
    }
    if (schema_field->has_fixed_size && value_length != schema_field->fixed_size) {
      return {.error = SecurityError::kInvalidLength};
    }

    const std::size_t end = offset + static_cast<std::size_t>(value_length);
    object.fields.push_back(ParsedField{
        .id = field_id,
        .value = Bytes(encoded.begin() + static_cast<std::ptrdiff_t>(offset),
                       encoded.begin() + static_cast<std::ptrdiff_t>(end))});
    offset = end;
    previous_id = field_id;
  }

  if (offset != encoded.size()) {
    return {.error = SecurityError::kTrailingData};
  }
  for (const FieldSchema& required : schema) {
    if (FindField(object, required.id) == nullptr) {
      return {.error = SecurityError::kMissingField};
    }
  }

  const SecurityError semantic_error = ValidateSemanticObject(object);
  if (semantic_error != SecurityError::kNone) {
    return {.error = semantic_error};
  }
  return {.value = std::move(object), .error = SecurityError::kNone};
}

Result<Bytes> EncodeObject(const CanonicalObjectKind kind,
                           const std::span<const FieldInput> fields) {
  if (fields.size() > kMaximumCanonicalFields) {
    return {.error = SecurityError::kLimitExceeded};
  }

  std::size_t body_length = 0;
  std::uint16_t previous_id = 0;
  for (const FieldInput& field : fields) {
    if (field.id <= previous_id) {
      return {.error = SecurityError::kNonCanonicalEncoding};
    }
    if (field.value.size() > std::numeric_limits<std::uint32_t>::max() ||
        body_length > kMaximumCanonicalBodySize - 6 ||
        field.value.size() > kMaximumCanonicalBodySize - body_length - 6) {
      return {.error = SecurityError::kLimitExceeded};
    }
    body_length += 6 + field.value.size();
    previous_id = field.id;
  }

  Bytes encoded;
  encoded.reserve(12 + body_length);
  encoded.insert(encoded.end(), kCanonicalMagic.begin(), kCanonicalMagic.end());
  encoded.push_back(kCanonicalVersion);
  encoded.push_back(static_cast<std::uint8_t>(kind));
  AppendU16(encoded, static_cast<std::uint16_t>(fields.size()));
  AppendU32(encoded, static_cast<std::uint32_t>(body_length));
  for (const FieldInput& field : fields) {
    AppendU16(encoded, field.id);
    AppendU32(encoded, static_cast<std::uint32_t>(field.value.size()));
    encoded.insert(encoded.end(), field.value.begin(), field.value.end());
  }

  const ParsedResult parsed = ParseObject(encoded, kind);
  if (!parsed.ok()) {
    return {.error = parsed.error};
  }
  return {.value = std::move(encoded), .error = SecurityError::kNone};
}

SecurityError ValidateKeyField(const ParsedObject& object, const std::uint16_t id) {
  const auto result = ValidateEd25519PublicKey(FindField(object, id)->value);
  return result.ok() ? SecurityError::kNone : result.error;
}

}  // namespace xnn_transfer::core::security::tls::internal

namespace xnn_transfer::core::security::tls {

std::string_view SecurityErrorName(const SecurityError error) noexcept {
  switch (error) {
    case SecurityError::kNone:
      return "NONE";
    case SecurityError::kCryptoFailure:
      return "CRYPTO_FAILURE";
    case SecurityError::kInvalidLength:
      return "INVALID_LENGTH";
    case SecurityError::kMalformedEncoding:
      return "MALFORMED_ENCODING";
    case SecurityError::kNonCanonicalEncoding:
      return "NON_CANONICAL_ENCODING";
    case SecurityError::kUnsupportedVersion:
      return "UNSUPPORTED_VERSION";
    case SecurityError::kUnknownKind:
      return "UNKNOWN_KIND";
    case SecurityError::kUnknownField:
      return "UNKNOWN_FIELD";
    case SecurityError::kDuplicateField:
      return "DUPLICATE_FIELD";
    case SecurityError::kMissingField:
      return "MISSING_FIELD";
    case SecurityError::kTrailingData:
      return "TRAILING_DATA";
    case SecurityError::kLimitExceeded:
      return "LIMIT_EXCEEDED";
    case SecurityError::kDomainMismatch:
      return "DOMAIN_MISMATCH";
    case SecurityError::kRoleMismatch:
      return "ROLE_MISMATCH";
    case SecurityError::kInvalidPublicKey:
      return "INVALID_PUBLIC_KEY";
    case SecurityError::kUnsupportedProfile:
      return "UNSUPPORTED_PROFILE";
    case SecurityError::kInvalidNegotiation:
      return "INVALID_NEGOTIATION";
    case SecurityError::kUnsupportedCapability:
      return "UNSUPPORTED_CAPABILITY";
    case SecurityError::kDowngradeDetected:
      return "DOWNGRADE_DETECTED";
    case SecurityError::kMalformedTranscript:
      return "MALFORMED_TRANSCRIPT";
    case SecurityError::kContextMismatch:
      return "CONTEXT_MISMATCH";
    case SecurityError::kConfirmationMismatch:
      return "CONFIRMATION_MISMATCH";
    case SecurityError::kInvalidDecision:
      return "INVALID_DECISION";
    case SecurityError::kAuthenticatedReject:
      return "AUTHENTICATED_REJECT";
    case SecurityError::kLocalReject:
      return "LOCAL_REJECT";
    case SecurityError::kDeviceIdentifierMismatch:
      return "DEVICE_IDENTIFIER_MISMATCH";
    case SecurityError::kTransportFinishedMismatch:
      return "TRANSPORT_FINISHED_MISMATCH";
    case SecurityError::kInvalidRotation:
      return "INVALID_ROTATION";
    case SecurityError::kReplayDetected:
      return "REPLAY_DETECTED";
    case SecurityError::kOutputMismatch:
      return "OUTPUT_MISMATCH";
    case SecurityError::kIdentityUnavailable:
      return "IDENTITY_UNAVAILABLE";
    case SecurityError::kTlsConfigurationFailure:
      return "TLS_CONFIGURATION_FAILURE";
    case SecurityError::kHandshakeIncomplete:
      return "HANDSHAKE_INCOMPLETE";
    case SecurityError::kTlsVersionMismatch:
      return "TLS_VERSION_MISMATCH";
    case SecurityError::kCipherMismatch:
      return "CIPHER_MISMATCH";
    case SecurityError::kGroupMismatch:
      return "GROUP_MISMATCH";
    case SecurityError::kSignatureMismatch:
      return "SIGNATURE_MISMATCH";
    case SecurityError::kAlpnMismatch:
      return "ALPN_MISMATCH";
    case SecurityError::kPeerCertificateMismatch:
      return "PEER_CERTIFICATE_MISMATCH";
    case SecurityError::kPinMismatch:
      return "PIN_MISMATCH";
    case SecurityError::kResumptionDetected:
      return "RESUMPTION_DETECTED";
    case SecurityError::kEarlyDataDetected:
      return "EARLY_DATA_DETECTED";
    case SecurityError::kExporterFailure:
      return "EXPORTER_FAILURE";
  }
  return "UNKNOWN_ERROR";
}

Result<CanonicalObject> DecodeCanonicalObject(
    const std::span<const std::uint8_t> encoded) {
  internal::ParsedResult parsed = internal::ParseObject(encoded);
  if (!parsed.ok()) {
    return {.error = parsed.error};
  }
  return {
      .value =
          CanonicalObject{
              .kind = parsed.value->kind,
              .encoded = std::move(parsed.value->encoded),
          },
      .error = SecurityError::kNone,
  };
}

Result<CanonicalObject> DecodeCanonicalObject(
    const std::span<const std::uint8_t> encoded,
    const CanonicalObjectKind expected_kind) {
  internal::ParsedResult parsed = internal::ParseObject(encoded, expected_kind);
  if (!parsed.ok()) {
    return {.error = parsed.error};
  }
  return {
      .value =
          CanonicalObject{
              .kind = parsed.value->kind,
              .encoded = std::move(parsed.value->encoded),
          },
      .error = SecurityError::kNone,
  };
}

Result<NormalizedNegotiation> DecodeNormalizedNegotiation(
    const std::span<const std::uint8_t> encoded) {
  Result<CanonicalObject> decoded =
      DecodeCanonicalObject(encoded, CanonicalObjectKind::kNormalizedNegotiation);
  if (!decoded.ok()) {
    return {.error = decoded.error};
  }
  return {
      .value = NormalizedNegotiation(std::move(decoded.value->encoded)),
      .error = SecurityError::kNone,
  };
}

Result<bool> VerifyCanonicalDigest(
    const std::span<const std::uint8_t> encoded,
    const CanonicalObjectKind expected_kind,
    const std::span<const std::uint8_t> expected_digest) {
  if (expected_digest.size() != kSha256Size) {
    return {.error = SecurityError::kInvalidLength};
  }
  const Result<CanonicalObject> decoded = DecodeCanonicalObject(encoded, expected_kind);
  if (!decoded.ok()) {
    return {.error = decoded.error};
  }
  const Result<Digest256> digest = Sha256(decoded.value->encoded);
  if (!digest.ok()) {
    return {.error = digest.error};
  }
  if (!internal::ConstantTimeEqual(*digest.value, expected_digest)) {
    return {.error = SecurityError::kContextMismatch};
  }
  return {.value = true, .error = SecurityError::kNone};
}

}  // namespace xnn_transfer::core::security::tls
