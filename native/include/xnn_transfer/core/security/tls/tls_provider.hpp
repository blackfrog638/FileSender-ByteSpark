#ifndef XNN_TRANSFER_CORE_SECURITY_TLS_TLS_PROVIDER_HPP_
#define XNN_TRANSFER_CORE_SECURITY_TLS_TLS_PROVIDER_HPP_

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "xnn_transfer/core/security/identity/identity_repository.hpp"
#include "xnn_transfer/core/security/identity/secret_buffer.hpp"
#include "xnn_transfer/core/security/tls/security_profile.hpp"

struct ssl_ctx_st;
struct ssl_st;

namespace xnn_transfer::core::session {
class EstablishedTlsChannel;
class OpenSslPairingChannel;
}  // namespace xnn_transfer::core::session

namespace xnn_transfer::core::security::tls {

using PairingAlpnPolicy = std::function<std::uint64_t()>;

inline constexpr std::array<std::uint8_t, 22> kPairingAlpn = {
    'x', 'n', 'n', '-', 't', 'r', 'a', 'n', 's', 'f', 'e',
    'r', '-', 'p', 'a', 'i', 'r', 'i', 'n', 'g', '/', '1',
};
inline constexpr std::array<std::uint8_t, 14> kEstablishedAlpn = {
    'x', 'n', 'n', '-', 't', 'r', 'a', 'n', 's', 'f', 'e', 'r', '/', '1',
};

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
        peer_public_key_(std::move(other.peer_public_key_)),
        alpn_(std::move(other.alpn_)),
        local_finished_(other.local_finished_),
        peer_finished_(other.peer_finished_),
        local_finished_size_(std::exchange(other.local_finished_size_, std::uint8_t{})),
        peer_finished_size_(std::exchange(other.peer_finished_size_, std::uint8_t{})) {}

  VerifiedTlsConnection& operator=(VerifiedTlsConnection&& other) noexcept {
    if (this != &other) {
      connection_ = std::exchange(other.connection_, nullptr);
      owner_ = std::exchange(other.owner_, nullptr);
      peer_public_key_ = std::move(other.peer_public_key_);
      alpn_ = std::move(other.alpn_);
      local_finished_ = other.local_finished_;
      peer_finished_ = other.peer_finished_;
      local_finished_size_ = std::exchange(other.local_finished_size_, std::uint8_t{});
      peer_finished_size_ = std::exchange(other.peer_finished_size_, std::uint8_t{});
    }
    return *this;
  }

  [[nodiscard]] const ValidatedEd25519PublicKey& peer_public_key() const noexcept {
    return peer_public_key_;
  }

 private:
  VerifiedTlsConnection(ssl_st* connection, const void* owner,
                        ValidatedEd25519PublicKey peer_public_key,
                        std::span<const std::uint8_t> alpn,
                        std::array<std::uint8_t, 64> local_finished,
                        std::uint8_t local_finished_size,
                        std::array<std::uint8_t, 64> peer_finished,
                        std::uint8_t peer_finished_size)
      : connection_(connection),
        owner_(owner),
        peer_public_key_(std::move(peer_public_key)),
        alpn_(alpn.begin(), alpn.end()),
        local_finished_(local_finished),
        peer_finished_(peer_finished),
        local_finished_size_(local_finished_size),
        peer_finished_size_(peer_finished_size) {}

  ssl_st* connection_{};
  const void* owner_{};
  ValidatedEd25519PublicKey peer_public_key_;
  Bytes alpn_;
  std::array<std::uint8_t, 64> local_finished_{};
  std::array<std::uint8_t, 64> peer_finished_{};
  std::uint8_t local_finished_size_{};
  std::uint8_t peer_finished_size_{};

  friend class OpenSslTlsContext;
};

class AcceptedPairingTlsConnection final {
 public:
  AcceptedPairingTlsConnection(const AcceptedPairingTlsConnection&) = delete;
  AcceptedPairingTlsConnection& operator=(const AcceptedPairingTlsConnection&) = delete;
  AcceptedPairingTlsConnection(AcceptedPairingTlsConnection&&) noexcept = default;
  AcceptedPairingTlsConnection& operator=(AcceptedPairingTlsConnection&&) noexcept =
      default;

  [[nodiscard]] const ValidatedEd25519PublicKey& peer_public_key() const noexcept {
    return verified_.peer_public_key();
  }
  [[nodiscard]] std::uint64_t pairing_window_generation() const noexcept {
    return pairing_window_generation_;
  }

 private:
  AcceptedPairingTlsConnection(VerifiedTlsConnection verified,
                               std::uint64_t pairing_window_generation)
      : verified_(std::move(verified)),
        pairing_window_generation_(pairing_window_generation) {}

  VerifiedTlsConnection verified_;
  std::uint64_t pairing_window_generation_{};

  friend class OpenSslTlsContext;
  friend class xnn_transfer::core::session::OpenSslPairingChannel;
};

