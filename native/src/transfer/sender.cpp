#include <algorithm>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

#include "internal.hpp"

namespace xnn_transfer::core::transfer {
namespace {

constexpr std::uint32_t kMaximumChunkBodyOverhead = 132;

[[nodiscard]] bool SameBytes(const std::span<const std::uint8_t> left,
                             const std::span<const std::uint8_t> right) noexcept {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin());
}

[[nodiscard]] bool AllowedInbound(const TransferState state,
                                  const protocol::v1::MessageType type) noexcept {
  if (type == protocol::v1::MessageType::kError) {
    return true;
  }
  switch (state) {
    case TransferState::kAwaitingDecision:
      return type == protocol::v1::MessageType::kTransferAccept ||
             type == protocol::v1::MessageType::kTransferReject;
    case TransferState::kSendingFile:
      return type == protocol::v1::MessageType::kChunkAck;
    case TransferState::kAwaitingFileCommit:
      return type == protocol::v1::MessageType::kFileCommit;
    case TransferState::kCompleting:
      return type == protocol::v1::MessageType::kTransferCompleteAck;
    case TransferState::kCreated:
    case TransferState::kSendingManifest:
    case TransferState::kReceivingManifest:
    case TransferState::kReceivingFile:
    case TransferState::kRejecting:
    case TransferState::kAwaitingCompletion:
    case TransferState::kCommitted:
    case TransferState::kCompleted:
    case TransferState::kRejected:
    case TransferState::kFailed:
      return false;
  }
  return false;
}

}  // namespace

struct OneFileSender::Implementation {
  [[nodiscard]] TransferUpdate Update() const {
    return {
        .state = state,
        .error = terminal_error,
        .wire_error = terminal_wire_error,
        .terminal = internal::IsTerminal(state),
        .connection_fatal = terminal_connection_fatal,
        .retryable = terminal_retryable,
    };
  }

  void RememberTerminal(const TransferError error, const WireErrorCode wire_error,
                        const bool connection_fatal = false,
                        const bool retryable = false) noexcept {
    terminal_error = error;
    terminal_wire_error = wire_error;
    terminal_connection_fatal = connection_fatal;
    terminal_retryable = retryable;
  }

  [[nodiscard]] TransferUpdate Fail(const TransferError error,
                                    const WireErrorCode wire_error,
                                    const bool connection_fatal = false) {
    ReleaseResources();
    state = TransferState::kFailed;
    RememberTerminal(error, wire_error, connection_fatal,
                     internal::WireErrorMayRetry(wire_error));
    TransferUpdate update =
        internal::FailureUpdate(state, error, wire_error, connection_fatal);
    update.retryable = internal::WireErrorMayRetry(wire_error);
    if (!connection_fatal && wire_error != WireErrorCode::kNone &&
        internal::Authorized(context)) {
      static_cast<void>(internal::EncodeErrorFrame(
          context, *message_ids, wire_error, update.retryable, update.outbound_frame));
    }
    return update;
  }

  void ReleaseResources() noexcept {
    if (credit_reserved) {
      connection_credit->Release(CreditDirection::kOutbound, reserved_credit);
      credit_reserved = false;
      reserved_credit = 0;
    }
    if (stream_reserved) {
      connection_credit->CloseStream(context.stream_id);
      stream_reserved = false;
    }
  }

  [[nodiscard]] bool Observe(const std::uint64_t now_ms) noexcept {
    return internal::ObserveTime(now_ms, last_now_ms);
  }

  [[nodiscard]] TransferUpdate CheckTimeout(const std::uint64_t now_ms) {
    if (state == TransferState::kSendingManifest) {
      if (internal::DeadlineReached(now_ms, manifest_deadline_ms) ||
          internal::DeadlineReached(now_ms, manifest_progress_deadline_ms)) {
        return Fail(TransferError::kTimeout, WireErrorCode::kTimeout);
      }
    }
    if (state == TransferState::kAwaitingDecision &&
        internal::DeadlineReached(now_ms, decision_deadline_ms)) {
      return Fail(TransferError::kTimeout, WireErrorCode::kTimeout);
    }
    if (state == TransferState::kSendingFile) {
      if (unacknowledged != 0 &&
          internal::DeadlineReached(now_ms, acknowledgement_deadline_ms)) {
        return Fail(TransferError::kTimeout, WireErrorCode::kTimeout);
      }
      if (internal::DeadlineReached(now_ms, data_deadline_ms)) {
        return Fail(TransferError::kTimeout, WireErrorCode::kTimeout);
      }
    }
    if ((state == TransferState::kAwaitingFileCommit ||
         state == TransferState::kCompleting) &&
        internal::DeadlineReached(now_ms, data_deadline_ms)) {
      return Fail(TransferError::kTimeout, WireErrorCode::kTimeout);
    }
    return Update();
  }

