#ifndef XNN_TRANSFER_SECURITY_TLS_INTERNAL_HPP_
#define XNN_TRANSFER_SECURITY_TLS_INTERNAL_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "xnn_transfer/core/security/identity/secret_buffer.hpp"
#include "xnn_transfer/core/security/tls/security_profile.hpp"

namespace xnn_transfer::core::security::tls::internal {

inline constexpr std::string_view kPairContextLabel = "XnnTransfer pairing v1";
inline constexpr std::string_view kPairExporterLabel =
    "EXPORTER-XnnTransfer-Pairing-v1";
inline constexpr std::string_view kSasInformationLabel = "XnnTransfer SAS words v1";
inline constexpr std::string_view kConfirmationExporterLabel =
    "EXPORTER-XnnTransfer-Pairing-Confirmation-v1";
inline constexpr std::string_view kTransportContextLabel = "XnnTransfer transport v1";
inline constexpr std::string_view kTransportExporterLabel =
    "EXPORTER-XnnTransfer-Transport-v1";
inline constexpr std::string_view kRotationContextLabel = "XnnTransfer rotation v1";
inline constexpr std::string_view kRotationProofLabel = "XnnTransfer rotation proof v1";
inline constexpr std::string_view kDeviceIdentifierLabel =
    "XnnTransfer device identifier v1";

struct FieldInput {
  std::uint16_t id;
  std::span<const std::uint8_t> value;
};

struct ParsedField {
  std::uint16_t id;
  Bytes value;
};

struct ParsedObject {
  CanonicalObjectKind kind;
  Bytes encoded;
  std::vector<ParsedField> fields;
};

struct ParsedResult {
  std::optional<ParsedObject> value{};
  SecurityError error{SecurityError::kNone};

  [[nodiscard]] bool ok() const noexcept {
    return error == SecurityError::kNone && value.has_value();
  }
};

[[nodiscard]] std::span<const std::uint8_t> AsBytes(std::string_view value) noexcept;
[[nodiscard]] bool EqualBytes(std::span<const std::uint8_t> left,
                              std::span<const std::uint8_t> right) noexcept;
[[nodiscard]] bool ConstantTimeEqual(std::span<const std::uint8_t> left,
                                     std::span<const std::uint8_t> right) noexcept;

[[nodiscard]] std::uint16_t ReadU16(std::span<const std::uint8_t> encoded,
                                    std::size_t offset) noexcept;
[[nodiscard]] std::uint32_t ReadU32(std::span<const std::uint8_t> encoded,
                                    std::size_t offset) noexcept;
[[nodiscard]] std::uint64_t ReadU64(std::span<const std::uint8_t> encoded) noexcept;
[[nodiscard]] std::array<std::uint8_t, 8> EncodeU64(std::uint64_t value) noexcept;

[[nodiscard]] const ParsedField* FindField(const ParsedObject& object,
                                           std::uint16_t id) noexcept;
[[nodiscard]] ParsedResult ParseObject(
    std::span<const std::uint8_t> encoded,
    std::optional<CanonicalObjectKind> expected_kind = std::nullopt);
[[nodiscard]] Result<Bytes> EncodeObject(CanonicalObjectKind kind,
                                         std::span<const FieldInput> fields);

[[nodiscard]] SecurityError ValidateNegotiationObject(const ParsedObject& object);
[[nodiscard]] SecurityError ValidatePairingObject(const ParsedObject& object);
[[nodiscard]] SecurityError ValidateSasInformationObject(const ParsedObject& object);
[[nodiscard]] SecurityError ValidateTransportObject(const ParsedObject& object);
[[nodiscard]] SecurityError ValidateRotationContextObject(const ParsedObject& object);
[[nodiscard]] SecurityError ValidateRotationProofObject(const ParsedObject& object);
[[nodiscard]] SecurityError ValidateDeviceIdentifierObject(const ParsedObject& object);
[[nodiscard]] SecurityError ValidateKeyField(const ParsedObject& object,
                                             std::uint16_t id);

[[nodiscard]] bool IsValidRole(Role role) noexcept;
[[nodiscard]] bool IsValidDecision(ConfirmationDecision decision) noexcept;

[[nodiscard]] Result<identity::SecretBuffer> HmacSha256Secure(
    std::span<const std::uint8_t> key, std::span<const std::uint8_t> message);
[[nodiscard]] Result<identity::SecretBuffer> HkdfExpandSha256Secure(
    std::span<const std::uint8_t> pseudorandom_key,
    std::span<const std::uint8_t> information, std::size_t output_length);

}  // namespace xnn_transfer::core::security::tls::internal

#endif  // XNN_TRANSFER_SECURITY_TLS_INTERNAL_HPP_
