#ifndef XNN_TRANSFER_CORE_SESSION_SESSION_HPP_
#define XNN_TRANSFER_CORE_SESSION_SESSION_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xnn_transfer/core/security/identity/identity_repository.hpp"
#include "xnn_transfer/core/security/tls/security_profile.hpp"
#include "xnn_transfer/core/security/tls/tls_provider.hpp"

struct ssl_st;

namespace xnn_transfer::core::session {

namespace detail {
struct PairingAdmissionState;
struct PairingReplayState;
}  // namespace detail
namespace runtime_internal {
class PairingAdmissionBridge;
}  // namespace runtime_internal

inline constexpr std::size_t kAttemptHandleSize = 16;
inline constexpr std::size_t kPairingFrameHeaderSize = 20;
inline constexpr std::size_t kMaxPairingBodySize = 4'096;
inline constexpr std::size_t kMaxPairingFrameSize =
    kPairingFrameHeaderSize + kMaxPairingBodySize;
inline constexpr std::size_t kMaxPairingFields = 16;
inline constexpr std::size_t kMaxInboundPairingFrames = 16;
inline constexpr std::size_t kMaxInboundPairingBytes = 65'536;
inline constexpr std::size_t kMaxReplayEntries = 1'024;
inline constexpr std::size_t kMaxReplayEntriesPerPeer = 32;
inline constexpr std::uint64_t kReplayRetentionMs = 600'000;
inline constexpr std::uint64_t kMaximumPairingWindowMs = 120'000;
inline constexpr std::uint64_t kFirstPairingFrameTimeoutMs = 5'000;
inline constexpr std::uint64_t kSelectionTimeoutMs = 10'000;
inline constexpr std::uint64_t kPairingIdleTimeoutMs = 30'000;
inline constexpr std::uint64_t kDecisionTimeoutMs = 90'000;
inline constexpr std::uint64_t kCompletePairingTimeoutMs = 120'000;
inline constexpr std::size_t kMaxIncompletePairingHandshakes = 8;
inline constexpr std::size_t kMaxIncompletePairingHandshakesPerSource = 2;
inline constexpr std::size_t kReservedUserInitiatedPairingSlots = 1;
inline constexpr std::size_t kMaxVisiblePairingAttempts = 1;
inline constexpr std::size_t kGlobalAdmissionBucketCapacity = 16;
inline constexpr std::size_t kSourceAdmissionBucketCapacity = 4;
inline constexpr std::uint64_t kGlobalAdmissionRefillMs = 1'000;
inline constexpr std::uint64_t kSourceAdmissionRefillMs = 15'000;

using Bytes = std::vector<std::uint8_t>;
using AttemptHandle = std::array<std::uint8_t, kAttemptHandleSize>;
using SessionHandle = std::array<std::uint8_t, kAttemptHandleSize>;
using SourceToken = std::array<std::uint8_t, kAttemptHandleSize>;
using PublicKey = security::identity::PublicKey;
using DeviceId = security::identity::DeviceId;
using Digest256 = security::tls::Digest256;

enum class PairingError : std::uint16_t {
  kNone = 0,
  kMalformed = 0x1001,
  kLimitExceeded = 0x1002,
  kSequenceViolation = 0x1003,
  kUnsupportedVersion = 0x1004,
  kUnsupportedProfile = 0x1005,
  kDowngradeDetected = 0x1006,
  kRoleMismatch = 0x1007,
  kStateViolation = 0x1008,
  kReplayDetected = 0x1009,
  kConfirmationFailed = 0x100a,
  kAuthenticatedReject = 0x100b,
  kLocalReject = 0x100c,
  kAlreadyDecided = 0x100d,
  kCancelled = 0x100e,
  kTimeout = 0x100f,
  kBusy = 0x1010,
  kCertificateRejected = 0x1011,
  kInternalFailure = 0x1012,
};

[[nodiscard]] std::string_view PairingErrorName(PairingError error) noexcept;

enum class PairingState : std::uint8_t {
  kPairingTls,
  kExchangingHellos,
  kSelecting,
  kAwaitingDecisions,
  kReadyToCommit,
  kCommitting,
  kPairedLocal,
  kClosed,
};

struct Version {
  std::uint8_t major{1};
  std::uint8_t minor{0};

