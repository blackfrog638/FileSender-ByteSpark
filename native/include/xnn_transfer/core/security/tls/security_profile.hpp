#ifndef XNN_TRANSFER_CORE_SECURITY_TLS_SECURITY_PROFILE_HPP_
#define XNN_TRANSFER_CORE_SECURITY_TLS_SECURITY_PROFILE_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xnn_transfer/core/security/identity/identity_repository.hpp"
#include "xnn_transfer/core/security/identity/secret_buffer.hpp"

namespace xnn_transfer::core::security::tls {

inline constexpr std::size_t kSha256Size = 32;
inline constexpr std::size_t kEd25519PublicKeySize = identity::kEd25519PublicKeySize;
inline constexpr std::size_t kTransferSessionIdSize = 16;
inline constexpr std::size_t kMaximumCanonicalBodySize = 1'048'576;
inline constexpr std::size_t kMaximumCanonicalFields = 32;
inline constexpr std::uint16_t kSecurityProfileV1 = 1;

using Bytes = std::vector<std::uint8_t>;
using Digest256 = std::array<std::uint8_t, kSha256Size>;

enum class SecurityError {
  kNone,
  kCryptoFailure,
  kInvalidLength,
  kMalformedEncoding,
  kNonCanonicalEncoding,
  kUnsupportedVersion,
  kUnknownKind,
  kUnknownField,
  kDuplicateField,
  kMissingField,
  kTrailingData,
  kLimitExceeded,
  kDomainMismatch,
  kRoleMismatch,
  kInvalidPublicKey,
  kUnsupportedProfile,
  kInvalidNegotiation,
  kUnsupportedCapability,
  kDowngradeDetected,
  kMalformedTranscript,
  kContextMismatch,
  kConfirmationMismatch,
  kInvalidDecision,
  kAuthenticatedReject,
  kLocalReject,
  kDeviceIdentifierMismatch,
  kTransportFinishedMismatch,
  kInvalidRotation,
  kReplayDetected,
  kOutputMismatch,
  kIdentityUnavailable,
  kTlsConfigurationFailure,
  kHandshakeIncomplete,
  kTlsVersionMismatch,
  kCipherMismatch,
  kGroupMismatch,
  kSignatureMismatch,
  kAlpnMismatch,
  kPeerCertificateMismatch,
  kPinMismatch,
  kResumptionDetected,
  kEarlyDataDetected,
  kExporterFailure,
};

[[nodiscard]] std::string_view SecurityErrorName(SecurityError error) noexcept;

template <typename T>
struct Result {
  std::optional<T> value{};
  SecurityError error{SecurityError::kNone};

  [[nodiscard]] bool ok() const noexcept {
    return error == SecurityError::kNone && value.has_value();
  }
};

class ValidatedEd25519PublicKey final {
 public:
  ValidatedEd25519PublicKey(const ValidatedEd25519PublicKey&) = default;
  ValidatedEd25519PublicKey& operator=(const ValidatedEd25519PublicKey&) = default;

  [[nodiscard]] const identity::PublicKey& bytes() const noexcept { return bytes_; }

  friend bool operator==(const ValidatedEd25519PublicKey&,
                         const ValidatedEd25519PublicKey&) = default;

 private:
  explicit ValidatedEd25519PublicKey(identity::PublicKey bytes) noexcept
      : bytes_(std::move(bytes)) {}

  identity::PublicKey bytes_{};

  friend Result<ValidatedEd25519PublicKey> ValidateEd25519PublicKey(
      std::span<const std::uint8_t> encoded);
};

[[nodiscard]] Result<ValidatedEd25519PublicKey> ValidateEd25519PublicKey(
    std::span<const std::uint8_t> encoded);

class OpenSslPeerPublicKeyValidator final : public identity::PeerPublicKeyValidator {
 public:
  [[nodiscard]] identity::Result<void> Validate(
      const identity::PublicKey& public_key) override;
};

enum class CanonicalObjectKind : std::uint8_t {
  kNormalizedNegotiation = 1,
  kPairingContext = 2,
  kSasInformation = 3,
  kTransportContext = 4,
  kRotationContext = 5,
  kRotationProof = 6,
  kDeviceIdentifier = 7,
};

struct CanonicalObject {
  CanonicalObjectKind kind{CanonicalObjectKind::kNormalizedNegotiation};
  Bytes encoded{};
};

[[nodiscard]] Result<CanonicalObject> DecodeCanonicalObject(
    std::span<const std::uint8_t> encoded);
[[nodiscard]] Result<CanonicalObject> DecodeCanonicalObject(
    std::span<const std::uint8_t> encoded, CanonicalObjectKind expected_kind);
[[nodiscard]] Result<bool> VerifyCanonicalDigest(
    std::span<const std::uint8_t> encoded, CanonicalObjectKind expected_kind,
    std::span<const std::uint8_t> expected_digest);

class NormalizedNegotiation final {
 public:
  NormalizedNegotiation(const NormalizedNegotiation&) = default;
  NormalizedNegotiation& operator=(const NormalizedNegotiation&) = default;

