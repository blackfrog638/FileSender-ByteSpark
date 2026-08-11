#ifndef XNN_TRANSFER_CORE_SESSION_CONNECTION_RUNTIME_INTERNAL_HPP_
#define XNN_TRANSFER_CORE_SESSION_CONNECTION_RUNTIME_INTERNAL_HPP_

#include <array>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/steady_timer.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "xnn_transfer/core/security/tls/tls_provider.hpp"
#include "xnn_transfer/core/session/connection_runtime.hpp"

namespace xnn_transfer::core::session::runtime_internal {

using Tcp = asio::ip::tcp;
using TlsStream = asio::ssl::stream<Tcp::socket>;

enum class CallbackDispatchResult : std::uint8_t {
  kQueued,
  kClosed,
  kFailed,
};

class PairingAdmissionBridge final {
 public:
  [[nodiscard]] static PairingAdmissionController ProcessScoped() {
    return PairingAdmissionController::ProcessScoped();
  }

  [[nodiscard]] static bool ResetProcessStateForTesting() {
    return PairingAdmissionController::ResetProcessStateForTesting();
  }

  static void Retire(PairingAdmissionController& controller) noexcept {
    controller.RetireOwner();
  }

  [[nodiscard]] static std::unique_ptr<PairingAdmissionLease> Reserve(
      PairingAdmissionController& controller, const AttemptHandle& connection_id,
      const SourceToken& source, const bool user_initiated,
      const std::uint64_t now_ms) {
    return controller.ReserveHandshake(connection_id, source, user_initiated, now_ms);
  }

  [[nodiscard]] static PairingAdmissionResult Bind(
      PairingAdmissionController& controller,
      std::unique_ptr<PairingAdmissionLease> lease,
      const PairingAdmissionRequest& request,
      const std::uint64_t pairing_window_generation) {
    return controller.Bind(std::move(lease), request, pairing_window_generation);
  }

  [[nodiscard]] static std::uint64_t WindowGeneration(
      const PairingAdmissionController& controller,
      const std::uint64_t now_ms) noexcept {
    return controller.window_generation(now_ms);
  }
};

class PairingSocket final : public std::enable_shared_from_this<PairingSocket> {
 public:
  using EventHandler = std::function<void(PairingSocket&, PairingUpdate)>;
  using CloseHandler = std::function<void(const ConnectionId&)>;

  PairingSocket(std::shared_ptr<security::tls::OpenSslTlsContext> context,
                std::unique_ptr<TlsStream> stream,
                std::unique_ptr<PairingAttempt> attempt, ConnectionId connection_id,
                AttemptHandle attempt_handle, std::uint64_t request_id, bool inbound,
                std::size_t write_fragment_bytes, EventHandler event_handler,
                CloseHandler close_handler);

  void Start();
  void Decide(security::tls::ConfirmationDecision decision);
  void Cancel();
  void Stop(PairingError error, bool publish);

  [[nodiscard]] const ConnectionId& connection_id() const noexcept;
  [[nodiscard]] const AttemptHandle& attempt_handle() const noexcept;
  [[nodiscard]] std::uint64_t request_id() const noexcept;
  [[nodiscard]] bool inbound() const noexcept;

 private:
  enum class WriteAction {
    kNone,
    kSelectionAck,
    kConfirmedDecision,
  };

  struct PendingWrite {
    Bytes bytes{};
    std::size_t offset{};
    WriteAction action{WriteAction::kNone};
  };

  void StartRead();
  [[nodiscard]] bool ProcessFrames();
  void Apply(PairingUpdate update);
  void StartWrite();
  void ArmTimer();
  void ArmFrameAssembly();
  void CancelFrameAssembly();
  void ArmTerminalFlush();
  void ShortenTerminalFlushAfterWrite();
  void Fail(PairingError error);
  void CloseTransport();

