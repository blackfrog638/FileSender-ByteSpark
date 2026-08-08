#include <openssl/crypto.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "internal.hpp"

namespace xnn_transfer::core::security::tls::internal {

SecurityError ValidateSasInformationObject(const ParsedObject& object) {
  return EqualBytes(FindField(object, 1)->value, AsBytes(kSasInformationLabel))
             ? SecurityError::kNone
             : SecurityError::kDomainMismatch;
}

bool IsValidRole(const Role role) noexcept {
  return role == Role::kInitiator || role == Role::kResponder;
}

bool IsValidDecision(const ConfirmationDecision decision) noexcept {
  return decision == ConfirmationDecision::kReject ||
         decision == ConfirmationDecision::kConfirm;
}

}  // namespace xnn_transfer::core::security::tls::internal

namespace xnn_transfer::core::security::tls {

Result<SasWords> DeriveSasWords(const std::span<const std::uint8_t> pairing_exporter,
                                const PairingContext& context) {
  const std::array<internal::FieldInput, 2> fields = {{
      {1, internal::AsBytes(internal::kSasInformationLabel)},
      {2, context.digest()},
  }};
  Result<Bytes> information =
      internal::EncodeObject(CanonicalObjectKind::kSasInformation, fields);
  if (!information.ok()) {
    return {.error = information.error};
  }
  Result<identity::SecretBuffer> expanded =
      internal::HkdfExpandSha256Secure(pairing_exporter, *information.value, 7);
  if (!expanded.ok()) {
    return {.error = expanded.error};
  }

  SasWords result{
      .hkdf_information =
          CanonicalObject{
              .kind = CanonicalObjectKind::kSasInformation,
              .encoded = std::move(*information.value),
          },
      .expanded = std::move(*expanded.value),
  };

  std::uint64_t bit_storage = 0;
  for (const std::uint8_t byte : result.expanded.bytes()) {
    bit_storage = (bit_storage << 8U) | byte;
  }
  bit_storage >>= 1U;

  constexpr std::array<unsigned, 5> kShifts = {44, 33, 22, 11, 0};
  for (std::size_t index = 0; index < result.indices.size(); ++index) {
    result.indices[index] =
        static_cast<std::uint16_t>((bit_storage >> kShifts[index]) & 0x7ffU);
  }
  OPENSSL_cleanse(&bit_storage, sizeof(bit_storage));
  return {
      .value = std::move(result),
      .error = SecurityError::kNone,
  };
}

Result<ConfirmationValue> BuildConfirmation(
    const std::span<const std::uint8_t> confirmation_exporter,
    const PairingContext& context, const Role sender,
    const ConfirmationDecision decision) {
  if (!internal::IsValidRole(sender)) {
    return {.error = SecurityError::kRoleMismatch};
  }
  if (!internal::IsValidDecision(decision)) {
    return {.error = SecurityError::kInvalidDecision};
  }

  ConfirmationValue output{};
  std::copy(context.digest().begin(), context.digest().end(), output.message.begin());
  output.message[32] = static_cast<std::uint8_t>(sender);
  output.message[33] = static_cast<std::uint8_t>(decision);
  Result<identity::SecretBuffer> authenticator =
      internal::HmacSha256Secure(confirmation_exporter, output.message);
  if (!authenticator.ok()) {
    return {.error = authenticator.error};
  }
  std::copy(authenticator.value->bytes().begin(), authenticator.value->bytes().end(),
            output.authenticator.begin());
  return {.value = output, .error = SecurityError::kNone};
}

Result<ConfirmationVerification> VerifyConfirmation(
    const std::span<const std::uint8_t> confirmation_exporter,
    const std::span<const std::uint8_t> message,
    const std::span<const std::uint8_t> authenticator,
    const PairingContext& expected_context, const Role expected_sender) {
  if (message.size() != kSha256Size + 2 || authenticator.size() != kSha256Size) {
    return {.error = SecurityError::kInvalidLength};
  }
  if (!internal::IsValidRole(expected_sender) ||
      message[32] != static_cast<std::uint8_t>(expected_sender)) {
    return {.error = SecurityError::kRoleMismatch};
  }
  const auto decision = static_cast<ConfirmationDecision>(message[33]);
  if (!internal::IsValidDecision(decision)) {
    return {.error = SecurityError::kInvalidDecision};
  }

  Result<identity::SecretBuffer> expected =
      internal::HmacSha256Secure(confirmation_exporter, message);
  if (!expected.ok()) {
    return {.error = expected.error};
  }
  if (!internal::ConstantTimeEqual(expected.value->bytes(), authenticator)) {
    return {.error = SecurityError::kConfirmationMismatch};
  }
  if (!internal::ConstantTimeEqual(message.first(kSha256Size),
                                   expected_context.digest())) {
    return {.error = SecurityError::kContextMismatch};
  }

  if (decision == ConfirmationDecision::kReject) {
    return {
        .value =
            ConfirmationVerification{
                .sender = expected_sender,
                .decision = decision,
                .outcome = ConfirmationOutcome::kAuthenticatedReject,
                .terminal = true,
                .trust_commit_permitted = false,
            },
        .error = SecurityError::kNone,
    };
  }
  return {
      .value =
          ConfirmationVerification{
              .sender = expected_sender,
              .decision = decision,
              .outcome = ConfirmationOutcome::kAffirmativeConfirm,
              .terminal = false,
              .trust_commit_permitted = false,
          },
      .error = SecurityError::kNone,
  };
}

Result<ConfirmationVerification> RequireTrustCommit(
    const std::span<const std::uint8_t> confirmation_exporter,
    const std::span<const std::uint8_t> message,
    const std::span<const std::uint8_t> authenticator,
    const PairingContext& expected_context, const Role expected_sender,
    const ConfirmationDecision local_decision) {
  if (!internal::IsValidDecision(local_decision)) {
    return {.error = SecurityError::kInvalidDecision};
  }
  if (local_decision == ConfirmationDecision::kReject) {
    return {.error = SecurityError::kLocalReject};
  }
  Result<ConfirmationVerification> verified = VerifyConfirmation(
      confirmation_exporter, message, authenticator, expected_context, expected_sender);
  if (!verified.ok()) {
    return verified;
  }
  if (verified.value->decision == ConfirmationDecision::kReject) {
    return {.error = SecurityError::kAuthenticatedReject};
  }
  verified.value->trust_commit_permitted = true;
  return verified;
}

Result<TransportFinishedValue> BuildTransportFinished(
    const std::span<const std::uint8_t> transport_exporter,
    const TransportContext& context, const Role sender) {
  if (!internal::IsValidRole(sender)) {
    return {.error = SecurityError::kRoleMismatch};
  }
  TransportFinishedValue output{};
  std::copy(context.digest().begin(), context.digest().end(), output.message.begin());
  output.message[32] = static_cast<std::uint8_t>(sender);
  Result<identity::SecretBuffer> authenticator =
      internal::HmacSha256Secure(transport_exporter, output.message);
  if (!authenticator.ok()) {
    return {.error = authenticator.error};
  }
  std::copy(authenticator.value->bytes().begin(), authenticator.value->bytes().end(),
            output.authenticator.begin());
  return {.value = output, .error = SecurityError::kNone};
}

Result<bool> VerifyTransportFinished(
    const std::span<const std::uint8_t> transport_exporter,
    const std::span<const std::uint8_t> message,
    const std::span<const std::uint8_t> authenticator,
    const TransportContext& expected_context, const Role expected_sender) {
  if (message.size() != kSha256Size + 1 || authenticator.size() != kSha256Size) {
    return {.error = SecurityError::kInvalidLength};
  }
  if (!internal::IsValidRole(expected_sender) ||
      message[32] != static_cast<std::uint8_t>(expected_sender)) {
    return {.error = SecurityError::kRoleMismatch};
  }
  Result<identity::SecretBuffer> expected =
      internal::HmacSha256Secure(transport_exporter, message);
  if (!expected.ok()) {
    return {.error = expected.error};
  }
  if (!internal::ConstantTimeEqual(expected.value->bytes(), authenticator)) {
    return {.error = SecurityError::kTransportFinishedMismatch};
  }
  if (!internal::ConstantTimeEqual(message.first(kSha256Size),
                                   expected_context.digest())) {
    return {.error = SecurityError::kContextMismatch};
  }
  return {.value = true, .error = SecurityError::kNone};
}

}  // namespace xnn_transfer::core::security::tls
