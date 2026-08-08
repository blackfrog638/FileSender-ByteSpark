#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "internal.hpp"

namespace xnn_transfer::core::security::tls::internal {

SecurityError ValidateDeviceIdentifierObject(const ParsedObject& object) {
  if (!EqualBytes(FindField(object, 1)->value, AsBytes(kDeviceIdentifierLabel))) {
    return SecurityError::kDomainMismatch;
  }
  return ValidateKeyField(object, 2);
}

}  // namespace xnn_transfer::core::security::tls::internal

namespace xnn_transfer::core::security::tls {
namespace {

[[nodiscard]] std::string LowercaseHex(const std::span<const std::uint8_t> value) {
  constexpr std::string_view kDigits = "0123456789abcdef";
  std::string result;
  result.resize(value.size() * 2);
  for (std::size_t index = 0; index < value.size(); ++index) {
    result[index * 2] = kDigits[value[index] >> 4U];
    result[index * 2 + 1] = kDigits[value[index] & 0x0fU];
  }
  return result;
}

}  // namespace

Result<DeviceIdentifier> DeriveDeviceIdentifier(
    const ValidatedEd25519PublicKey& public_key) {
  const std::array<internal::FieldInput, 2> fields = {{
      {1, internal::AsBytes(internal::kDeviceIdentifierLabel)},
      {2, public_key.bytes()},
  }};
  Result<Bytes> encoded =
      internal::EncodeObject(CanonicalObjectKind::kDeviceIdentifier, fields);
  if (!encoded.ok()) {
    return {.error = encoded.error};
  }
  return DeriveDeviceIdentifier(*encoded.value);
}

Result<DeviceIdentifier> DeriveDeviceIdentifier(
    const std::span<const std::uint8_t> canonical_input) {
  Result<CanonicalObject> decoded =
      DecodeCanonicalObject(canonical_input, CanonicalObjectKind::kDeviceIdentifier);
  if (!decoded.ok()) {
    return {.error = decoded.error};
  }
  const Result<Digest256> digest = Sha256(decoded.value->encoded);
  if (!digest.ok()) {
    return {.error = digest.error};
  }
  return {
      .value =
          DeviceIdentifier{
              .input = std::move(*decoded.value),
              .digest = *digest.value,
              .text = LowercaseHex(*digest.value),
          },
      .error = SecurityError::kNone,
  };
}

Result<bool> VerifyDeviceIdentifier(
    const ValidatedEd25519PublicKey& public_key,
    const std::span<const std::uint8_t> expected_identifier) {
  if (expected_identifier.size() != kSha256Size) {
    return {.error = SecurityError::kInvalidLength};
  }
  const Result<DeviceIdentifier> actual = DeriveDeviceIdentifier(public_key);
  if (!actual.ok()) {
    return {.error = actual.error};
  }
  if (!internal::ConstantTimeEqual(actual.value->digest, expected_identifier)) {
    return {.error = SecurityError::kDeviceIdentifierMismatch};
  }
  return {.value = true, .error = SecurityError::kNone};
}

Result<bool> VerifyDeviceIdentifierText(const ValidatedEd25519PublicKey& public_key,
                                        const std::string_view presented) {
  if (presented.size() != kSha256Size * 2 ||
      std::any_of(presented.begin(), presented.end(), [](const char value) {
        return !((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'));
      })) {
    return {.error = SecurityError::kNonCanonicalEncoding};
  }
  const Result<DeviceIdentifier> actual = DeriveDeviceIdentifier(public_key);
  if (!actual.ok()) {
    return {.error = actual.error};
  }
  if (!internal::ConstantTimeEqual(internal::AsBytes(actual.value->text),
                                   internal::AsBytes(presented))) {
    return {.error = SecurityError::kDeviceIdentifierMismatch};
  }
  return {.value = true, .error = SecurityError::kNone};
}

}  // namespace xnn_transfer::core::security::tls
