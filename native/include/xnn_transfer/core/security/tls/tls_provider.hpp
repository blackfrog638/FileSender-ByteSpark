#ifndef XNN_TRANSFER_CORE_SECURITY_TLS_TLS_PROVIDER_HPP_
#define XNN_TRANSFER_CORE_SECURITY_TLS_TLS_PROVIDER_HPP_

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "xnn_transfer/core/security/identity/identity_repository.hpp"
#include "xnn_transfer/core/security/identity/secret_buffer.hpp"
#include "xnn_transfer/core/security/tls/security_profile.hpp"

struct ssl_ctx_st;
struct ssl_st;

namespace xnn_transfer::core::security::tls {

enum class TlsEndpointRole {
  kClient,
  kServer,
};

// Non-owning proof that one live SSL connection completed peer verification.
// The capability must not outlive the SSL stream/ssl_st or its creating
// OpenSslTlsContext. SSL_clear or any other connection reuse requires a fresh
// VerifyPeer result. Moving the capability invalidates the source.
class VerifiedTlsConnection final {
 public:
  VerifiedTlsConnection(const VerifiedTlsConnection&) = delete;
  VerifiedTlsConnection& operator=(const VerifiedTlsConnection&) = delete;

  VerifiedTlsConnection(VerifiedTlsConnection&& other) noexcept
      : connection_(std::exchange(other.connection_, nullptr)),
        owner_(std::exchange(other.owner_, nullptr)),
        peer_public_key_(std::move(other.peer_public_key_)) {}

  VerifiedTlsConnection& operator=(VerifiedTlsConnection&& other) noexcept {
    if (this != &other) {
      connection_ = std::exchange(other.connection_, nullptr);
      owner_ = std::exchange(other.owner_, nullptr);
      peer_public_key_ = std::move(other.peer_public_key_);
    }
    return *this;
  }

  [[nodiscard]] const ValidatedEd25519PublicKey& peer_public_key() const noexcept {
    return peer_public_key_;
  }

 private:
  VerifiedTlsConnection(ssl_st* connection, const void* owner,
                        ValidatedEd25519PublicKey peer_public_key)
      : connection_(connection),
        owner_(owner),
        peer_public_key_(std::move(peer_public_key)) {}

  ssl_st* connection_{};
  const void* owner_{};
  ValidatedEd25519PublicKey peer_public_key_;

  friend class OpenSslTlsContext;
};

// Owns a fully configured OpenSSL SSL_CTX. The returned native handle is for
// constructing the Asio SSL stream only and must not be reconfigured.
class OpenSslTlsContext final {
 public:
  [[nodiscard]] static Result<OpenSslTlsContext> Create(
      TlsEndpointRole role, identity::IdentityRepository& identity_repository,
      std::span<const std::uint8_t> alpn_protocol);

  ~OpenSslTlsContext();

  OpenSslTlsContext(const OpenSslTlsContext&) = delete;
  OpenSslTlsContext& operator=(const OpenSslTlsContext&) = delete;
  OpenSslTlsContext(OpenSslTlsContext&&) noexcept;
  OpenSslTlsContext& operator=(OpenSslTlsContext&&) noexcept;

  [[nodiscard]] ssl_ctx_st* native_handle() const noexcept;
  [[nodiscard]] std::span<const std::uint8_t> alpn_protocol() const noexcept;

  [[nodiscard]] Result<VerifiedTlsConnection> VerifyPeer(
      ssl_st* connection, std::optional<identity::PublicKey> expected_pin) const;

  [[nodiscard]] Result<identity::SecretBuffer> ExportKeyingMaterial(
      const VerifiedTlsConnection& connection, const PairingExporterInput& input) const;
  [[nodiscard]] Result<identity::SecretBuffer> ExportKeyingMaterial(
      const VerifiedTlsConnection& connection,
      const ConfirmationExporterInput& input) const;
  [[nodiscard]] Result<identity::SecretBuffer> ExportKeyingMaterial(
      const VerifiedTlsConnection& connection,
      const TransportExporterInput& input) const;

 private:
  struct Implementation;

  explicit OpenSslTlsContext(std::unique_ptr<Implementation> implementation);

  [[nodiscard]] Result<identity::SecretBuffer> ExportKeyingMaterial(
      const VerifiedTlsConnection& connection, std::string_view label,
      std::span<const std::uint8_t> context) const;

  std::unique_ptr<Implementation> implementation_;
};

}  // namespace xnn_transfer::core::security::tls

#endif  // XNN_TRANSFER_CORE_SECURITY_TLS_TLS_PROVIDER_HPP_