  friend constexpr bool operator==(Version, Version) = default;
};

struct VersionRange {
  Version minimum{};
  Version maximum{};
};

struct ReceiveLimits {
  std::uint32_t max_body{};
  std::uint32_t max_in_flight{};
  std::uint16_t max_streams{};

  friend constexpr bool operator==(ReceiveLimits, ReceiveLimits) = default;
};

struct NegotiationOffer {
  VersionRange versions{};
  std::vector<std::uint32_t> offered_capabilities{};
  std::vector<std::uint32_t> required_capabilities{};
  ReceiveLimits receive_limits{};
};

struct PairingAttemptOptions {
  NegotiationOffer offer{};
  std::string peer_display_label{};
};

struct PairingPrompt {
  AttemptHandle handle{};
  std::array<std::uint16_t, 5> sas_word_indices{};
  std::uint64_t deadline_ms{};
};

struct PairingUpdate {
  PairingState state{PairingState::kPairingTls};
  PairingError error{PairingError::kNone};
  Bytes outbound_frame{};
  std::optional<PairingPrompt> prompt{};
  std::optional<DeviceId> paired_peer{};
  bool terminal{};
};

class SessionEntropy {
 public:
  virtual ~SessionEntropy() = default;
  [[nodiscard]] virtual bool Fill(std::span<std::uint8_t> output) = 0;
};

class OpenSslSessionEntropy final : public SessionEntropy {
 public:
  [[nodiscard]] bool Fill(std::span<std::uint8_t> output) override;
};

// This interface represents one already profile-verified, unpinned pairing
// TLS connection. It is consumed by exactly one PairingAttempt.
class PairingChannel {
 public:
  virtual ~PairingChannel() = default;

  [[nodiscard]] virtual const security::tls::ValidatedEd25519PublicKey&
  peer_public_key() const noexcept = 0;
  [[nodiscard]] virtual security::identity::Result<security::identity::SecretBuffer>
  ExportPairing(const security::tls::PairingContext& context) = 0;
  [[nodiscard]] virtual security::identity::Result<security::identity::SecretBuffer>
  ExportConfirmation(const security::tls::PairingContext& context) = 0;
};

class OpenSslPairingChannel final : public PairingChannel {
 public:
  [[nodiscard]] static security::tls::Result<std::unique_ptr<OpenSslPairingChannel>>
  Create(const security::tls::OpenSslTlsContext& context, ssl_st* connection);
  [[nodiscard]] static security::tls::Result<std::unique_ptr<OpenSslPairingChannel>>
  Create(const security::tls::OpenSslTlsContext& context, ssl_st* connection,
         security::tls::AcceptedPairingTlsConnection accepted);

  ~OpenSslPairingChannel() override;

  OpenSslPairingChannel(const OpenSslPairingChannel&) = delete;
  OpenSslPairingChannel& operator=(const OpenSslPairingChannel&) = delete;
  OpenSslPairingChannel(OpenSslPairingChannel&&) noexcept;
  OpenSslPairingChannel& operator=(OpenSslPairingChannel&&) noexcept;

  [[nodiscard]] const security::tls::ValidatedEd25519PublicKey& peer_public_key()
      const noexcept override;
  [[nodiscard]] security::identity::Result<security::identity::SecretBuffer>
  ExportPairing(const security::tls::PairingContext& context) override;
  [[nodiscard]] security::identity::Result<security::identity::SecretBuffer>
  ExportConfirmation(const security::tls::PairingContext& context) override;

 private:
  struct Implementation;
  explicit OpenSslPairingChannel(std::unique_ptr<Implementation> implementation);

