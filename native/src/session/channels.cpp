#include <openssl/crypto.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <new>
#include <utility>

#include "xnn_transfer/core/session/session.hpp"

namespace xnn_transfer::core::session {
namespace {

using security::identity::ErrorCode;
using security::identity::Result;
using security::identity::SecretBuffer;
using security::identity::TrustState;
using security::tls::OpenSslTlsContext;
using security::tls::SecurityError;
using security::tls::VerifiedTlsConnection;

constexpr std::size_t kMaxTlsFinishedSize = 64;

struct HandshakeBinding {
  std::array<std::uint8_t, kMaxTlsFinishedSize> local_finished{};
  std::array<std::uint8_t, kMaxTlsFinishedSize> peer_finished{};
  std::size_t local_size{};
  std::size_t peer_size{};
};

[[nodiscard]] bool CaptureHandshakeBinding(ssl_st* const connection,
                                           HandshakeBinding& output) noexcept {
  if (connection == nullptr) {
    return false;
  }
  output.local_size = SSL_get_finished(connection, output.local_finished.data(),
                                       output.local_finished.size());
  output.peer_size = SSL_get_peer_finished(connection, output.peer_finished.data(),
                                           output.peer_finished.size());
  return output.local_size != 0 && output.local_size <= output.local_finished.size() &&
         output.peer_size != 0 && output.peer_size <= output.peer_finished.size();
}

[[nodiscard]] bool SameHandshake(ssl_st* const connection,
                                 const HandshakeBinding& expected) noexcept {
  HandshakeBinding current{};
  return CaptureHandshakeBinding(connection, current) &&
         current.local_size == expected.local_size &&
         current.peer_size == expected.peer_size &&
         CRYPTO_memcmp(current.local_finished.data(), expected.local_finished.data(),
                       expected.local_size) == 0 &&
         CRYPTO_memcmp(current.peer_finished.data(), expected.peer_finished.data(),
                       expected.peer_size) == 0;
}

[[nodiscard]] bool ExactAlpn(const std::span<const std::uint8_t> actual,
                             const std::span<const std::uint8_t> expected) noexcept {
  return actual.size() == expected.size() &&
         std::equal(actual.begin(), actual.end(), expected.begin());
}

[[nodiscard]] ErrorCode MapExporterError(const SecurityError error) noexcept {
  switch (error) {
    case SecurityError::kIdentityUnavailable:
      return ErrorCode::kIdentityLoss;
    case SecurityError::kInvalidLength:
      return ErrorCode::kInvalidArgument;
    case SecurityError::kNone:
      return ErrorCode::kNone;
    default:
      return ErrorCode::kCryptoFailure;
  }
}

}  // namespace

struct OpenSslPairingChannel::Implementation {
  Implementation(const OpenSslTlsContext& value_context,
                 ssl_st* const value_native_connection,
                 HandshakeBinding value_handshake,
                 VerifiedTlsConnection value_connection)
      : context(&value_context),
        native_connection(value_native_connection),
        handshake(std::move(value_handshake)),
        connection(std::move(value_connection)) {}

