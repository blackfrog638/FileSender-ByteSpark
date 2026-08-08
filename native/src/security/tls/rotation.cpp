#include <array>
#include <cstdint>
#include <span>
#include <utility>

#include "internal.hpp"

namespace xnn_transfer::core::security::tls::internal {

SecurityError ValidateRotationContextObject(const ParsedObject& object) {
  const auto field = [&object](const std::uint16_t id) {
    return std::span<const std::uint8_t>(FindField(object, id)->value);
  };
  if (!EqualBytes(field(1), AsBytes(kRotationContextLabel))) {
    return SecurityError::kDomainMismatch;
  }
  if (EqualBytes(field(2), field(3)) || ReadU64(field(4)) == 0) {
    return SecurityError::kInvalidRotation;
  }
  SecurityError error = ValidateKeyField(object, 2);
  if (error != SecurityError::kNone) {
    return error;
  }
  return ValidateKeyField(object, 3);
}

SecurityError ValidateRotationProofObject(const ParsedObject& object) {
  const auto field = [&object](const std::uint16_t id) {
    return std::span<const std::uint8_t>(FindField(object, id)->value);
  };
  if (!EqualBytes(field(1), AsBytes(kRotationProofLabel))) {
    return SecurityError::kDomainMismatch;
  }
  return field(3)[0] == static_cast<std::uint8_t>(RotationSigner::kOldKey) ||
                 field(3)[0] == static_cast<std::uint8_t>(RotationSigner::kNewKey)
             ? SecurityError::kNone
             : SecurityError::kRoleMismatch;
}

}  // namespace xnn_transfer::core::security::tls::internal

namespace xnn_transfer::core::security::tls {

Result<RotationContext> BuildRotationContext(const RotationContextInput& input) {
  if (input.old_key == input.new_key || input.counter == 0) {
    return {.error = SecurityError::kInvalidRotation};
  }
  const std::array<std::uint8_t, 8> counter = internal::EncodeU64(input.counter);
  const std::array<internal::FieldInput, 6> fields = {{
      {1, internal::AsBytes(internal::kRotationContextLabel)},
      {2, input.old_key.bytes()},
      {3, input.new_key.bytes()},
      {4, counter},
      {5, input.nonce.bytes},
      {6, input.transport_context.digest()},
  }};
  Result<Bytes> encoded =
      internal::EncodeObject(CanonicalObjectKind::kRotationContext, fields);
  if (!encoded.ok()) {
    return {.error = encoded.error};
  }
  const Result<Digest256> digest = Sha256(*encoded.value);
  if (!digest.ok()) {
    return {.error = digest.error};
  }
  return {
      .value = RotationContext(std::move(*encoded.value), *digest.value),
      .error = SecurityError::kNone,
  };
}

Result<CanonicalObject> BuildRotationProofInput(const RotationContext& context,
                                                const RotationSigner signer) {
  if (signer != RotationSigner::kOldKey && signer != RotationSigner::kNewKey) {
    return {.error = SecurityError::kRoleMismatch};
  }
  const std::array<std::uint8_t, 1> signer_value = {static_cast<std::uint8_t>(signer)};
  const std::array<internal::FieldInput, 3> fields = {{
      {1, internal::AsBytes(internal::kRotationProofLabel)},
      {2, context.digest()},
      {3, signer_value},
  }};
  Result<Bytes> encoded =
      internal::EncodeObject(CanonicalObjectKind::kRotationProof, fields);
  if (!encoded.ok()) {
    return {.error = encoded.error};
  }
  return {
      .value =
          CanonicalObject{
              .kind = CanonicalObjectKind::kRotationProof,
              .encoded = std::move(*encoded.value),
          },
      .error = SecurityError::kNone,
  };
}

SecurityError ValidateRotationCounter(const std::uint64_t previous,
                                      const std::uint64_t current) noexcept {
  return current > previous ? SecurityError::kNone : SecurityError::kReplayDetected;
}

}  // namespace xnn_transfer::core::security::tls