  [[nodiscard]] bool EncodeOffer(Bytes& output) {
    internal::BodyBuilder body;
    static_cast<void>(body.AddBytes(1, manifest.transfer_id));
    static_cast<void>(body.AddU32(2, 1));
    static_cast<void>(body.AddU64(3, manifest.file_size));
    static_cast<void>(body.AddBytes(4, manifest.manifest_commitment));
    if (!manifest.display_name.empty()) {
      static_cast<void>(body.AddUtf8(5, manifest.display_name, false));
    }
    return internal::EncodeFrame(context, protocol::v1::MessageType::kTransferOffer,
                                 *message_ids, body, output);
  }

  [[nodiscard]] bool EncodeManifestEntry(Bytes& output) {
    internal::BodyBuilder body;
    static_cast<void>(body.AddBytes(1, manifest.transfer_id));
    static_cast<void>(body.AddU32(2, 0));
    static_cast<void>(body.AddU8(3, 1));
    static_cast<void>(body.AddUtf8(4, manifest.relative_path));
    static_cast<void>(body.AddU64(5, manifest.file_size));
    static_cast<void>(body.AddBytes(6, manifest.file_commitment));
    return internal::EncodeFrame(context, protocol::v1::MessageType::kManifestEntry,
                                 *message_ids, body, output);
  }

  [[nodiscard]] bool EncodeManifestEnd(Bytes& output) {
    internal::BodyBuilder body;
    static_cast<void>(body.AddBytes(1, manifest.transfer_id));
    static_cast<void>(body.AddU32(2, 1));
    static_cast<void>(body.AddU64(3, manifest.file_size));
    static_cast<void>(body.AddBytes(4, manifest.manifest_commitment));
    return internal::EncodeFrame(context, protocol::v1::MessageType::kManifestEnd,
                                 *message_ids, body, output);
  }

  [[nodiscard]] bool EncodeFileBegin(Bytes& output) {
    internal::BodyBuilder body;
    static_cast<void>(body.AddBytes(1, manifest.transfer_id));
    static_cast<void>(body.AddU32(2, 0));
    static_cast<void>(body.AddU64(3, manifest.file_size));
    static_cast<void>(body.AddU32(4, chunk_size));
    return internal::EncodeFrame(context, protocol::v1::MessageType::kFileBegin,
                                 *message_ids, body, output);
  }

  [[nodiscard]] bool EncodeFileEnd(Bytes& output) {
    internal::BodyBuilder body;
    static_cast<void>(body.AddBytes(1, manifest.transfer_id));
    static_cast<void>(body.AddU32(2, 0));
    static_cast<void>(body.AddU64(3, manifest.file_size));
    static_cast<void>(body.AddBytes(4, manifest.file_commitment));
    return internal::EncodeFrame(context, protocol::v1::MessageType::kFileEnd,
                                 *message_ids, body, output);
  }

  [[nodiscard]] bool EncodeComplete(Bytes& output) {
    internal::BodyBuilder body;
    static_cast<void>(body.AddBytes(1, manifest.transfer_id));
    static_cast<void>(body.AddBytes(2, manifest.manifest_commitment));
    return internal::EncodeFrame(context, protocol::v1::MessageType::kTransferComplete,
                                 *message_ids, body, output);
  }

  mutable std::mutex mutex{};
  TransferContext context{};
  OneFileManifest manifest{};
  std::unique_ptr<FileSource> source{};
  ConnectionMessageSequence* message_ids{};
  TransferIntegrityProvider* integrity{};
  ConnectionCreditBudget* connection_credit{};
  TransferState state{TransferState::kCreated};
  std::size_t manifest_stage{};
  bool file_begin_sent{};
  bool file_end_sent{};
  std::uint32_t chunk_size{};
  std::uint32_t reserved_credit{};
  std::uint32_t available_credit{};
  std::uint64_t unacknowledged{};
  std::uint64_t bytes_sent{};
  std::uint64_t acknowledged_offset{};
  std::uint64_t last_now_ms{};
  std::uint64_t manifest_deadline_ms{};
  std::uint64_t manifest_progress_deadline_ms{};
  std::uint64_t decision_deadline_ms{};
  std::uint64_t acknowledgement_deadline_ms{};
  std::uint64_t data_deadline_ms{};
  bool stream_reserved{};
  bool credit_reserved{};
  TransferError terminal_error{TransferError::kNone};
  WireErrorCode terminal_wire_error{WireErrorCode::kNone};
  bool terminal_connection_fatal{};
  bool terminal_retryable{};
};