  std::unique_ptr<Implementation> implementation_;
};

class PairingReplayReservation final {
 public:
  ~PairingReplayReservation();

  PairingReplayReservation(const PairingReplayReservation&) = delete;
  PairingReplayReservation& operator=(const PairingReplayReservation&) = delete;
  PairingReplayReservation(PairingReplayReservation&&) noexcept;
  PairingReplayReservation& operator=(PairingReplayReservation&&) noexcept;

  [[nodiscard]] PairingError Bind(const Digest256& context, std::uint64_t now_ms);
  void Commit(std::uint64_t now_ms) noexcept;
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool bound() const noexcept;

 private:
  PairingReplayReservation(std::shared_ptr<detail::PairingReplayState> state,
                           std::uint64_t reservation_id);

  std::shared_ptr<detail::PairingReplayState> state_;
  std::uint64_t reservation_id_{};

  friend class PairingReplayCache;
};

class PairingReplayCache final {
 public:
  PairingReplayCache();
  ~PairingReplayCache();

  PairingReplayCache(const PairingReplayCache&) = delete;
  PairingReplayCache& operator=(const PairingReplayCache&) = delete;
  PairingReplayCache(PairingReplayCache&&) noexcept;
  PairingReplayCache& operator=(PairingReplayCache&&) noexcept;

  [[nodiscard]] bool Contains(const Digest256& context, const PublicKey& peer_key,
                              std::uint64_t now_ms);
  [[nodiscard]] std::unique_ptr<PairingReplayReservation> Reserve(
      const PublicKey& peer_key, std::uint64_t now_ms);
  [[nodiscard]] bool Remember(const Digest256& context, const PublicKey& peer_key,
                              std::uint64_t now_ms);
  [[nodiscard]] std::size_t size() const noexcept;

 private:
  std::shared_ptr<detail::PairingReplayState> state_;
};

struct PairingAdmissionRequest {
  AttemptHandle connection_id{};
  SourceToken source{};
  PublicKey local_key{};
  PublicKey peer_key{};
  security::tls::Role local_role{security::tls::Role::kInitiator};
  bool user_initiated{};
  std::uint64_t now_ms{};
};

class PairingAdmissionLease final {
 public:
  ~PairingAdmissionLease();

  PairingAdmissionLease(const PairingAdmissionLease&) = delete;
  PairingAdmissionLease& operator=(const PairingAdmissionLease&) = delete;
  PairingAdmissionLease(PairingAdmissionLease&&) noexcept;
  PairingAdmissionLease& operator=(PairingAdmissionLease&&) noexcept;

  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] PairingError MarkVisible() noexcept;
  [[nodiscard]] const AttemptHandle& connection_id() const noexcept;
  [[nodiscard]] PublicKey local_key() const noexcept;
  [[nodiscard]] PublicKey peer_key() const noexcept;
  [[nodiscard]] security::tls::Role local_role() const noexcept;
  [[nodiscard]] std::uint64_t admitted_at_ms() const noexcept;
  [[nodiscard]] std::uint64_t window_deadline_ms() const noexcept;

 private:
  [[nodiscard]] bool bound() const noexcept;
  PairingAdmissionLease(std::shared_ptr<detail::PairingAdmissionState> state,
                        AttemptHandle connection_id, std::uint64_t lease_generation);

  std::shared_ptr<detail::PairingAdmissionState> state_;
  AttemptHandle connection_id_{};
  std::uint64_t lease_generation_{};

  friend class PairingAdmissionController;
  friend class PairingAttempt;
};

struct PairingAdmissionResult {
  PairingError error{PairingError::kNone};
  std::optional<AttemptHandle> displaced_connection{};
  std::unique_ptr<PairingAdmissionLease> lease{};

  [[nodiscard]] bool accepted() const noexcept {
    return error == PairingError::kNone && lease != nullptr;
  }
};