  [[nodiscard]] std::span<const std::uint8_t> encoded() const noexcept {
    return encoded_;
  }

 private:
  explicit NormalizedNegotiation(Bytes encoded) : encoded_(std::move(encoded)) {}

  Bytes encoded_{};

  friend Result<NormalizedNegotiation> DecodeNormalizedNegotiation(
      std::span<const std::uint8_t> encoded);
};

[[nodiscard]] Result<NormalizedNegotiation> DecodeNormalizedNegotiation(
    std::span<const std::uint8_t> encoded);

struct Nonce256 {
  Digest256 bytes{};
};

struct TransferSessionId {
  std::array<std::uint8_t, kTransferSessionIdSize> bytes{};
};

class PairingContext final {
 public:
  PairingContext(const PairingContext&) = default;
  PairingContext& operator=(const PairingContext&) = default;

  [[nodiscard]] std::span<const std::uint8_t> encoded() const noexcept {
    return encoded_;
  }
  [[nodiscard]] const Digest256& digest() const noexcept { return digest_; }

 private:
  PairingContext(Bytes encoded, Digest256 digest)
      : encoded_(std::move(encoded)), digest_(digest) {}

  Bytes encoded_{};
  Digest256 digest_{};

  friend Result<PairingContext> BuildPairingContext(
      const struct PairingContextInput& input);
};

struct PairingContextInput {
  Nonce256 initiator_nonce{};
  Nonce256 responder_nonce{};
  ValidatedEd25519PublicKey initiator_key;
  ValidatedEd25519PublicKey responder_key;
  NormalizedNegotiation negotiation;
};

[[nodiscard]] Result<PairingContext> BuildPairingContext(
    const PairingContextInput& input);

class TransportContext final {
 public:
  TransportContext(const TransportContext&) = default;
  TransportContext& operator=(const TransportContext&) = default;

  [[nodiscard]] std::span<const std::uint8_t> encoded() const noexcept {
    return encoded_;
  }
  [[nodiscard]] const Digest256& digest() const noexcept { return digest_; }

 private:
  TransportContext(Bytes encoded, Digest256 digest)
      : encoded_(std::move(encoded)), digest_(digest) {}

  Bytes encoded_{};
  Digest256 digest_{};

  friend Result<TransportContext> BuildTransportContext(
      const struct TransportContextInput& input);
};

struct TransportContextInput {
  ValidatedEd25519PublicKey initiator_key;
  ValidatedEd25519PublicKey responder_key;
  Nonce256 initiator_nonce{};
  Nonce256 responder_nonce{};
  NormalizedNegotiation negotiation;
  Bytes raw_negotiation_transcript{};
  TransferSessionId session_id{};
};

[[nodiscard]] Result<TransportContext> BuildTransportContext(
    const TransportContextInput& input);

struct PairingExporterLabel final {};
struct ConfirmationExporterLabel final {};
struct TransportExporterLabel final {};

[[nodiscard]] std::string_view ExporterLabel(PairingExporterLabel) noexcept;
[[nodiscard]] std::string_view ExporterLabel(ConfirmationExporterLabel) noexcept;
[[nodiscard]] std::string_view ExporterLabel(TransportExporterLabel) noexcept;

template <typename Label>
struct ExporterInput {
  using LabelType = Label;
  static constexpr std::size_t kOutputLength = kSha256Size;