  const OpenSslTlsContext* context{};
  ssl_st* native_connection{};
  HandshakeBinding handshake{};
  VerifiedTlsConnection connection;
  SecretBuffer pairing_binding{};
  bool pairing_exported{};
  bool confirmation_exported{};
};

security::tls::Result<std::unique_ptr<OpenSslPairingChannel>>
OpenSslPairingChannel::Create(const OpenSslTlsContext& context,
                              ssl_st* const connection) {
  if (!ExactAlpn(context.alpn_protocol(), security::tls::kPairingAlpn)) {
    return {.error = SecurityError::kAlpnMismatch};
  }
  auto verified = context.VerifyPeer(connection, std::nullopt);
  if (!verified.ok()) {
    return {.error = verified.error};
  }
  HandshakeBinding handshake{};
  if (!CaptureHandshakeBinding(connection, handshake)) {
    return {.error = SecurityError::kHandshakeIncomplete};
  }
  try {
    auto implementation = std::make_unique<Implementation>(
        context, connection, handshake, std::move(*verified.value));
    return {
        .value = std::unique_ptr<OpenSslPairingChannel>(
            new OpenSslPairingChannel(std::move(implementation))),
        .error = SecurityError::kNone,
    };
  } catch (const std::bad_alloc&) {
    return {.error = SecurityError::kCryptoFailure};
  }
}

security::tls::Result<std::unique_ptr<OpenSslPairingChannel>>
OpenSslPairingChannel::Create(const OpenSslTlsContext& context,
                              ssl_st* const connection,
                              security::tls::AcceptedPairingTlsConnection accepted) {
  if (!context.Owns(accepted.verified_, connection, security::tls::kPairingAlpn)) {
    return {.error = SecurityError::kAlpnMismatch};
  }
  HandshakeBinding handshake{};
  if (!CaptureHandshakeBinding(connection, handshake)) {
    return {.error = SecurityError::kHandshakeIncomplete};
  }
  try {
    auto implementation = std::make_unique<Implementation>(
        context, connection, handshake, std::move(accepted.verified_));
    return {
        .value = std::unique_ptr<OpenSslPairingChannel>(
            new OpenSslPairingChannel(std::move(implementation))),
        .error = SecurityError::kNone,
    };
  } catch (const std::bad_alloc&) {
    return {.error = SecurityError::kCryptoFailure};
  }
}

OpenSslPairingChannel::OpenSslPairingChannel(
    std::unique_ptr<Implementation> implementation)
    : implementation_(std::move(implementation)) {}
OpenSslPairingChannel::~OpenSslPairingChannel() = default;
OpenSslPairingChannel::OpenSslPairingChannel(OpenSslPairingChannel&&) noexcept =
    default;
OpenSslPairingChannel& OpenSslPairingChannel::operator=(
    OpenSslPairingChannel&&) noexcept = default;

const security::tls::ValidatedEd25519PublicKey& OpenSslPairingChannel::peer_public_key()
    const noexcept {
  return implementation_->connection.peer_public_key();
}

Result<SecretBuffer> OpenSslPairingChannel::ExportPairing(
    const security::tls::PairingContext& context) {
  if (implementation_->pairing_exported ||
      !SameHandshake(implementation_->native_connection, implementation_->handshake)) {
    return Result<SecretBuffer>::Failure(ErrorCode::kInvalidArgument);
  }
  auto exported = implementation_->context->ExportKeyingMaterial(
      implementation_->connection, security::tls::MakePairingExporterInput(context));
  if (!exported.ok()) {
    return Result<SecretBuffer>::Failure(MapExporterError(exported.error));
  }
  try {
    implementation_->pairing_binding = SecretBuffer(exported.value->bytes());
    implementation_->pairing_exported = true;
    return Result<SecretBuffer>::Success(std::move(*exported.value));
  } catch (const std::bad_alloc&) {
    return Result<SecretBuffer>::Failure(ErrorCode::kCryptoFailure);
  }
}

Result<SecretBuffer> OpenSslPairingChannel::ExportConfirmation(
    const security::tls::PairingContext& context) {
  if (!implementation_->pairing_exported || implementation_->confirmation_exported) {
    return Result<SecretBuffer>::Failure(ErrorCode::kInvalidArgument);
  }
  if (!SameHandshake(implementation_->native_connection, implementation_->handshake)) {
    implementation_->pairing_binding.clear();
    return Result<SecretBuffer>::Failure(ErrorCode::kCryptoFailure);
  }
  auto current_pairing = implementation_->context->ExportKeyingMaterial(
      implementation_->connection, security::tls::MakePairingExporterInput(context));
  if (!current_pairing.ok() ||
      current_pairing.value->size() != implementation_->pairing_binding.size() ||
      CRYPTO_memcmp(current_pairing.value->bytes().data(),
                    implementation_->pairing_binding.bytes().data(),
                    implementation_->pairing_binding.size()) != 0) {
    implementation_->pairing_binding.clear();
    return Result<SecretBuffer>::Failure(ErrorCode::kCryptoFailure);
  }
  current_pairing.value->clear();
  auto exported = implementation_->context->ExportKeyingMaterial(
      implementation_->connection,
      security::tls::MakeConfirmationExporterInput(context));
  if (!exported.ok()) {
    return Result<SecretBuffer>::Failure(MapExporterError(exported.error));
  }
  implementation_->pairing_binding.clear();
  implementation_->confirmation_exported = true;
  return Result<SecretBuffer>::Success(std::move(*exported.value));
}

struct EstablishedTlsChannel::Implementation {
  Implementation(const OpenSslTlsContext& value_context,
                 ssl_st* const value_native_connection,
                 HandshakeBinding value_handshake, DeviceId value_device_id,
                 PublicKey value_public_key, const std::uint16_t value_security_profile,
                 const std::uint64_t value_record_revision,
                 const std::uint64_t value_identity_revision,
                 VerifiedTlsConnection value_connection)
      : context(&value_context),
        native_connection(value_native_connection),
        handshake(std::move(value_handshake)),
        device_id(value_device_id),
        public_key(value_public_key),
        security_profile(value_security_profile),
        record_revision(value_record_revision),
        identity_revision(value_identity_revision),
        connection(std::move(value_connection)) {}