class PairingAdmissionController final {
 public:
  PairingAdmissionController();
  ~PairingAdmissionController();

  PairingAdmissionController(const PairingAdmissionController&) = delete;
  PairingAdmissionController& operator=(const PairingAdmissionController&) = delete;
  PairingAdmissionController(PairingAdmissionController&&) noexcept;
  PairingAdmissionController& operator=(PairingAdmissionController&&) noexcept;

  [[nodiscard]] bool OpenWindow(std::uint64_t now_ms, std::uint64_t duration_ms);
  void CloseWindow() noexcept;
  [[nodiscard]] PairingAdmissionResult Admit(const PairingAdmissionRequest& request);
  [[nodiscard]] bool window_open(std::uint64_t now_ms) const noexcept;
  [[nodiscard]] std::size_t active_connections() const noexcept;
  [[nodiscard]] std::size_t visible_attempts() const noexcept;

 private:
  PairingAdmissionController(std::shared_ptr<detail::PairingAdmissionState> state,
                             std::uint64_t owner_generation);
  [[nodiscard]] static PairingAdmissionController ProcessScoped();
  [[nodiscard]] static bool ResetProcessStateForTesting();
  void RetireOwner() noexcept;
  [[nodiscard]] std::uint64_t window_generation(std::uint64_t now_ms) const noexcept;
  [[nodiscard]] std::unique_ptr<PairingAdmissionLease> ReserveHandshake(
      const AttemptHandle& connection_id, const SourceToken& source,
      bool user_initiated, std::uint64_t now_ms);
  [[nodiscard]] PairingAdmissionResult Bind(
      std::unique_ptr<PairingAdmissionLease> lease,
      const PairingAdmissionRequest& request, std::uint64_t pairing_window_generation);

  std::shared_ptr<detail::PairingAdmissionState> state_;
  std::uint64_t owner_generation_{};

  friend class runtime_internal::PairingAdmissionBridge;
};

class PairingAttempt final {
 public:
  [[nodiscard]] static PairingUpdate Create(
      security::identity::IdentityRepository& repository,
      std::unique_ptr<PairingChannel> channel, SessionEntropy& entropy,
      std::unique_ptr<PairingAdmissionLease> admission,
      PairingReplayCache& replay_cache, PairingAttemptOptions options,
      std::unique_ptr<PairingAttempt>& output);

  ~PairingAttempt();

  PairingAttempt(const PairingAttempt&) = delete;
  PairingAttempt& operator=(const PairingAttempt&) = delete;
  PairingAttempt(PairingAttempt&&) = delete;
  PairingAttempt& operator=(PairingAttempt&&) = delete;

  [[nodiscard]] PairingUpdate Start(std::uint64_t now_ms);
  [[nodiscard]] PairingUpdate ReceiveFrame(std::span<const std::uint8_t> encoded,
                                           std::uint64_t now_ms);
  [[nodiscard]] PairingUpdate LocalSelectionAckWritten(const AttemptHandle& handle,
                                                       std::uint64_t now_ms);
  [[nodiscard]] PairingUpdate Decide(const AttemptHandle& handle,
                                     security::tls::ConfirmationDecision decision,
                                     std::uint64_t now_ms);
  [[nodiscard]] PairingUpdate LocalDecisionWritten(const AttemptHandle& handle,
                                                   std::uint64_t now_ms);
  [[nodiscard]] PairingUpdate Cancel(const AttemptHandle& handle, std::uint64_t now_ms);
  [[nodiscard]] PairingUpdate Advance(std::uint64_t now_ms);
  [[nodiscard]] PairingUpdate Shutdown();

  [[nodiscard]] PairingState state() const;
  [[nodiscard]] AttemptHandle handle() const;
  [[nodiscard]] std::optional<Digest256> pair_context() const;

 private:
  struct Implementation;
  explicit PairingAttempt(std::unique_ptr<Implementation> implementation);

  std::unique_ptr<Implementation> implementation_;
};