  Digest256 context{};
};

using PairingExporterInput = ExporterInput<PairingExporterLabel>;
using ConfirmationExporterInput = ExporterInput<ConfirmationExporterLabel>;
using TransportExporterInput = ExporterInput<TransportExporterLabel>;

[[nodiscard]] PairingExporterInput MakePairingExporterInput(
    const PairingContext& context) noexcept;
[[nodiscard]] ConfirmationExporterInput MakeConfirmationExporterInput(
    const PairingContext& context) noexcept;
[[nodiscard]] TransportExporterInput MakeTransportExporterInput(
    const TransportContext& context) noexcept;

[[nodiscard]] Result<Digest256> Sha256(std::span<const std::uint8_t> input);
[[nodiscard]] Result<Digest256> HmacSha256(std::span<const std::uint8_t> key,
                                           std::span<const std::uint8_t> message);
[[nodiscard]] Result<bool> VerifySha256(std::span<const std::uint8_t> value,
                                        std::span<const std::uint8_t> expected_digest);

struct SasWords {
  CanonicalObject hkdf_information{};
  identity::SecretBuffer expanded{};
  std::array<std::uint16_t, 5> indices{};
};

[[nodiscard]] Result<SasWords> DeriveSasWords(
    std::span<const std::uint8_t> pairing_exporter, const PairingContext& context);

enum class Role : std::uint8_t {
  kInitiator = 1,
  kResponder = 2,
};

enum class ConfirmationDecision : std::uint8_t {
  kReject = 0,
  kConfirm = 1,
};

enum class ConfirmationOutcome {
  kAuthenticatedReject,
  kAffirmativeConfirm,
};

struct ConfirmationValue {
  std::array<std::uint8_t, kSha256Size + 2> message{};
  Digest256 authenticator{};
};

struct ConfirmationVerification {
  Role sender{Role::kInitiator};
  ConfirmationDecision decision{ConfirmationDecision::kReject};
  ConfirmationOutcome outcome{ConfirmationOutcome::kAuthenticatedReject};
  bool terminal{true};
  bool trust_commit_permitted{false};
};

[[nodiscard]] Result<ConfirmationValue> BuildConfirmation(
    std::span<const std::uint8_t> confirmation_exporter, const PairingContext& context,
    Role sender, ConfirmationDecision decision);
[[nodiscard]] Result<ConfirmationVerification> VerifyConfirmation(
    std::span<const std::uint8_t> confirmation_exporter,
    std::span<const std::uint8_t> message, std::span<const std::uint8_t> authenticator,
    const PairingContext& expected_context, Role expected_sender);
[[nodiscard]] Result<ConfirmationVerification> RequireTrustCommit(
    std::span<const std::uint8_t> confirmation_exporter,
    std::span<const std::uint8_t> message, std::span<const std::uint8_t> authenticator,
    const PairingContext& expected_context, Role expected_sender,
    ConfirmationDecision local_decision);

struct DeviceIdentifier {
  CanonicalObject input{};
  Digest256 digest{};
  std::string text{};
};

[[nodiscard]] Result<DeviceIdentifier> DeriveDeviceIdentifier(
    const ValidatedEd25519PublicKey& public_key);
[[nodiscard]] Result<DeviceIdentifier> DeriveDeviceIdentifier(
    std::span<const std::uint8_t> canonical_input);
[[nodiscard]] Result<bool> VerifyDeviceIdentifier(
    const ValidatedEd25519PublicKey& public_key,
    std::span<const std::uint8_t> expected_identifier);
[[nodiscard]] Result<bool> VerifyDeviceIdentifierText(
    const ValidatedEd25519PublicKey& public_key, std::string_view presented);

struct TransportFinishedValue {
  std::array<std::uint8_t, kSha256Size + 1> message{};
  Digest256 authenticator{};
};

[[nodiscard]] Result<TransportFinishedValue> BuildTransportFinished(
    std::span<const std::uint8_t> transport_exporter, const TransportContext& context,
    Role sender);
[[nodiscard]] Result<bool> VerifyTransportFinished(
    std::span<const std::uint8_t> transport_exporter,
    std::span<const std::uint8_t> message, std::span<const std::uint8_t> authenticator,
    const TransportContext& expected_context, Role expected_sender);

class RotationContext final {
 public:
  RotationContext(const RotationContext&) = default;
  RotationContext& operator=(const RotationContext&) = default;

  [[nodiscard]] std::span<const std::uint8_t> encoded() const noexcept {
    return encoded_;
  }
  [[nodiscard]] const Digest256& digest() const noexcept { return digest_; }

 private:
  RotationContext(Bytes encoded, Digest256 digest)
      : encoded_(std::move(encoded)), digest_(digest) {}

  Bytes encoded_{};
  Digest256 digest_{};

  friend Result<RotationContext> BuildRotationContext(
      const struct RotationContextInput& input);
};

struct RotationContextInput {
  ValidatedEd25519PublicKey old_key;
  ValidatedEd25519PublicKey new_key;
  std::uint64_t counter{};
  Nonce256 nonce{};
  TransportContext transport_context;
};

enum class RotationSigner : std::uint8_t {
  kOldKey = 1,
  kNewKey = 2,
};

[[nodiscard]] Result<RotationContext> BuildRotationContext(
    const RotationContextInput& input);
[[nodiscard]] Result<CanonicalObject> BuildRotationProofInput(
    const RotationContext& context, RotationSigner signer);
[[nodiscard]] SecurityError ValidateRotationCounter(std::uint64_t previous,
                                                    std::uint64_t current) noexcept;

}  // namespace xnn_transfer::core::security::tls

#endif  // XNN_TRANSFER_CORE_SECURITY_TLS_SECURITY_PROFILE_HPP_
