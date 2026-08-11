#ifndef XNN_TRANSFER_CORE_SESSION_CONNECTION_RUNTIME_HPP_
#define XNN_TRANSFER_CORE_SESSION_CONNECTION_RUNTIME_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "xnn_transfer/core/session/session.hpp"

namespace xnn_transfer::core::session {

inline constexpr std::size_t kMaxEstablishedIoBytes = 1'048'576;
inline constexpr std::size_t kMaxEstablishedQueuedWriteBytes = 4'194'304;
inline constexpr std::size_t kMaxEstablishedPendingWrites = 64;
inline constexpr std::size_t kMaxAuthenticatedConnections = 64;
inline constexpr std::size_t kMaxPendingTlsHandshakes = kMaxIncompletePairingHandshakes;
inline constexpr std::uint64_t kDefaultTlsHandshakeTimeoutMs = 5'000;

enum class NetworkAddressFamily : std::uint8_t {
  kIpv4 = 4,
  kIpv6 = 6,
};

struct NetworkEndpoint {
  NetworkAddressFamily family{NetworkAddressFamily::kIpv4};
  std::array<std::uint8_t, 16> address{};
  std::uint32_t scope_id{};
  std::uint16_t port{};

  [[nodiscard]] static NetworkEndpoint V4(const std::array<std::uint8_t, 4>& address,
                                          std::uint16_t port) noexcept;
  [[nodiscard]] static NetworkEndpoint V6(const std::array<std::uint8_t, 16>& address,
                                          std::uint32_t scope_id,
                                          std::uint16_t port) noexcept;
};

using ConnectionId = AttemptHandle;

enum class ConnectionIoError : std::uint8_t {
  kNone,
  kInvalidArgument,
  kBusy,
  kClosed,
  kTransportFailure,
};

class AuthenticatedEstablishedConnection final {
 public:
  using ChannelHandler = std::function<void(EstablishedTlsChannel&)>;
  using ReadHandler = std::function<void(ConnectionIoError, Bytes)>;
  using WriteHandler = std::function<void(ConnectionIoError)>;

  ~AuthenticatedEstablishedConnection();

  AuthenticatedEstablishedConnection(const AuthenticatedEstablishedConnection&) =
      delete;
  AuthenticatedEstablishedConnection& operator=(
      const AuthenticatedEstablishedConnection&) = delete;
  AuthenticatedEstablishedConnection(AuthenticatedEstablishedConnection&&) = delete;
  AuthenticatedEstablishedConnection& operator=(AuthenticatedEstablishedConnection&&) =
      delete;

  [[nodiscard]] const ConnectionId& id() const noexcept;
  [[nodiscard]] const DeviceId& peer_device_id() const noexcept;
  [[nodiscard]] bool inbound() const noexcept;
  [[nodiscard]] bool open() const noexcept;

  // ChannelHandler runs on the network executor and must not block. Read and
  // write completions run on the serialized callback executor. All handlers
  // are discarded by the runtime stop barrier. Only one read may be
  // outstanding; writes are serialized in call order.
  [[nodiscard]] bool DispatchChannel(ChannelHandler handler);
  [[nodiscard]] bool ReadSome(std::size_t maximum_bytes, ReadHandler handler);
  [[nodiscard]] bool Write(Bytes bytes, WriteHandler handler);
  void Close();

 private:
  class Impl;
  struct Construction;
  class RuntimeLease final {
   public:
    [[nodiscard]] const ConnectionId& id() const noexcept;
    void Close();
    void Stop();

   private:
    explicit RuntimeLease(std::shared_ptr<Impl> implementation);
    std::shared_ptr<Impl> implementation_;

    friend class AuthenticatedEstablishedConnection;
  };

  explicit AuthenticatedEstablishedConnection(std::shared_ptr<Impl> implementation);
  [[nodiscard]] static std::shared_ptr<AuthenticatedEstablishedConnection> Create(
      std::unique_ptr<Construction> construction,
      std::shared_ptr<RuntimeLease>& runtime_lease);

  std::shared_ptr<Impl> implementation_;

  friend class AuthenticatedConnectionRuntime;
};

struct PairingRuntimeEvent {
  ConnectionId connection_id{};
  AttemptHandle attempt{};
  std::uint64_t request_id{};
  PairingUpdate update{};
  bool inbound{};
};

struct EstablishedRuntimeEvent {
  std::uint64_t request_id{};
  bool inbound{};
  ConnectionIoError io_error{ConnectionIoError::kNone};
  security::tls::SecurityError security_error{security::tls::SecurityError::kNone};
  std::shared_ptr<AuthenticatedEstablishedConnection> connection{};
};

struct AuthenticatedConnectionRuntimeConfig {
  std::uint16_t listen_port{};
  std::uint64_t tls_handshake_timeout_ms{kDefaultTlsHandshakeTimeoutMs};
  std::size_t pairing_write_fragment_bytes{kMaxPairingFrameSize};
  NegotiationOffer pairing_offer{};
};

class AuthenticatedConnectionRuntime final {
 public:
  // Runtime handlers are serialized on a callback executor isolated from
  // sockets and timers. Stop waits for a handler already in flight, so every
  // handler must eventually return.
  using PairingHandler = std::function<void(const PairingRuntimeEvent&)>;
  using EstablishedHandler = std::function<void(const EstablishedRuntimeEvent&)>;

  AuthenticatedConnectionRuntime(security::identity::IdentityRepository& repository,
                                 AuthenticatedConnectionRuntimeConfig config,
                                 PairingHandler pairing_handler,
                                 EstablishedHandler established_handler);
  ~AuthenticatedConnectionRuntime();

  AuthenticatedConnectionRuntime(const AuthenticatedConnectionRuntime&) = delete;
  AuthenticatedConnectionRuntime& operator=(const AuthenticatedConnectionRuntime&) =
      delete;
  AuthenticatedConnectionRuntime(AuthenticatedConnectionRuntime&&) = delete;
  AuthenticatedConnectionRuntime& operator=(AuthenticatedConnectionRuntime&&) = delete;

  [[nodiscard]] bool Start();
  void Stop();
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] std::uint16_t listen_port() const noexcept;

  [[nodiscard]] bool OpenPairingWindow(std::uint64_t duration_ms);
  void ClosePairingWindow();
  [[nodiscard]] bool StartPairing(const NetworkEndpoint& endpoint,
                                  std::uint64_t request_id,
                                  std::string peer_display_label);
  [[nodiscard]] bool Decide(const AttemptHandle& attempt,
                            security::tls::ConfirmationDecision decision);
  [[nodiscard]] bool Cancel(const AttemptHandle& attempt);

  [[nodiscard]] bool OpenEstablished(const NetworkEndpoint& endpoint,
                                     const DeviceId& peer_device_id,
                                     std::uint64_t request_id);

 private:
  class Impl;
  std::shared_ptr<Impl> implementation_;
};

}  // namespace xnn_transfer::core::session

#endif  // XNN_TRANSFER_CORE_SESSION_CONNECTION_RUNTIME_HPP_