class AcceptedEstablishedTlsConnection final {
 public:
  AcceptedEstablishedTlsConnection(const AcceptedEstablishedTlsConnection&) = delete;
  AcceptedEstablishedTlsConnection& operator=(const AcceptedEstablishedTlsConnection&) =
      delete;
  AcceptedEstablishedTlsConnection(AcceptedEstablishedTlsConnection&&) noexcept =
      default;
  AcceptedEstablishedTlsConnection& operator=(
      AcceptedEstablishedTlsConnection&&) noexcept = default;

  [[nodiscard]] const ValidatedEd25519PublicKey& peer_public_key() const noexcept {
    return verified_.peer_public_key();
  }
  [[nodiscard]] const identity::DeviceId& peer_device_id() const noexcept {
    return peer_device_id_;
  }
  [[nodiscard]] std::uint16_t security_profile() const noexcept {
    return security_profile_;
  }
  [[nodiscard]] std::uint64_t repository_revision() const noexcept {
    return repository_revision_;
  }
  [[nodiscard]] std::uint64_t record_revision() const noexcept {
    return record_revision_;
  }

 private:
  AcceptedEstablishedTlsConnection(VerifiedTlsConnection verified,
                                   identity::DeviceId peer_device_id,
                                   std::uint16_t security_profile,
                                   std::uint64_t repository_revision,
                                   std::uint64_t record_revision)
      : verified_(std::move(verified)),
        peer_device_id_(peer_device_id),
        security_profile_(security_profile),
        repository_revision_(repository_revision),
        record_revision_(record_revision) {}

  VerifiedTlsConnection verified_;
  identity::DeviceId peer_device_id_{};
  std::uint16_t security_profile_{};
  std::uint64_t repository_revision_{};
  std::uint64_t record_revision_{};

  friend class OpenSslTlsContext;
  friend class xnn_transfer::core::session::EstablishedTlsChannel;
};

using AcceptedServerTlsConnection =
    std::variant<AcceptedPairingTlsConnection, AcceptedEstablishedTlsConnection>;

// Owns a fully configured OpenSSL SSL_CTX. The returned native handle is for
// constructing the Asio SSL stream only and must not be reconfigured.
class OpenSslTlsContext final {
 public:
  [[nodiscard]] static Result<OpenSslTlsContext> Create(
      TlsEndpointRole role, identity::IdentityRepository& identity_repository,
      std::span<const std::uint8_t> alpn_protocol);

  // Creates the only server context permitted to demultiplex the shared
  // discovery service port. Each client must offer exactly one registered
  // protocol. Pairing ALPN is selected only when the non-blocking policy
  // reports a nonzero local window generation; that generation is retained by
  // the pairing capability for post-TLS admission. The returned post-handshake
  // capabilities are not interchangeable.
  [[nodiscard]] static Result<OpenSslTlsContext> CreateServerDispatcher(
      identity::IdentityRepository& identity_repository,
      PairingAlpnPolicy pairing_alpn_policy);

  ~OpenSslTlsContext();

  OpenSslTlsContext(const OpenSslTlsContext&) = delete;
  OpenSslTlsContext& operator=(const OpenSslTlsContext&) = delete;
  OpenSslTlsContext(OpenSslTlsContext&&) noexcept;
  OpenSslTlsContext& operator=(OpenSslTlsContext&&) noexcept;

  [[nodiscard]] ssl_ctx_st* native_handle() const noexcept;
  [[nodiscard]] std::span<const std::uint8_t> alpn_protocol() const noexcept;

  [[nodiscard]] Result<VerifiedTlsConnection> VerifyPeer(
      ssl_st* connection, std::optional<identity::PublicKey> expected_pin) const;

  [[nodiscard]] Result<AcceptedServerTlsConnection> AcceptServerPeer(
      ssl_st* connection,
      const identity::IdentityRepository& identity_repository) const;

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
  [[nodiscard]] static Result<OpenSslTlsContext> CreateForAlpns(
      TlsEndpointRole role, identity::IdentityRepository& identity_repository,
      std::vector<Bytes> alpn_protocols, bool server_dispatcher,
      PairingAlpnPolicy pairing_alpn_policy);

  [[nodiscard]] Result<identity::SecretBuffer> ExportKeyingMaterial(
      const VerifiedTlsConnection& connection, std::string_view label,
      std::span<const std::uint8_t> context) const;
  [[nodiscard]] bool Owns(const VerifiedTlsConnection& connection,
                          ssl_st* native_connection,
                          std::span<const std::uint8_t> alpn) const noexcept;
  [[nodiscard]] bool MatchesHandshake(
      const VerifiedTlsConnection& connection) const noexcept;

  std::unique_ptr<Implementation> implementation_;

  friend class xnn_transfer::core::session::EstablishedTlsChannel;
  friend class xnn_transfer::core::session::OpenSslPairingChannel;
};

}  // namespace xnn_transfer::core::security::tls

#endif  // XNN_TRANSFER_CORE_SECURITY_TLS_TLS_PROVIDER_HPP_