OneFileSender::OneFileSender(std::unique_ptr<Implementation> implementation)
    : implementation_(std::move(implementation)) {}

OneFileSender::~OneFileSender() {
  if (implementation_ != nullptr) {
    static_cast<void>(Shutdown());
  }
}

TransferUpdate OneFileSender::Create(TransferContext context, OneFileManifest manifest,
                                     std::unique_ptr<FileSource> source,
                                     ConnectionMessageSequence& message_ids,
                                     TransferIntegrityProvider& integrity,
                                     ConnectionCreditBudget& connection_credit,
                                     std::unique_ptr<OneFileSender>& output) {
  output.reset();
  if (!internal::ValidateContext(context, true) || !internal::Authorized(context) ||
      internal::AllZero(manifest.transfer_id) ||
      manifest.file_size > storage::kMaxFileBytes ||
      !internal::ValidCommitment(manifest.file_commitment) ||
      !internal::ValidCommitment(manifest.manifest_commitment) ||
      manifest.display_name.size() > 255U || source == nullptr ||
      source->size() != manifest.file_size || connection_credit.limit_bytes() == 0 ||
      connection_credit.limit_bytes() > context.limits.maximum_in_flight ||
      connection_credit.maximum_streams() == 0 ||
      connection_credit.maximum_streams() > context.limits.maximum_active_streams) {
    return internal::FailureUpdate(TransferState::kFailed,
                                   TransferError::kInvalidArgument,
                                   WireErrorCode::kInvalidOffer);
  }
  const storage::RequestValidationResult request = storage::ValidateReceiveRequest(
      {reinterpret_cast<const std::uint8_t*>(manifest.relative_path.data()),
       manifest.relative_path.size()},
      manifest.file_size);
  if (!request.ok()) {
    return internal::FailureUpdate(TransferState::kFailed,
                                   TransferError::kInvalidArgument,
                                   WireErrorCode::kInvalidManifest);
  }
  internal::BodyBuilder offer_body;
  static_cast<void>(offer_body.AddBytes(1, manifest.transfer_id));
  static_cast<void>(offer_body.AddU32(2, 1));
  static_cast<void>(offer_body.AddU64(3, manifest.file_size));
  static_cast<void>(offer_body.AddBytes(4, manifest.manifest_commitment));
  if (!manifest.display_name.empty()) {
    static_cast<void>(offer_body.AddUtf8(5, manifest.display_name, false));
  }
  ConnectionMessageSequence validation_ids;
  Bytes validated_offer;
  if (!internal::EncodeFrame(context, protocol::v1::MessageType::kTransferOffer,
                             validation_ids, offer_body, validated_offer)) {
    return internal::FailureUpdate(TransferState::kFailed,
                                   TransferError::kInvalidArgument,
                                   WireErrorCode::kInvalidOffer);
  }
  if (!connection_credit.TryOpenStream(context.stream_id, true, context.local_role)) {
    return internal::FailureUpdate(TransferState::kFailed, TransferError::kBusy,
                                   WireErrorCode::kBusy);
  }

  try {
    auto implementation = std::make_unique<Implementation>();
    implementation->context = context;
    implementation->manifest = std::move(manifest);
    implementation->source = std::move(source);
    implementation->message_ids = &message_ids;
    implementation->integrity = &integrity;
    implementation->connection_credit = &connection_credit;
    implementation->stream_reserved = true;
    output =
        std::unique_ptr<OneFileSender>(new OneFileSender(std::move(implementation)));
    return output->implementation_->Update();
  } catch (const std::bad_alloc&) {
    connection_credit.CloseStream(context.stream_id);
    return internal::FailureUpdate(
        TransferState::kFailed, TransferError::kInternalFailure, WireErrorCode::kBusy);
  }
}

