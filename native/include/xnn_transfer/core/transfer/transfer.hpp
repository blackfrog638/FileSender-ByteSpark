#ifndef XNN_TRANSFER_CORE_TRANSFER_TRANSFER_HPP_
#define XNN_TRANSFER_CORE_TRANSFER_TRANSFER_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xnn_transfer/core/security/tls/security_profile.hpp"
#include "xnn_transfer/core/session/session.hpp"
#include "xnn_transfer/core/storage/storage.hpp"
#include "xnn_transfer/protocol/v1_parser.hpp"

namespace xnn_transfer::core::transfer {

inline constexpr std::size_t kTransferIdSize = 16;
inline constexpr std::uint32_t kMinimumChunkSize = 4'096;
inline constexpr std::uint32_t kMaximumChunkSize = 1'048'000;
inline constexpr std::uint32_t kMaximumTransferWindow = 16'777'216;
inline constexpr std::uint32_t kMaximumConnectionWindow = 67'108'864;
inline constexpr std::uint64_t kManifestProgressTimeoutMs = 30'000;
inline constexpr std::uint64_t kManifestTimeoutMs = 300'000;
inline constexpr std::uint64_t kDecisionTimeoutMs = 300'000;
inline constexpr std::uint64_t kDataProgressTimeoutMs = 60'000;
inline constexpr std::uint64_t kAcknowledgementTimeoutMs = 30'000;
inline constexpr std::uint64_t kCancelAcknowledgementTimeoutMs = 10'000;

using Bytes = std::vector<std::uint8_t>;
using TransferId = std::array<std::uint8_t, kTransferIdSize>;

enum class TransferState : std::uint8_t {
  kCreated,
  kSendingManifest,
  kReceivingManifest,
  kAwaitingDecision,
  kSendingFile,
  kReceivingFile,
  kRejecting,
  kAwaitingFileCommit,
  kAwaitingCompletion,
  kCompleting,
  kCancelling,
  kCommitted,
  kCompleted,
  kCancelled,
  kRejected,
  kFailed,
};

enum class TransferError : std::uint16_t {
  kNone,
  kInvalidArgument,
  kUnauthenticated,
  kMalformedFrame,
  kMalformedMessage,
  kStateViolation,
  kMessageIdViolation,
  kLimitExceeded,
  kInvalidOffer,
  kInvalidManifest,
  kPolicyRejected,
  kNoSpace,
  kBusy,
  kIoFailure,
  kIntegrityFailed,
  kTimeout,
  kCancelled,
  kIdempotencyConflict,
  kSourceFailure,
  kInternalFailure,
};

enum class WireErrorCode : std::uint16_t {
  kNone = 0,
  kMalformedFrame = 0x0001,
  kFrameTooLarge = 0x0002,
  kMalformedMessage = 0x0003,
  kUnsupportedVersion = 0x0004,
  kDowngradeDetected = 0x0005,
  kUnsupportedCapability = 0x0006,
  kUnsupportedMessage = 0x0007,
  kUnknownCriticalField = 0x0008,
  kStateViolation = 0x0009,
  kMessageIdViolation = 0x000a,
  kLimitExceeded = 0x000b,
  kTimeout = 0x000c,
  kInvalidOffer = 0x0100,
  kInvalidManifest = 0x0101,
  kPolicyRejected = 0x0102,
  kNoSpace = 0x0103,
  kBusy = 0x0104,
  kIoFailure = 0x0105,
  kIntegrityFailed = 0x0106,
  kCancelled = 0x0107,
  kResumeUnavailable = 0x0108,
  kExpired = 0x0109,
  kIdempotencyConflict = 0x010a,
  kCompleted = 0x010b,
};

[[nodiscard]] std::string_view TransferErrorName(TransferError error) noexcept;

struct TransferLimits {
  std::uint32_t maximum_body{1'048'576};
  std::uint32_t maximum_in_flight{kMaximumConnectionWindow};
  std::uint16_t maximum_active_streams{32};
  std::uint32_t preferred_chunk_size{65'536};
  std::uint32_t preferred_initial_window{262'144};
};

struct TransferContext {
  session::SessionAuthority* authority{};
  session::SessionHandle session_handle{};
  security::tls::Role local_role{security::tls::Role::kInitiator};
  protocol::v1::Version version{};
  std::uint32_t stream_id{};
  TransferLimits limits{};
};

struct OneFileManifest {
  TransferId transfer_id{};
  std::string relative_path{};
  std::uint64_t file_size{};
  Bytes file_commitment{};
  Bytes manifest_commitment{};
  std::string display_name{};
};

struct IncomingOffer {
  TransferId transfer_id{};
  std::string relative_path{};
  std::uint64_t file_size{};
  std::string display_name{};
};

struct TransferUpdate {
  TransferState state{TransferState::kCreated};
  TransferError error{TransferError::kNone};
  WireErrorCode wire_error{WireErrorCode::kNone};
  Bytes outbound_frame{};
  std::optional<IncomingOffer> offer{};
  bool terminal{};
  bool connection_fatal{};
  bool retryable{};
};

// One instance is shared by every control and transfer stream on one endpoint.
// It validates sequence numbers but does not serialize transport publication.
class ConnectionMessageSequence final {
 public:
  explicit ConnectionMessageSequence(std::uint64_t next_outbound = 1,
                                     std::uint64_t next_inbound = 1) noexcept;

  [[nodiscard]] bool NextOutbound(std::uint64_t& output) noexcept;
  [[nodiscard]] bool ObserveInbound(std::uint64_t message_id) noexcept;
  [[nodiscard]] bool OutboundExhausted() const noexcept;

  ConnectionMessageSequence(const ConnectionMessageSequence&) = delete;
  ConnectionMessageSequence& operator=(const ConnectionMessageSequence&) = delete;
  ConnectionMessageSequence(ConnectionMessageSequence&&) = delete;
  ConnectionMessageSequence& operator=(ConnectionMessageSequence&&) = delete;

 private:
  mutable std::mutex mutex_{};
  std::uint64_t next_outbound_{};
  std::uint64_t next_inbound_{};
  bool outbound_exhausted_{};
  bool inbound_exhausted_{};
};

enum class FileSourceError : std::uint8_t {
  kNone,
  kOutOfRange,
  kIoFailure,
};

struct FileReadResult {
  FileSourceError error{FileSourceError::kNone};
  Bytes data{};

  [[nodiscard]] bool ok() const noexcept { return error == FileSourceError::kNone; }
};

class FileSource {
 public:
  virtual ~FileSource() = default;
  [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
  [[nodiscard]] virtual FileReadResult Read(std::uint64_t offset,
                                            std::size_t maximum_bytes) = 0;
};

struct ManifestVerificationInput {
  const OneFileManifest* manifest{};
  std::span<const std::uint8_t> offer_frame{};
  std::span<const std::uint8_t> entry_frame{};
  std::span<const std::uint8_t> end_frame{};
};

class TransferIntegrityProvider {
 public:
  virtual ~TransferIntegrityProvider() = default;

  [[nodiscard]] virtual bool VerifyManifest(const ManifestVerificationInput& input) = 0;
  [[nodiscard]] virtual bool BuildChunkCommitment(const TransferId& transfer_id,
                                                  std::uint32_t entry_index,
                                                  std::uint64_t offset,
                                                  std::span<const std::uint8_t> data,
                                                  Bytes& output) = 0;
  [[nodiscard]] virtual bool VerifyChunkCommitment(
      const TransferId& transfer_id, std::uint32_t entry_index, std::uint64_t offset,
      std::span<const std::uint8_t> data, std::span<const std::uint8_t> commitment) = 0;
  [[nodiscard]] virtual std::unique_ptr<storage::StreamingIntegrityVerifier>
  CreateFileVerifier(const TransferId& transfer_id, std::uint32_t entry_index,
                     std::span<const std::uint8_t> expected_commitment) = 0;
};

enum class CreditDirection : std::uint8_t {
  kOutbound,
  kInbound,
};

enum class StreamOpenResult : std::uint8_t {
  kOpened,
  kCapacityRejected,
  kInvalid,
};

// One instance is shared by every transfer stream on one endpoint.
class ConnectionCreditBudget final {
 public:
  explicit ConnectionCreditBudget(std::uint32_t limit_bytes,
                                  std::uint16_t maximum_streams = 32) noexcept;

  [[nodiscard]] bool TryOpenStream(std::uint32_t stream_id, bool local_is_creator,
                                   security::tls::Role local_role) noexcept;
  [[nodiscard]] StreamOpenResult OpenStream(std::uint32_t stream_id,
                                            bool local_is_creator,
                                            security::tls::Role local_role) noexcept;
  void CloseStream(std::uint32_t stream_id) noexcept;
  [[nodiscard]] bool TryReserve(CreditDirection direction,
                                std::uint32_t bytes) noexcept;
  [[nodiscard]] bool TryAdjust(CreditDirection direction, std::uint32_t release_bytes,
                               std::uint32_t reserve_bytes) noexcept;
  void Release(CreditDirection direction, std::uint32_t bytes) noexcept;
  [[nodiscard]] std::uint32_t limit_bytes() const noexcept;
  [[nodiscard]] std::uint32_t reserved_bytes(CreditDirection direction) const noexcept;
  [[nodiscard]] std::uint16_t maximum_streams() const noexcept;
  [[nodiscard]] std::uint16_t active_streams() const noexcept;

 private:
  mutable std::mutex mutex_{};
  std::uint32_t limit_bytes_{};
  std::array<std::uint32_t, 2> reserved_bytes_{};
  std::uint16_t maximum_streams_{};
  std::uint16_t active_streams_{};
  std::array<std::uint32_t, 32> active_stream_ids_{};
  security::tls::Role local_role_{security::tls::Role::kInitiator};
  std::uint32_t next_local_stream_id_{};
  std::uint32_t next_peer_stream_id_{};
  bool role_initialized_{};
  bool local_stream_ids_exhausted_{};
  bool peer_stream_ids_exhausted_{};
};

class OneFileSender final {
 public:
  [[nodiscard]] static TransferUpdate Create(TransferContext context,
                                             OneFileManifest manifest,
                                             std::unique_ptr<FileSource> source,
                                             ConnectionMessageSequence& message_ids,
                                             TransferIntegrityProvider& integrity,
                                             ConnectionCreditBudget& connection_credit,
                                             std::unique_ptr<OneFileSender>& output);

  ~OneFileSender();

  OneFileSender(const OneFileSender&) = delete;
  OneFileSender& operator=(const OneFileSender&) = delete;
  OneFileSender(OneFileSender&&) = delete;
  OneFileSender& operator=(OneFileSender&&) = delete;

  [[nodiscard]] TransferUpdate Start(std::uint64_t now_ms);
  [[nodiscard]] TransferUpdate NextOutbound(std::uint64_t now_ms);
  [[nodiscard]] TransferUpdate ReceiveFrame(std::span<const std::uint8_t> encoded,
                                            std::uint64_t now_ms);
  [[nodiscard]] TransferUpdate Advance(std::uint64_t now_ms);
  [[nodiscard]] TransferUpdate Cancel(std::uint64_t now_ms);
  [[nodiscard]] TransferUpdate Shutdown();
  [[nodiscard]] TransferState state() const;

 private:
  struct Implementation;
  explicit OneFileSender(std::unique_ptr<Implementation> implementation);

  std::unique_ptr<Implementation> implementation_;
};

class OneFileReceiver final {
 public:
  [[nodiscard]] static TransferUpdate Create(
      TransferContext context, ConnectionMessageSequence& message_ids,
      TransferIntegrityProvider& integrity,
      std::shared_ptr<storage::TemporaryBudget> temporary_budget,
      ConnectionCreditBudget& connection_credit, storage::PlatformBackend& platform,
      std::unique_ptr<OneFileReceiver>& output);

  ~OneFileReceiver();

  OneFileReceiver(const OneFileReceiver&) = delete;
  OneFileReceiver& operator=(const OneFileReceiver&) = delete;
  OneFileReceiver(OneFileReceiver&&) = delete;
  OneFileReceiver& operator=(OneFileReceiver&&) = delete;

  [[nodiscard]] TransferUpdate ReceiveFrame(std::span<const std::uint8_t> encoded,
                                            std::uint64_t now_ms);
  [[nodiscard]] TransferUpdate NextOutbound(std::uint64_t now_ms);
  [[nodiscard]] TransferUpdate ConfirmChunkAckWritten(std::uint64_t next_offset,
                                                      std::uint64_t now_ms);
  [[nodiscard]] TransferUpdate Accept(std::uint32_t chunk_size,
                                      std::uint32_t initial_window,
                                      std::uint64_t now_ms);
  [[nodiscard]] TransferUpdate Reject(WireErrorCode code, bool retryable,
                                      std::uint64_t now_ms);
  [[nodiscard]] TransferUpdate Advance(std::uint64_t now_ms);
  [[nodiscard]] TransferUpdate Cancel(std::uint64_t now_ms);
  [[nodiscard]] TransferUpdate Shutdown();
  [[nodiscard]] TransferState state() const;

 private:
  struct Implementation;
  explicit OneFileReceiver(std::unique_ptr<Implementation> implementation);

  std::unique_ptr<Implementation> implementation_;
};

}  // namespace xnn_transfer::core::transfer

#endif  // XNN_TRANSFER_CORE_TRANSFER_TRANSFER_HPP_