  std::shared_ptr<security::tls::OpenSslTlsContext> context_;
  std::unique_ptr<TlsStream> stream_;
  std::unique_ptr<PairingAttempt> attempt_;
  asio::steady_timer timer_;
  asio::steady_timer frame_timer_;
  asio::steady_timer terminal_flush_timer_;
  ConnectionId connection_id_{};
  AttemptHandle attempt_handle_{};
  std::uint64_t request_id_{};
  bool inbound_{};
  std::size_t write_fragment_bytes_{};
  EventHandler event_handler_{};
  CloseHandler close_handler_{};
  std::array<std::uint8_t, 2'048> read_buffer_{};
  Bytes inbound_bytes_{};
  std::uint32_t next_inbound_sequence_{1};
  std::size_t inbound_frames_{};
  std::size_t inbound_frame_bytes_{};
  std::deque<PendingWrite> writes_{};
  bool write_active_{};
  bool frame_assembly_active_{};
  bool terminal_flush_active_{};
  bool terminal_{};
  bool closed_{};
};

class TlsHandshake final : public std::enable_shared_from_this<TlsHandshake> {
 public:
  enum class Mode {
    kServerUnknown,
    kClientPairing,
    kClientEstablished,
  };

  using CompletionHandler = std::function<void(const std::shared_ptr<TlsHandshake>&)>;
  using FailureHandler =
      std::function<void(const std::shared_ptr<TlsHandshake>&, ConnectionIoError,
                         security::tls::SecurityError, bool)>;

  TlsHandshake(std::shared_ptr<security::tls::OpenSslTlsContext> context,
               std::unique_ptr<TlsStream> stream, ConnectionId connection_id,
               Tcp::endpoint peer_endpoint, Mode mode, std::uint64_t request_id,
               std::string peer_display_label, std::optional<DeviceId> expected_peer,
               std::unique_ptr<PairingAdmissionLease> admission,
               std::uint64_t pairing_window_generation, std::uint64_t timeout_ms,
               CompletionHandler completion_handler, FailureHandler failure_handler);

  void StartServer();
  void StartClient(const Tcp::endpoint& endpoint);
  void Stop();
  [[nodiscard]] std::unique_ptr<TlsStream> TakeStream();
  [[nodiscard]] std::unique_ptr<PairingAdmissionLease> TakeAdmission();

  [[nodiscard]] const std::shared_ptr<security::tls::OpenSslTlsContext>& context()
      const noexcept;
  [[nodiscard]] const ConnectionId& connection_id() const noexcept;
  [[nodiscard]] const Tcp::endpoint& peer_endpoint() const noexcept;
  [[nodiscard]] Mode mode() const noexcept;
  [[nodiscard]] std::uint64_t request_id() const noexcept;
  [[nodiscard]] std::uint64_t pairing_window_generation() const noexcept;
  [[nodiscard]] const std::string& peer_display_label() const noexcept;
  [[nodiscard]] const std::optional<DeviceId>& expected_peer() const noexcept;

 private:
  void ArmTimeout();
  void Complete(const asio::error_code& error);

  std::shared_ptr<security::tls::OpenSslTlsContext> context_;
  std::unique_ptr<TlsStream> stream_;
  asio::steady_timer timer_;
  ConnectionId connection_id_{};
  Tcp::endpoint peer_endpoint_{};
  Mode mode_{Mode::kServerUnknown};
  std::uint64_t request_id_{};
  std::string peer_display_label_{};
  std::optional<DeviceId> expected_peer_{};
  std::unique_ptr<PairingAdmissionLease> admission_{};
  std::uint64_t pairing_window_generation_{};
  std::uint64_t timeout_ms_{};
  CompletionHandler completion_handler_{};
  FailureHandler failure_handler_{};
  bool finished_{};
};

}  // namespace xnn_transfer::core::session::runtime_internal

namespace xnn_transfer::core::session {

struct AuthenticatedEstablishedConnection::Construction {
  std::shared_ptr<void> executor_owner{};
  std::shared_ptr<security::tls::OpenSslTlsContext> context{};
  std::unique_ptr<runtime_internal::TlsStream> stream{};
  std::unique_ptr<EstablishedTlsChannel> channel{};
  ConnectionId connection_id{};
  bool inbound{};
  std::function<void(const ConnectionId&)> close_handler{};
  std::function<bool()> completion_reserver{};
  std::function<void()> completion_releaser{};
  std::function<runtime_internal::CallbackDispatchResult(std::function<void()>&)>
      callback_dispatcher{};
  std::function<runtime_internal::CallbackDispatchResult(const void*,
                                                         std::function<void()>&)>
      network_dispatcher{};
  std::function<void(const void*)> network_canceller{};
};

}  // namespace xnn_transfer::core::session

#endif  // XNN_TRANSFER_CORE_SESSION_CONNECTION_RUNTIME_INTERNAL_HPP_