TransferUpdate OneFileSender::Start(const std::uint64_t now_ms) {
  const std::lock_guard lock(implementation_->mutex);
  if (internal::IsTerminal(implementation_->state)) {
    return implementation_->Update();
  }
  if (!implementation_->Observe(now_ms)) {
    return implementation_->Fail(TransferError::kStateViolation,
                                 WireErrorCode::kStateViolation);
  }
  if (implementation_->state != TransferState::kCreated) {
    return implementation_->Fail(TransferError::kStateViolation,
                                 WireErrorCode::kStateViolation);
  }
  if (!internal::Authorized(implementation_->context)) {
    return implementation_->Fail(TransferError::kUnauthenticated, WireErrorCode::kNone,
                                 true);
  }

  TransferUpdate update;
  if (!implementation_->EncodeOffer(update.outbound_frame)) {
    return implementation_->Fail(TransferError::kInternalFailure, WireErrorCode::kBusy);
  }
  implementation_->state = TransferState::kSendingManifest;
  implementation_->manifest_deadline_ms =
      internal::CheckedDeadline(now_ms, kManifestTimeoutMs);
  implementation_->manifest_progress_deadline_ms =
      internal::CheckedDeadline(now_ms, kManifestProgressTimeoutMs);
  update.state = implementation_->state;
  return update;
}

TransferUpdate OneFileSender::NextOutbound(const std::uint64_t now_ms) {
  const std::lock_guard lock(implementation_->mutex);
  if (internal::IsTerminal(implementation_->state)) {
    return implementation_->Update();
  }
  if (!implementation_->Observe(now_ms)) {
    return implementation_->Fail(TransferError::kStateViolation,
                                 WireErrorCode::kStateViolation);
  }
  if (!internal::Authorized(implementation_->context)) {
    return implementation_->Fail(TransferError::kUnauthenticated, WireErrorCode::kNone,
                                 true);
  }
  TransferUpdate timeout = implementation_->CheckTimeout(now_ms);
  if (timeout.error != TransferError::kNone) {
    return timeout;
  }

  TransferUpdate update = implementation_->Update();
  if (implementation_->state == TransferState::kSendingManifest) {
    bool encoded = false;
    if (implementation_->manifest_stage == 0) {
      encoded = implementation_->EncodeManifestEntry(update.outbound_frame);
      if (encoded) {
        ++implementation_->manifest_stage;
        implementation_->manifest_progress_deadline_ms =
            internal::CheckedDeadline(now_ms, kManifestProgressTimeoutMs);
      }
    } else if (implementation_->manifest_stage == 1) {
      encoded = implementation_->EncodeManifestEnd(update.outbound_frame);
      if (encoded) {
        ++implementation_->manifest_stage;
        implementation_->state = TransferState::kAwaitingDecision;
        implementation_->decision_deadline_ms =
            internal::CheckedDeadline(now_ms, kDecisionTimeoutMs);
      }
    }
    if (!encoded) {
      return implementation_->Fail(TransferError::kInternalFailure,
                                   WireErrorCode::kBusy);
    }
    update.state = implementation_->state;
    return update;
  }

  if (implementation_->state != TransferState::kSendingFile) {
    return update;
  }
  if (!implementation_->file_begin_sent) {
    if (!implementation_->EncodeFileBegin(update.outbound_frame)) {
      return implementation_->Fail(TransferError::kInternalFailure,
                                   WireErrorCode::kBusy);
    }
    implementation_->file_begin_sent = true;
    return update;
  }

  if (implementation_->bytes_sent < implementation_->manifest.file_size) {
    const std::uint64_t remaining =
        implementation_->manifest.file_size - implementation_->bytes_sent;
    const std::uint64_t wanted =
        std::min<std::uint64_t>(implementation_->chunk_size, remaining);
    if (implementation_->available_credit < wanted) {
      return update;
    }
    const FileReadResult read = implementation_->source->Read(
        implementation_->bytes_sent, static_cast<std::size_t>(wanted));
    if (!read.ok() || read.data.size() != static_cast<std::size_t>(wanted)) {
      return implementation_->Fail(TransferError::kSourceFailure,
                                   WireErrorCode::kIoFailure);
    }

    Bytes chunk_commitment;
    if (!implementation_->integrity->BuildChunkCommitment(
            implementation_->manifest.transfer_id, 0, implementation_->bytes_sent,
            read.data, chunk_commitment) ||
        !internal::ValidCommitment(chunk_commitment)) {
      return implementation_->Fail(TransferError::kInternalFailure,
                                   WireErrorCode::kIntegrityFailed);
    }

    if (!internal::EncodeFileChunkFrame(
            implementation_->context, *implementation_->message_ids,
            implementation_->manifest.transfer_id, implementation_->bytes_sent,
            read.data, chunk_commitment, update.outbound_frame)) {
      return implementation_->Fail(TransferError::kInternalFailure,
                                   WireErrorCode::kBusy);
    }

    const bool acknowledgement_timer_was_idle = implementation_->unacknowledged == 0;
    implementation_->bytes_sent += wanted;
    implementation_->available_credit -= static_cast<std::uint32_t>(wanted);
    implementation_->unacknowledged += wanted;
    if (acknowledgement_timer_was_idle) {
      implementation_->acknowledgement_deadline_ms =
          internal::CheckedDeadline(now_ms, kAcknowledgementTimeoutMs);
    }
    implementation_->data_deadline_ms =
        internal::CheckedDeadline(now_ms, kDataProgressTimeoutMs);
    return update;
  }

  if (implementation_->unacknowledged == 0 && !implementation_->file_end_sent) {
    if (!implementation_->EncodeFileEnd(update.outbound_frame)) {
      return implementation_->Fail(TransferError::kInternalFailure,
                                   WireErrorCode::kBusy);
    }
    implementation_->file_end_sent = true;
    implementation_->state = TransferState::kAwaitingFileCommit;
    implementation_->data_deadline_ms =
        internal::CheckedDeadline(now_ms, kDataProgressTimeoutMs);
    update.state = implementation_->state;
  }
  return update;
}