  const OpenSslTlsContext* context{};
  ssl_st* native_connection{};
  HandshakeBinding handshake{};
  DeviceId device_id{};
  PublicKey public_key{};
  std::uint16_t security_profile{};
  std::uint64_t record_revision{};
  std::uint64_t identity_revision{};
  VerifiedTlsConnection connection;
  std::optional<security::tls::TransportContext> transport_context{};
  SecretBuffer transport_exporter{};
  security::tls::Role local_role{security::tls::Role::kInitiator};
  bool local_finished_prepared{};
  bool local_finished_written{};
  bool peer_finished_verified{};
};

security::tls::Result<std::unique_ptr<EstablishedTlsChannel>>
EstablishedTlsChannel::Create(const OpenSslTlsContext& context,
                              ssl_st* const connection,
                              const security::identity::IdentityRepository& repository,
                              const DeviceId& peer_device_id) {
  if (!ExactAlpn(context.alpn_protocol(), security::tls::kEstablishedAlpn)) {
    return {.error = SecurityError::kAlpnMismatch};
  }
  if (!repository.ready()) {
    return {.error = SecurityError::kIdentityUnavailable};
  }
  const security::identity::PeerRecord* const peer =
      repository.FindPeer(peer_device_id);
  if (peer == nullptr || peer->trust_state != TrustState::kActive) {
    return {.error = SecurityError::kPinMismatch};
  }
  if (peer->security_profile > security::tls::kSecurityProfileV1) {
    return {.error = SecurityError::kUnsupportedProfile};
  }
  auto verified = context.VerifyPeer(connection, peer->public_key);
  if (!verified.ok()) {
    return {.error = verified.error};
  }
  HandshakeBinding handshake{};
  if (!CaptureHandshakeBinding(connection, handshake)) {
    return {.error = SecurityError::kHandshakeIncomplete};
  }
  try {
    auto implementation = std::make_unique<Implementation>(
        context, connection, handshake, peer->device_id, peer->public_key,
        security::tls::kSecurityProfileV1, peer->record_revision, repository.revision(),
        std::move(*verified.value));
    return {
        .value = std::unique_ptr<EstablishedTlsChannel>(
            new EstablishedTlsChannel(std::move(implementation))),
        .error = SecurityError::kNone,
    };
  } catch (const std::bad_alloc&) {
    return {.error = SecurityError::kCryptoFailure};
  }
}

security::tls::Result<std::unique_ptr<EstablishedTlsChannel>>
EstablishedTlsChannel::Create(
    const OpenSslTlsContext& context, ssl_st* const connection,
    security::tls::AcceptedEstablishedTlsConnection accepted) {
  if (!context.Owns(accepted.verified_, connection, security::tls::kEstablishedAlpn)) {
    return {.error = SecurityError::kAlpnMismatch};
  }
  HandshakeBinding handshake{};
  if (!CaptureHandshakeBinding(connection, handshake)) {
    return {.error = SecurityError::kHandshakeIncomplete};
  }
  try {
    const DeviceId peer_device_id = accepted.peer_device_id_;
    const PublicKey peer_public_key = accepted.verified_.peer_public_key().bytes();
    const std::uint64_t record_revision = accepted.record_revision_;
    const std::uint64_t repository_revision = accepted.repository_revision_;
    auto implementation = std::make_unique<Implementation>(
        context, connection, handshake, peer_device_id, peer_public_key,
        security::tls::kSecurityProfileV1, record_revision, repository_revision,
        std::move(accepted.verified_));
    return {
        .value = std::unique_ptr<EstablishedTlsChannel>(
            new EstablishedTlsChannel(std::move(implementation))),
        .error = SecurityError::kNone,
    };
  } catch (const std::bad_alloc&) {
    return {.error = SecurityError::kCryptoFailure};
  }
}

EstablishedTlsChannel::EstablishedTlsChannel(
    std::unique_ptr<Implementation> implementation)
    : implementation_(std::move(implementation)) {}
EstablishedTlsChannel::~EstablishedTlsChannel() = default;
EstablishedTlsChannel::EstablishedTlsChannel(EstablishedTlsChannel&&) noexcept =
    default;
EstablishedTlsChannel& EstablishedTlsChannel::operator=(
    EstablishedTlsChannel&&) noexcept = default;

const DeviceId& EstablishedTlsChannel::peer_device_id() const noexcept {
  return implementation_->device_id;
}

const PublicKey& EstablishedTlsChannel::peer_public_key() const noexcept {
  return implementation_->public_key;
}

std::uint16_t EstablishedTlsChannel::security_profile() const noexcept {
  return implementation_->security_profile;
}

std::uint64_t EstablishedTlsChannel::peer_record_revision() const noexcept {
  return implementation_->record_revision;
}

std::uint64_t EstablishedTlsChannel::identity_revision() const noexcept {
  return implementation_->identity_revision;
}

security::tls::Result<security::tls::TransportFinishedValue>
EstablishedTlsChannel::BeginTransportBinding(
    const security::tls::TransportContext& context,
    const security::tls::Role local_role) {
  if (implementation_->local_finished_prepared ||
      (local_role != security::tls::Role::kInitiator &&
       local_role != security::tls::Role::kResponder) ||
      !SameHandshake(implementation_->native_connection, implementation_->handshake)) {
    return {.error = SecurityError::kHandshakeIncomplete};
  }
  try {
    auto exported = implementation_->context->ExportKeyingMaterial(
        implementation_->connection,
        security::tls::MakeTransportExporterInput(context));
    if (!exported.ok()) {
      return {.error = exported.error};
    }
    auto finished = security::tls::BuildTransportFinished(exported.value->bytes(),
                                                          context, local_role);
    if (!finished.ok()) {
      exported.value->clear();
      return {.error = finished.error};
    }
    implementation_->transport_context = context;
    implementation_->transport_exporter = std::move(*exported.value);
    implementation_->local_role = local_role;
    implementation_->local_finished_prepared = true;
    return finished;
  } catch (const std::bad_alloc&) {
    return {.error = SecurityError::kCryptoFailure};
  }
}

SecurityError EstablishedTlsChannel::LocalTransportFinishedWritten() noexcept {
  if (!implementation_->local_finished_prepared) {
    return SecurityError::kHandshakeIncomplete;
  }
  implementation_->local_finished_written = true;
  return SecurityError::kNone;
}

SecurityError EstablishedTlsChannel::VerifyPeerTransportFinished(
    const std::span<const std::uint8_t> message,
    const std::span<const std::uint8_t> authenticator) {
  if (!implementation_->local_finished_prepared ||
      implementation_->peer_finished_verified ||
      !implementation_->transport_context.has_value()) {
    return SecurityError::kHandshakeIncomplete;
  }
  const security::tls::Role peer_role =
      implementation_->local_role == security::tls::Role::kInitiator
          ? security::tls::Role::kResponder
          : security::tls::Role::kInitiator;
  const auto verified = security::tls::VerifyTransportFinished(
      implementation_->transport_exporter.bytes(), message, authenticator,
      *implementation_->transport_context, peer_role);
  if (!verified.ok()) {
    return verified.error;
  }
  implementation_->peer_finished_verified = true;
  return SecurityError::kNone;
}

bool EstablishedTlsChannel::transport_bound() const noexcept {
  return implementation_->local_finished_prepared &&
         implementation_->local_finished_written &&
         implementation_->peer_finished_verified;
}

bool EstablishedTlsChannel::current_transport() const noexcept {
  if (!transport_bound() || !implementation_->transport_context.has_value() ||
      !SameHandshake(implementation_->native_connection, implementation_->handshake)) {
    return false;
  }
  try {
    auto current = implementation_->context->ExportKeyingMaterial(
        implementation_->connection,
        security::tls::MakeTransportExporterInput(*implementation_->transport_context));
    if (!current.ok() ||
        current.value->size() != implementation_->transport_exporter.size()) {
      return false;
    }
    const bool matches =
        CRYPTO_memcmp(current.value->bytes().data(),
                      implementation_->transport_exporter.bytes().data(),
                      implementation_->transport_exporter.size()) == 0;
    current.value->clear();
    return matches;
  } catch (const std::bad_alloc&) {
    return false;
  }
}

}  // namespace xnn_transfer::core::session