// A connection reaches this type only after OpenSslTlsContext verifies a fresh
// TLS 1.3 handshake against the exact active repository pin.
class EstablishedTlsChannel final {
 public:
  [[nodiscard]] static security::tls::Result<std::unique_ptr<EstablishedTlsChannel>>
  Create(const security::tls::OpenSslTlsContext& context, ssl_st* connection,
         const security::identity::IdentityRepository& repository,
         const DeviceId& peer_device_id);
  [[nodiscard]] static security::tls::Result<std::unique_ptr<EstablishedTlsChannel>>
  Create(const security::tls::OpenSslTlsContext& context, ssl_st* connection,
         security::tls::AcceptedEstablishedTlsConnection accepted);

  ~EstablishedTlsChannel();

  EstablishedTlsChannel(const EstablishedTlsChannel&) = delete;
  EstablishedTlsChannel& operator=(const EstablishedTlsChannel&) = delete;
  EstablishedTlsChannel(EstablishedTlsChannel&&) noexcept;
  EstablishedTlsChannel& operator=(EstablishedTlsChannel&&) noexcept;

  [[nodiscard]] const DeviceId& peer_device_id() const noexcept;
  [[nodiscard]] const PublicKey& peer_public_key() const noexcept;
  [[nodiscard]] std::uint16_t security_profile() const noexcept;
  [[nodiscard]] std::uint64_t peer_record_revision() const noexcept;
  [[nodiscard]] std::uint64_t identity_revision() const noexcept;
  [[nodiscard]] security::tls::Result<security::tls::TransportFinishedValue>
  BeginTransportBinding(const security::tls::TransportContext& context,
                        security::tls::Role local_role);
  [[nodiscard]] security::tls::SecurityError LocalTransportFinishedWritten() noexcept;
  [[nodiscard]] security::tls::SecurityError VerifyPeerTransportFinished(
      std::span<const std::uint8_t> message,
      std::span<const std::uint8_t> authenticator);
  [[nodiscard]] bool transport_bound() const noexcept;

 private:
  struct Implementation;
  explicit EstablishedTlsChannel(std::unique_ptr<Implementation> implementation);
  [[nodiscard]] bool current_transport() const noexcept;

  std::unique_ptr<Implementation> implementation_;

  friend class SessionAuthority;
};

enum class AuthorizationError : std::uint8_t {
  kNone,
  kInvalidArgument,
  kUnavailable,
  kUnauthenticated,
  kStale,
  kStorageFailure,
  kEntropyFailure,
};

struct AuthorizationResult {
  AuthorizationError error{AuthorizationError::kNone};
  std::optional<SessionHandle> handle{};

  [[nodiscard]] bool ok() const noexcept {
    return error == AuthorizationError::kNone && handle.has_value();
  }
};

class SessionAuthority final {
 public:
  SessionAuthority(security::identity::IdentityRepository& repository,
                   SessionEntropy& entropy);
  ~SessionAuthority();

  SessionAuthority(const SessionAuthority&) = delete;
  SessionAuthority& operator=(const SessionAuthority&) = delete;
  SessionAuthority(SessionAuthority&&) = delete;
  SessionAuthority& operator=(SessionAuthority&&) = delete;

  [[nodiscard]] AuthorizationResult Activate(
      std::unique_ptr<EstablishedTlsChannel> channel);
  [[nodiscard]] bool IsAuthorized(const SessionHandle& handle) const noexcept;
  [[nodiscard]] AuthorizationError Deactivate(const SessionHandle& handle) noexcept;
  [[nodiscard]] AuthorizationError Revoke(const SessionHandle& handle);
  void InvalidateStale() noexcept;
  void Shutdown() noexcept;
  [[nodiscard]] std::size_t active_sessions() const noexcept;

 private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

}  // namespace xnn_transfer::core::session

#endif  // XNN_TRANSFER_CORE_SESSION_SESSION_HPP_