TransferUpdate OneFileSender::ReceiveFrame(const std::span<const std::uint8_t> encoded,
                                           const std::uint64_t now_ms) {
  const std::lock_guard lock(implementation_->mutex);
  if (internal::IsTerminal(implementation_->state)) {
    internal::ParsedTransferFrame parsed = internal::ParseInbound(
        implementation_->context, *implementation_->message_ids, encoded);
    TransferUpdate update = implementation_->Update();
    update.retryable = false;
    update.connection_fatal = false;
    if (!parsed.ok()) {
      update.error = parsed.error;
      update.wire_error = parsed.wire_error;
      update.connection_fatal = parsed.connection_fatal;
    } else if (!internal::ParseBody(parsed)) {
      update.error = parsed.error;
      update.wire_error = parsed.wire_error;
      update.connection_fatal = parsed.connection_fatal;
    } else {
      update.error = TransferError::kStateViolation;
      update.wire_error = WireErrorCode::kStateViolation;
    }
    return update;
  }
  if (!implementation_->Observe(now_ms)) {
    return implementation_->Fail(TransferError::kStateViolation,
                                 WireErrorCode::kStateViolation);
  }
  internal::ParsedTransferFrame parsed = internal::ParseInbound(
      implementation_->context, *implementation_->message_ids, encoded);
  if (!parsed.ok()) {
    return implementation_->Fail(parsed.error, parsed.wire_error,
                                 parsed.connection_fatal);
  }
  TransferUpdate timeout = implementation_->CheckTimeout(now_ms);
  if (timeout.error != TransferError::kNone) {
    return timeout;
  }
  const protocol::v1::MessageType type = parsed.frame.header.message_type;
  if (!AllowedInbound(implementation_->state, type)) {
    return implementation_->Fail(TransferError::kStateViolation,
                                 WireErrorCode::kStateViolation);
  }
  if (!internal::ParseBody(parsed)) {
    return implementation_->Fail(parsed.error, parsed.wire_error,
                                 parsed.connection_fatal);
  }

  if (type == protocol::v1::MessageType::kError) {
    WireErrorCode wire_error = WireErrorCode::kNone;
    const bool fatal = internal::Unsigned(parsed.frame, 2) != 0;
    const bool retryable = internal::Unsigned(parsed.frame, 3) != 0;
    if (!internal::DecodeWireError(internal::Unsigned(parsed.frame, 1), wire_error) ||
        fatal != internal::WireErrorIsFatal(wire_error) ||
        (retryable && !internal::WireErrorMayRetry(wire_error)) ||
        (internal::Field(parsed.frame, 5) != nullptr &&
         internal::Unsigned(parsed.frame, 5) > 60'000)) {
      return implementation_->Fail(TransferError::kMalformedMessage,
                                   WireErrorCode::kMalformedMessage);
    }
    implementation_->ReleaseResources();
    implementation_->state = TransferState::kFailed;
    implementation_->RememberTerminal(
        internal::TransferErrorForWire(wire_error), wire_error,
        internal::WireErrorIsConnectionFatal(wire_error), retryable);
    return implementation_->Update();
  }

  TransferId transfer_id{};
  if (!internal::ReadTransferId(parsed.frame, transfer_id)) {
    return implementation_->Fail(TransferError::kMalformedMessage,
                                 WireErrorCode::kMalformedMessage);
  }
  if (transfer_id != implementation_->manifest.transfer_id) {
    return implementation_->Fail(TransferError::kIdempotencyConflict,
                                 WireErrorCode::kIdempotencyConflict);
  }

  if (implementation_->state == TransferState::kAwaitingDecision) {
    if (type == protocol::v1::MessageType::kTransferReject) {
      WireErrorCode wire_error = WireErrorCode::kNone;
      const bool retryable = internal::Unsigned(parsed.frame, 3) != 0;
      if (!internal::DecodeWireError(internal::Unsigned(parsed.frame, 2), wire_error) ||
          !internal::WireErrorIsRejectReason(wire_error) ||
          (retryable && !internal::WireErrorMayRetry(wire_error)) ||
          (internal::Field(parsed.frame, 4) != nullptr &&
           internal::Unsigned(parsed.frame, 4) > 60'000)) {
        return implementation_->Fail(TransferError::kMalformedMessage,
                                     WireErrorCode::kMalformedMessage);
      }
      implementation_->ReleaseResources();
      implementation_->state = TransferState::kRejected;
      implementation_->RememberTerminal(internal::TransferErrorForWire(wire_error),
                                        wire_error, false, retryable);
      return implementation_->Update();
    }
    if (type != protocol::v1::MessageType::kTransferAccept ||
        internal::Field(parsed.frame, 4) != nullptr ||
        internal::Field(parsed.frame, 5) != nullptr) {
      return implementation_->Fail(TransferError::kStateViolation,
                                   WireErrorCode::kStateViolation);
    }
    const std::uint64_t selected_chunk = internal::Unsigned(parsed.frame, 2);
    const std::uint64_t selected_window = internal::Unsigned(parsed.frame, 3);
    if (selected_chunk < kMinimumChunkSize || selected_chunk > kMaximumChunkSize ||
        implementation_->context.limits.maximum_body <= kMaximumChunkBodyOverhead ||
        selected_chunk >
            implementation_->context.limits.maximum_body - kMaximumChunkBodyOverhead ||
        selected_window < selected_chunk || selected_window > kMaximumTransferWindow ||
        selected_window > implementation_->context.limits.maximum_in_flight) {
      return implementation_->Fail(TransferError::kLimitExceeded,
                                   WireErrorCode::kLimitExceeded);
    }
    if (!implementation_->connection_credit->TryReserve(
            CreditDirection::kOutbound, static_cast<std::uint32_t>(selected_window))) {
      return implementation_->Fail(TransferError::kBusy, WireErrorCode::kBusy);
    }
    implementation_->chunk_size = static_cast<std::uint32_t>(selected_chunk);
    implementation_->reserved_credit = static_cast<std::uint32_t>(selected_window);
    implementation_->available_credit = static_cast<std::uint32_t>(selected_window);
    implementation_->credit_reserved = true;
    implementation_->state = TransferState::kSendingFile;
    implementation_->data_deadline_ms =
        internal::CheckedDeadline(now_ms, kDataProgressTimeoutMs);
    return implementation_->Update();
  }

  if (implementation_->state == TransferState::kSendingFile) {
    if (type != protocol::v1::MessageType::kChunkAck ||
        internal::Unsigned(parsed.frame, 2) != 0) {
      return implementation_->Fail(TransferError::kStateViolation,
                                   WireErrorCode::kStateViolation);
    }
    const std::uint64_t next_offset = internal::Unsigned(parsed.frame, 3);
    const std::uint64_t increment = internal::Unsigned(parsed.frame, 4);
    if (next_offset < implementation_->acknowledged_offset ||
        next_offset > implementation_->bytes_sent ||
        (next_offset == implementation_->acknowledged_offset && increment == 0)) {
      return implementation_->Fail(TransferError::kStateViolation,
                                   WireErrorCode::kStateViolation);
    }
    const std::uint64_t newly_acknowledged =
        next_offset - implementation_->acknowledged_offset;
    if (newly_acknowledged > implementation_->unacknowledged ||
        increment > std::numeric_limits<std::uint32_t>::max()) {
      return implementation_->Fail(TransferError::kLimitExceeded,
                                   WireErrorCode::kLimitExceeded);
    }
    const std::uint64_t new_unacknowledged =
        implementation_->unacknowledged - newly_acknowledged;
    const std::uint64_t new_available =
        static_cast<std::uint64_t>(implementation_->available_credit) + increment;
    const std::uint64_t new_reservation = new_available + new_unacknowledged;
    if (new_reservation > kMaximumTransferWindow ||
        newly_acknowledged > std::numeric_limits<std::uint32_t>::max() ||
        !implementation_->connection_credit->TryAdjust(
            CreditDirection::kOutbound, static_cast<std::uint32_t>(newly_acknowledged),
            static_cast<std::uint32_t>(increment))) {
      return implementation_->Fail(TransferError::kLimitExceeded,
                                   WireErrorCode::kLimitExceeded);
    }
    implementation_->acknowledged_offset = next_offset;
    implementation_->unacknowledged = new_unacknowledged;
    implementation_->available_credit = static_cast<std::uint32_t>(new_available);
    implementation_->reserved_credit = static_cast<std::uint32_t>(new_reservation);
    if (newly_acknowledged != 0) {
      implementation_->acknowledgement_deadline_ms =
          new_unacknowledged == 0
              ? 0
              : internal::CheckedDeadline(now_ms, kAcknowledgementTimeoutMs);
    }
    implementation_->data_deadline_ms =
        internal::CheckedDeadline(now_ms, kDataProgressTimeoutMs);
    return implementation_->Update();
  }

  if (implementation_->state == TransferState::kAwaitingFileCommit) {
    if (type != protocol::v1::MessageType::kFileCommit ||
        internal::Unsigned(parsed.frame, 2) != 0 ||
        internal::Unsigned(parsed.frame, 3) != implementation_->manifest.file_size) {
      return implementation_->Fail(TransferError::kStateViolation,
                                   WireErrorCode::kStateViolation);
    }
    TransferUpdate update;
    if (!implementation_->EncodeComplete(update.outbound_frame)) {
      return implementation_->Fail(TransferError::kInternalFailure,
                                   WireErrorCode::kBusy);
    }
    implementation_->state = TransferState::kCompleting;
    implementation_->data_deadline_ms =
        internal::CheckedDeadline(now_ms, kDataProgressTimeoutMs);
    update.state = implementation_->state;
    return update;
  }

  if (implementation_->state == TransferState::kCompleting) {
    if (type != protocol::v1::MessageType::kTransferCompleteAck ||
        !SameBytes(internal::FieldBytes(parsed.frame, 2),
                   implementation_->manifest.manifest_commitment)) {
      return implementation_->Fail(TransferError::kStateViolation,
                                   WireErrorCode::kStateViolation);
    }
    implementation_->state = TransferState::kCompleted;
    implementation_->RememberTerminal(TransferError::kNone, WireErrorCode::kNone);
    implementation_->ReleaseResources();
    return implementation_->Update();
  }

  return implementation_->Fail(TransferError::kStateViolation,
                               WireErrorCode::kStateViolation);
}

TransferUpdate OneFileSender::Advance(const std::uint64_t now_ms) {
  const std::lock_guard lock(implementation_->mutex);
  if (internal::IsTerminal(implementation_->state)) {
    return implementation_->Update();
  }
  if (!implementation_->Observe(now_ms)) {
    return implementation_->Fail(TransferError::kStateViolation,
                                 WireErrorCode::kStateViolation);
  }
  if (!internal::Authorized(implementation_->context)) {
    return implementation_->Fail(TransferError::kUnauthenticated, WireErrorCode::kNone,
                                 true);
  }
  return implementation_->CheckTimeout(now_ms);
}

TransferUpdate OneFileSender::Shutdown() {
  const std::lock_guard lock(implementation_->mutex);
  if (internal::IsTerminal(implementation_->state)) {
    return implementation_->Update();
  }
  implementation_->ReleaseResources();
  implementation_->state = TransferState::kFailed;
  return implementation_->Update();
}

TransferState OneFileSender::state() const {
  const std::lock_guard lock(implementation_->mutex);
  return implementation_->state;
}

}  // namespace xnn_transfer::core::transfer
