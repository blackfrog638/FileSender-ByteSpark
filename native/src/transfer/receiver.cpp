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

[[nodiscard]] TransferError TransferErrorForTransaction(
    const storage::TransactionResult& result) noexcept {
  switch (result.error) {
    case storage::TransactionError::kNone:
      return TransferError::kNone;
    case storage::TransactionError::kTemporaryBudgetExceeded:
      return TransferError::kBusy;
    case storage::TransactionError::kNoSpace:
      return TransferError::kNoSpace;
    case storage::TransactionError::kIntegrityFailed:
      return TransferError::kIntegrityFailed;
    case storage::TransactionError::kInvalidState:
    case storage::TransactionError::kOpenFailed:
    case storage::TransactionError::kDataTooLarge:
    case storage::TransactionError::kDataTooShort:
    case storage::TransactionError::kWriteFailed:
    case storage::TransactionError::kFlushFailed:
    case storage::TransactionError::kCommitFailed:
    case storage::TransactionError::kCleanupUnresolved:
    case storage::TransactionError::kOutcomeUncertain:
      return TransferError::kIoFailure;
  }
  return TransferError::kIoFailure;
}

[[nodiscard]] WireErrorCode WireErrorForTransaction(
    const storage::TransactionResult& result) noexcept {
  switch (result.error) {
    case storage::TransactionError::kTemporaryBudgetExceeded:
      return WireErrorCode::kBusy;
    case storage::TransactionError::kNoSpace:
      return WireErrorCode::kNoSpace;
    case storage::TransactionError::kIntegrityFailed:
      return WireErrorCode::kIntegrityFailed;
    case storage::TransactionError::kNone:
      return WireErrorCode::kNone;
    case storage::TransactionError::kInvalidState:
    case storage::TransactionError::kOpenFailed:
    case storage::TransactionError::kDataTooLarge:
    case storage::TransactionError::kDataTooShort:
    case storage::TransactionError::kWriteFailed:
    case storage::TransactionError::kFlushFailed:
    case storage::TransactionError::kCommitFailed:
    case storage::TransactionError::kCleanupUnresolved:
    case storage::TransactionError::kOutcomeUncertain:
      return WireErrorCode::kIoFailure;
  }
  return WireErrorCode::kIoFailure;
}

[[nodiscard]] bool LocalRejectCode(const WireErrorCode code) noexcept {
  return code == WireErrorCode::kPolicyRejected || code == WireErrorCode::kNoSpace ||
         code == WireErrorCode::kBusy || code == WireErrorCode::kIoFailure;
}

[[nodiscard]] bool AllowedInbound(const TransferState state,
                                  const protocol::v1::MessageType type,
                                  const bool manifest_entry_received,
                                  const bool file_begin_received) noexcept {
  if (type == protocol::v1::MessageType::kError) {
    return true;
  }
  switch (state) {
    case TransferState::kCreated:
      return type == protocol::v1::MessageType::kTransferOffer;
    case TransferState::kReceivingManifest:
      return manifest_entry_received
                 ? type == protocol::v1::MessageType::kManifestEnd
                 : type == protocol::v1::MessageType::kManifestEntry;
    case TransferState::kReceivingFile:
      return file_begin_received ? type == protocol::v1::MessageType::kFileChunk ||
                                       type == protocol::v1::MessageType::kFileEnd
                                 : type == protocol::v1::MessageType::kFileBegin;
    case TransferState::kAwaitingCompletion:
      return type == protocol::v1::MessageType::kTransferComplete;
    case TransferState::kSendingManifest:
    case TransferState::kAwaitingDecision:
    case TransferState::kSendingFile:
    case TransferState::kRejecting:
    case TransferState::kAwaitingFileCommit:
    case TransferState::kCompleting:
    case TransferState::kCommitted:
    case TransferState::kCompleted:
    case TransferState::kRejected:
    case TransferState::kFailed:
      return false;
  }
  return false;
}

}  // namespace

struct OneFileReceiver::Implementation {
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

  void ReleaseCredit() noexcept {
    if (!credit_reserved) {
      return;
    }
    connection_credit->Release(CreditDirection::kInbound, initial_window);
    credit_reserved = false;
  }

  void ReleaseStream() noexcept {
    if (!stream_reserved) {
      return;
    }
    connection_credit->CloseStream(context.stream_id);
    stream_reserved = false;
  }

  void AbortTransaction() noexcept {
    if (transaction == nullptr) {
      return;
    }
    static_cast<void>(transaction->Abort());
  }

  [[nodiscard]] TransferUpdate Fail(const TransferError error,
                                    const WireErrorCode wire_error,
                                    const bool connection_fatal = false) {
    const bool durable_commit =
        transaction != nullptr &&
        transaction->state() == storage::TransactionState::kCommitted;
    if (!durable_commit) {
      AbortTransaction();
    }
    ReleaseCredit();
    ReleaseStream();
    state = durable_commit ? TransferState::kCommitted : TransferState::kFailed;
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

  [[nodiscard]] bool Observe(const std::uint64_t now_ms) noexcept {
    return internal::ObserveTime(now_ms, last_now_ms);
  }

  [[nodiscard]] TransferUpdate CheckTimeout(const std::uint64_t now_ms) {
    if (state == TransferState::kReceivingManifest &&
        (internal::DeadlineReached(now_ms, manifest_deadline_ms) ||
         internal::DeadlineReached(now_ms, manifest_progress_deadline_ms))) {
      return Fail(TransferError::kTimeout, WireErrorCode::kTimeout);
    }
    if (state == TransferState::kAwaitingDecision &&
        internal::DeadlineReached(now_ms, decision_deadline_ms)) {
      return Fail(TransferError::kTimeout, WireErrorCode::kTimeout);
    }
    if ((state == TransferState::kRejecting || state == TransferState::kReceivingFile ||
         state == TransferState::kAwaitingCompletion) &&
        internal::DeadlineReached(now_ms, data_deadline_ms)) {
      return Fail(TransferError::kTimeout, WireErrorCode::kTimeout);
    }
    return Update();
  }

  [[nodiscard]] bool EncodeAccept(Bytes& output) {
    internal::BodyBuilder body;
    static_cast<void>(body.AddBytes(1, manifest.transfer_id));
    static_cast<void>(body.AddU32(2, chunk_size));
    static_cast<void>(body.AddU32(3, initial_window));
    return internal::EncodeFrame(context, protocol::v1::MessageType::kTransferAccept,
                                 *message_ids, body, output);
  }

  [[nodiscard]] bool EncodeReject(const WireErrorCode code, const bool retryable,
                                  Bytes& output) {
    internal::BodyBuilder body;
    static_cast<void>(body.AddBytes(1, manifest.transfer_id));
    static_cast<void>(body.AddU16(2, static_cast<std::uint16_t>(code)));
    static_cast<void>(body.AddBool(3, retryable));
    return internal::EncodeFrame(context, protocol::v1::MessageType::kTransferReject,
                                 *message_ids, body, output);
  }

  [[nodiscard]] bool EncodeChunkAck(Bytes& output) {
    internal::BodyBuilder body;
    static_cast<void>(body.AddBytes(1, manifest.transfer_id));
    static_cast<void>(body.AddU32(2, 0));
    static_cast<void>(body.AddU64(3, next_offset));
    static_cast<void>(body.AddU32(4, last_chunk_size));
    return internal::EncodeFrame(context, protocol::v1::MessageType::kChunkAck,
                                 *message_ids, body, output);
  }

  [[nodiscard]] bool EncodeFileCommit(Bytes& output) {
    internal::BodyBuilder body;
    static_cast<void>(body.AddBytes(1, manifest.transfer_id));
    static_cast<void>(body.AddU32(2, 0));
    static_cast<void>(body.AddU64(3, manifest.file_size));
    return internal::EncodeFrame(context, protocol::v1::MessageType::kFileCommit,
                                 *message_ids, body, output);
  }

  [[nodiscard]] bool EncodeCompleteAck(Bytes& output) {
    internal::BodyBuilder body;
    static_cast<void>(body.AddBytes(1, manifest.transfer_id));
    static_cast<void>(body.AddBytes(2, manifest.manifest_commitment));
    return internal::EncodeFrame(context,
                                 protocol::v1::MessageType::kTransferCompleteAck,
                                 *message_ids, body, output);
  }

  [[nodiscard]] TransferUpdate PendingEncodingFailure() {
    if (message_ids->OutboundExhausted()) {
      return Fail(TransferError::kMessageIdViolation,
                  WireErrorCode::kMessageIdViolation, true);
    }
    TransferUpdate update = Update();
    update.error = TransferError::kInternalFailure;
    update.wire_error = WireErrorCode::kBusy;
    update.retryable = true;
    return update;
  }

  [[nodiscard]] TransferUpdate EmitPendingReject() {
    TransferUpdate update = Update();
    if (!EncodeReject(pending_reject_code, pending_reject_retryable,
                      update.outbound_frame)) {
      return PendingEncodingFailure();
    }
    state = TransferState::kRejected;
    RememberTerminal(pending_reject_error, pending_reject_code, false,
                     pending_reject_retryable);
    ReleaseStream();
    pending_reject = false;
    update.state = state;
    update.error = pending_reject_error;
    update.wire_error = pending_reject_code;
    update.terminal = true;
    update.retryable = pending_reject_retryable;
    return update;
  }

  [[nodiscard]] TransferUpdate BeginReject(const TransferError error,
                                           const WireErrorCode wire_error,
                                           const bool retryable,
                                           const std::uint64_t now_ms) {
    AbortTransaction();
    ReleaseCredit();
    state = TransferState::kRejecting;
    pending_reject = true;
    pending_reject_error = error;
    pending_reject_code = wire_error;
    pending_reject_retryable = retryable;
    data_deadline_ms = internal::CheckedDeadline(now_ms, kDataProgressTimeoutMs);
    return EmitPendingReject();
  }

  [[nodiscard]] TransferUpdate EmitPendingFileCommit() {
    TransferUpdate update = Update();
    if (!EncodeFileCommit(update.outbound_frame)) {
      return PendingEncodingFailure();
    }
    file_commit_sent = true;
    return update;
  }

  [[nodiscard]] TransferUpdate EmitPendingCompletionAck() {
    TransferUpdate update = Update();
    if (!EncodeCompleteAck(update.outbound_frame)) {
      return PendingEncodingFailure();
    }
    completion_ack_pending = false;
    ReleaseCredit();
    ReleaseStream();
    state = TransferState::kCompleted;
    RememberTerminal(TransferError::kNone, WireErrorCode::kNone);
    update.state = state;
    update.terminal = true;
    return update;
  }

  [[nodiscard]] TransferUpdate RejectForStorage(
      const storage::TransactionResult& result, const std::uint64_t now_ms) {
    const TransferError error = TransferErrorForTransaction(result);
    const WireErrorCode wire_error = WireErrorForTransaction(result);
    const bool retryable = wire_error == WireErrorCode::kBusy ||
                           wire_error == WireErrorCode::kNoSpace ||
                           wire_error == WireErrorCode::kIoFailure;
    if (!internal::WireErrorIsRejectReason(wire_error)) {
      return Fail(error, wire_error);
    }
    return BeginReject(error, wire_error, retryable, now_ms);
  }

  mutable std::mutex mutex{};
  TransferContext context{};
  ConnectionMessageSequence* message_ids{};
  TransferIntegrityProvider* integrity{};
  std::shared_ptr<storage::TemporaryBudget> temporary_budget{};
  ConnectionCreditBudget* connection_credit{};
  storage::PlatformBackend* platform{};
  TransferState state{TransferState::kCreated};
  OneFileManifest manifest{};
  std::optional<storage::ValidatedReceiveRequest> request{};
  std::unique_ptr<storage::ReceiveTransaction> transaction{};
  Bytes raw_offer_frame{};
  Bytes raw_entry_frame{};
  Bytes raw_end_frame{};
  std::uint32_t chunk_size{};
  std::uint32_t initial_window{};
  std::uint32_t available_credit{};
  std::uint32_t last_chunk_size{};
  std::uint64_t next_offset{};
  std::uint64_t last_ack_encoded_offset{};
  std::uint64_t last_ack_written_offset{};
  std::uint64_t last_now_ms{};
  std::uint64_t manifest_deadline_ms{};
  std::uint64_t manifest_progress_deadline_ms{};
  std::uint64_t decision_deadline_ms{};
  std::uint64_t data_deadline_ms{};
  bool manifest_entry_received{};
  bool file_begin_received{};
  bool file_commit_sent{};
  bool completion_ack_pending{};
  bool pending_reject{};
  bool stream_reserved{};
  bool credit_reserved{};
  TransferError pending_reject_error{TransferError::kNone};
  WireErrorCode pending_reject_code{WireErrorCode::kNone};
  bool pending_reject_retryable{};
  TransferError terminal_error{TransferError::kNone};
  WireErrorCode terminal_wire_error{WireErrorCode::kNone};
  bool terminal_connection_fatal{};
  bool terminal_retryable{};
};

OneFileReceiver::OneFileReceiver(std::unique_ptr<Implementation> implementation)
    : implementation_(std::move(implementation)) {}

OneFileReceiver::~OneFileReceiver() {
  if (implementation_ != nullptr) {
    static_cast<void>(Shutdown());
  }
}

TransferUpdate OneFileReceiver::Create(
    TransferContext context, ConnectionMessageSequence& message_ids,
    TransferIntegrityProvider& integrity,
    std::shared_ptr<storage::TemporaryBudget> temporary_budget,
    ConnectionCreditBudget& connection_credit, storage::PlatformBackend& platform,
    std::unique_ptr<OneFileReceiver>& output) {
  output.reset();
  if (!internal::ValidateContext(context, false) || !internal::Authorized(context) ||
      temporary_budget == nullptr || connection_credit.limit_bytes() == 0 ||
      connection_credit.limit_bytes() > context.limits.maximum_in_flight ||
      connection_credit.maximum_streams() == 0 ||
      connection_credit.maximum_streams() > context.limits.maximum_active_streams) {
    return internal::FailureUpdate(TransferState::kFailed,
                                   TransferError::kInvalidArgument,
                                   WireErrorCode::kInvalidOffer);
  }
  try {
    auto implementation = std::make_unique<Implementation>();
    implementation->context = context;
    implementation->message_ids = &message_ids;
    implementation->integrity = &integrity;
    implementation->temporary_budget = std::move(temporary_budget);
    implementation->connection_credit = &connection_credit;
    implementation->platform = &platform;
    output = std::unique_ptr<OneFileReceiver>(
        new OneFileReceiver(std::move(implementation)));
    return output->implementation_->Update();
  } catch (const std::bad_alloc&) {
    return internal::FailureUpdate(
        TransferState::kFailed, TransferError::kInternalFailure, WireErrorCode::kBusy);
  }
}

TransferUpdate OneFileReceiver::ReceiveFrame(
    const std::span<const std::uint8_t> encoded, const std::uint64_t now_ms) {
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
  if (!implementation_->stream_reserved) {
    const StreamOpenResult opened = implementation_->connection_credit->OpenStream(
        implementation_->context.stream_id, false, implementation_->context.local_role);
    if (opened != StreamOpenResult::kOpened) {
      return implementation_->Fail(opened == StreamOpenResult::kCapacityRejected
                                       ? TransferError::kBusy
                                       : TransferError::kStateViolation,
                                   opened == StreamOpenResult::kCapacityRejected
                                       ? WireErrorCode::kBusy
                                       : WireErrorCode::kStateViolation);
    }
    implementation_->stream_reserved = true;
  }
  TransferUpdate timeout = implementation_->CheckTimeout(now_ms);
  if (timeout.error != TransferError::kNone) {
    return timeout;
  }
  const protocol::v1::MessageType type = parsed.frame.header.message_type;
  if (!AllowedInbound(implementation_->state, type,
                      implementation_->manifest_entry_received,
                      implementation_->file_begin_received)) {
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
    const bool durable_commit =
        implementation_->transaction != nullptr &&
        implementation_->transaction->state() == storage::TransactionState::kCommitted;
    if (!durable_commit) {
      implementation_->AbortTransaction();
    }
    implementation_->ReleaseCredit();
    implementation_->ReleaseStream();
    implementation_->state =
        durable_commit ? TransferState::kCommitted : TransferState::kFailed;
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

  try {
    if (implementation_->state == TransferState::kCreated) {
      if (type != protocol::v1::MessageType::kTransferOffer) {
        return implementation_->Fail(TransferError::kStateViolation,
                                     WireErrorCode::kStateViolation);
      }
      if (internal::AllZero(transfer_id) || internal::Unsigned(parsed.frame, 2) != 1 ||
          internal::Unsigned(parsed.frame, 3) > storage::kMaxFileBytes ||
          !internal::ValidCommitment(internal::FieldBytes(parsed.frame, 4))) {
        return implementation_->Fail(TransferError::kInvalidOffer,
                                     WireErrorCode::kInvalidOffer);
      }
      implementation_->manifest.transfer_id = transfer_id;
      implementation_->manifest.file_size = internal::Unsigned(parsed.frame, 3);
      const std::span<const std::uint8_t> manifest_commitment =
          internal::FieldBytes(parsed.frame, 4);
      implementation_->manifest.manifest_commitment.assign(manifest_commitment.begin(),
                                                           manifest_commitment.end());
      const std::span<const std::uint8_t> display =
          internal::FieldBytes(parsed.frame, 5);
      implementation_->manifest.display_name.assign(
          reinterpret_cast<const char*>(display.data()), display.size());
      implementation_->raw_offer_frame.assign(parsed.frame.raw.begin(),
                                              parsed.frame.raw.end());
      implementation_->state = TransferState::kReceivingManifest;
      implementation_->manifest_deadline_ms =
          internal::CheckedDeadline(now_ms, kManifestTimeoutMs);
      implementation_->manifest_progress_deadline_ms =
          internal::CheckedDeadline(now_ms, kManifestProgressTimeoutMs);
      return implementation_->Update();
    }

    if (transfer_id != implementation_->manifest.transfer_id) {
      return implementation_->Fail(TransferError::kIdempotencyConflict,
                                   WireErrorCode::kIdempotencyConflict);
    }

    if (implementation_->state == TransferState::kReceivingManifest) {
      if (!implementation_->manifest_entry_received) {
        if (type != protocol::v1::MessageType::kManifestEntry ||
            internal::Unsigned(parsed.frame, 2) != 0 ||
            internal::Unsigned(parsed.frame, 3) != 1 ||
            internal::Unsigned(parsed.frame, 5) !=
                implementation_->manifest.file_size ||
            !internal::ValidCommitment(internal::FieldBytes(parsed.frame, 6))) {
          return implementation_->Fail(TransferError::kInvalidManifest,
                                       WireErrorCode::kInvalidManifest);
        }
        const std::span<const std::uint8_t> path =
            internal::FieldBytes(parsed.frame, 4);
        const storage::RequestValidationResult request =
            storage::ValidateReceiveRequest(path, implementation_->manifest.file_size);
        if (!request.ok() || request.request() == nullptr) {
          return implementation_->Fail(TransferError::kInvalidManifest,
                                       WireErrorCode::kInvalidManifest);
        }
        implementation_->request.emplace(*request.request());
        implementation_->manifest.relative_path.assign(
            reinterpret_cast<const char*>(path.data()), path.size());
        const std::span<const std::uint8_t> file_commitment =
            internal::FieldBytes(parsed.frame, 6);
        implementation_->manifest.file_commitment.assign(file_commitment.begin(),
                                                         file_commitment.end());
        implementation_->raw_entry_frame.assign(parsed.frame.raw.begin(),
                                                parsed.frame.raw.end());
        implementation_->manifest_entry_received = true;
        implementation_->manifest_progress_deadline_ms =
            internal::CheckedDeadline(now_ms, kManifestProgressTimeoutMs);
        return implementation_->Update();
      }

      if (type != protocol::v1::MessageType::kManifestEnd ||
          internal::Unsigned(parsed.frame, 2) != 1 ||
          internal::Unsigned(parsed.frame, 3) != implementation_->manifest.file_size ||
          !SameBytes(internal::FieldBytes(parsed.frame, 4),
                     implementation_->manifest.manifest_commitment)) {
        return implementation_->Fail(TransferError::kInvalidManifest,
                                     WireErrorCode::kInvalidManifest);
      }
      implementation_->raw_end_frame.assign(parsed.frame.raw.begin(),
                                            parsed.frame.raw.end());
      const ManifestVerificationInput verification{
          .manifest = &implementation_->manifest,
          .offer_frame = implementation_->raw_offer_frame,
          .entry_frame = implementation_->raw_entry_frame,
          .end_frame = implementation_->raw_end_frame,
      };
      if (!implementation_->integrity->VerifyManifest(verification)) {
        return implementation_->Fail(TransferError::kIntegrityFailed,
                                     WireErrorCode::kIntegrityFailed);
      }
      implementation_->state = TransferState::kAwaitingDecision;
      implementation_->decision_deadline_ms =
          internal::CheckedDeadline(now_ms, kDecisionTimeoutMs);
      TransferUpdate update = implementation_->Update();
      update.offer = IncomingOffer{
          .transfer_id = implementation_->manifest.transfer_id,
          .relative_path = implementation_->manifest.relative_path,
          .file_size = implementation_->manifest.file_size,
          .display_name = implementation_->manifest.display_name,
      };
      return update;
    }

    if (implementation_->state == TransferState::kReceivingFile) {
      if (!implementation_->file_begin_received) {
        if (type != protocol::v1::MessageType::kFileBegin ||
            internal::Unsigned(parsed.frame, 2) != 0 ||
            internal::Unsigned(parsed.frame, 3) !=
                implementation_->manifest.file_size ||
            internal::Unsigned(parsed.frame, 4) != implementation_->chunk_size) {
          return implementation_->Fail(TransferError::kStateViolation,
                                       WireErrorCode::kStateViolation);
        }
        implementation_->file_begin_received = true;
        return implementation_->Update();
      }

      if (type == protocol::v1::MessageType::kFileChunk) {
        const std::uint64_t entry_index = internal::Unsigned(parsed.frame, 2);
        const std::uint64_t offset = internal::Unsigned(parsed.frame, 3);
        const std::span<const std::uint8_t> data =
            internal::FieldBytes(parsed.frame, 4);
        const std::span<const std::uint8_t> commitment =
            internal::FieldBytes(parsed.frame, 5);
        if (entry_index != 0 || offset != implementation_->next_offset ||
            data.empty() || data.size() > implementation_->chunk_size ||
            data.size() >
                implementation_->manifest.file_size - implementation_->next_offset ||
            (implementation_->next_offset + data.size() <
                 implementation_->manifest.file_size &&
             data.size() != implementation_->chunk_size) ||
            !internal::ValidCommitment(commitment)) {
          return implementation_->Fail(TransferError::kStateViolation,
                                       WireErrorCode::kStateViolation);
        }
        if (data.size() > implementation_->available_credit) {
          return implementation_->Fail(TransferError::kLimitExceeded,
                                       WireErrorCode::kLimitExceeded);
        }
        if (!implementation_->integrity->VerifyChunkCommitment(
                implementation_->manifest.transfer_id, 0, implementation_->next_offset,
                data, commitment)) {
          return implementation_->Fail(TransferError::kIntegrityFailed,
                                       WireErrorCode::kIntegrityFailed);
        }
        const storage::TransactionResult written =
            implementation_->transaction->Write(data);
        if (!written.ok()) {
          return implementation_->Fail(TransferErrorForTransaction(written),
                                       WireErrorForTransaction(written));
        }
        implementation_->available_credit -= static_cast<std::uint32_t>(data.size());
        implementation_->next_offset += data.size();
        implementation_->last_chunk_size = static_cast<std::uint32_t>(data.size());
        TransferUpdate update = implementation_->Update();
        if (!implementation_->EncodeChunkAck(update.outbound_frame)) {
          return implementation_->Fail(TransferError::kInternalFailure,
                                       WireErrorCode::kBusy);
        }
        implementation_->last_ack_encoded_offset = implementation_->next_offset;
        implementation_->data_deadline_ms =
            internal::CheckedDeadline(now_ms, kDataProgressTimeoutMs);
        return update;
      }

      if (type == protocol::v1::MessageType::kFileEnd) {
        if (internal::Unsigned(parsed.frame, 2) != 0 ||
            internal::Unsigned(parsed.frame, 3) !=
                implementation_->manifest.file_size ||
            implementation_->next_offset != implementation_->manifest.file_size ||
            !SameBytes(internal::FieldBytes(parsed.frame, 4),
                       implementation_->manifest.file_commitment)) {
          return implementation_->Fail(TransferError::kStateViolation,
                                       WireErrorCode::kStateViolation);
        }
        const storage::TransactionResult committed =
            implementation_->transaction->SealAndCommit();
        if (!committed.ok()) {
          return implementation_->Fail(TransferErrorForTransaction(committed),
                                       WireErrorForTransaction(committed));
        }
        implementation_->state = TransferState::kAwaitingCompletion;
        implementation_->data_deadline_ms =
            internal::CheckedDeadline(now_ms, kDataProgressTimeoutMs);
        return implementation_->EmitPendingFileCommit();
      }

      return implementation_->Fail(TransferError::kStateViolation,
                                   WireErrorCode::kStateViolation);
    }

    if (implementation_->state == TransferState::kAwaitingCompletion) {
      if (!implementation_->file_commit_sent ||
          type != protocol::v1::MessageType::kTransferComplete ||
          !SameBytes(internal::FieldBytes(parsed.frame, 2),
                     implementation_->manifest.manifest_commitment)) {
        return implementation_->Fail(TransferError::kStateViolation,
                                     WireErrorCode::kStateViolation);
      }
      implementation_->completion_ack_pending = true;
      implementation_->data_deadline_ms =
          internal::CheckedDeadline(now_ms, kDataProgressTimeoutMs);
      return implementation_->EmitPendingCompletionAck();
    }
  } catch (const std::bad_alloc&) {
    return implementation_->Fail(TransferError::kInternalFailure, WireErrorCode::kBusy);
  }

  return implementation_->Fail(TransferError::kStateViolation,
                               WireErrorCode::kStateViolation);
}

TransferUpdate OneFileReceiver::NextOutbound(const std::uint64_t now_ms) {
  const std::lock_guard lock(implementation_->mutex);
  if (internal::IsTerminal(implementation_->state)) {
    return implementation_->Update();
  }
  if (implementation_->state != TransferState::kRejecting &&
      implementation_->state != TransferState::kAwaitingCompletion) {
    return implementation_->Update();
  }
  if (!implementation_->Observe(now_ms)) {
    return implementation_->Fail(TransferError::kStateViolation,
                                 WireErrorCode::kStateViolation);
  }
  TransferUpdate timeout = implementation_->CheckTimeout(now_ms);
  if (timeout.error != TransferError::kNone) {
    return timeout;
  }
  if (!internal::Authorized(implementation_->context)) {
    return implementation_->Fail(TransferError::kUnauthenticated, WireErrorCode::kNone,
                                 true);
  }
  if (implementation_->state == TransferState::kRejecting) {
    return implementation_->EmitPendingReject();
  }
  if (implementation_->completion_ack_pending) {
    return implementation_->EmitPendingCompletionAck();
  }
  if (!implementation_->file_commit_sent) {
    TransferUpdate update = implementation_->EmitPendingFileCommit();
    if (!update.outbound_frame.empty()) {
      implementation_->data_deadline_ms =
          internal::CheckedDeadline(now_ms, kDataProgressTimeoutMs);
    }
    return update;
  }
  return implementation_->Update();
}

TransferUpdate OneFileReceiver::ConfirmChunkAckWritten(const std::uint64_t next_offset,
                                                       const std::uint64_t now_ms) {
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
  if ((implementation_->state != TransferState::kReceivingFile &&
       implementation_->state != TransferState::kAwaitingCompletion) ||
      next_offset <= implementation_->last_ack_written_offset ||
      next_offset > implementation_->last_ack_encoded_offset) {
    return implementation_->Fail(TransferError::kStateViolation,
                                 WireErrorCode::kStateViolation);
  }
  const std::uint64_t restored = next_offset - implementation_->last_ack_written_offset;
  if (restored > implementation_->initial_window - implementation_->available_credit) {
    return implementation_->Fail(TransferError::kLimitExceeded,
                                 WireErrorCode::kLimitExceeded);
  }
  implementation_->available_credit += static_cast<std::uint32_t>(restored);
  implementation_->last_ack_written_offset = next_offset;
  implementation_->data_deadline_ms =
      internal::CheckedDeadline(now_ms, kDataProgressTimeoutMs);
  return implementation_->Update();
}

TransferUpdate OneFileReceiver::Accept(const std::uint32_t chunk_size,
                                       const std::uint32_t initial_window,
                                       const std::uint64_t now_ms) {
  const std::lock_guard lock(implementation_->mutex);
  if (internal::IsTerminal(implementation_->state)) {
    return implementation_->Update();
  }
  if (implementation_->state == TransferState::kReceivingFile &&
      implementation_->request != std::nullopt) {
    if (implementation_->chunk_size == chunk_size &&
        implementation_->initial_window == initial_window) {
      return implementation_->Update();
    }
    TransferUpdate update = implementation_->Update();
    update.error = TransferError::kStateViolation;
    update.wire_error = WireErrorCode::kStateViolation;
    return update;
  }
  if (implementation_->state != TransferState::kAwaitingDecision ||
      implementation_->request == std::nullopt) {
    TransferUpdate update = implementation_->Update();
    update.error = TransferError::kStateViolation;
    update.wire_error = WireErrorCode::kStateViolation;
    return update;
  }
  if (!implementation_->Observe(now_ms)) {
    return implementation_->Fail(TransferError::kStateViolation,
                                 WireErrorCode::kStateViolation);
  }
  TransferUpdate timeout = implementation_->CheckTimeout(now_ms);
  if (timeout.error != TransferError::kNone) {
    return timeout;
  }
  if (!internal::Authorized(implementation_->context)) {
    return implementation_->Fail(TransferError::kUnauthenticated, WireErrorCode::kNone,
                                 true);
  }
  if (chunk_size < kMinimumChunkSize || chunk_size > kMaximumChunkSize ||
      implementation_->context.limits.maximum_body <= kMaximumChunkBodyOverhead ||
      chunk_size >
          implementation_->context.limits.maximum_body - kMaximumChunkBodyOverhead ||
      initial_window < chunk_size || initial_window > kMaximumTransferWindow ||
      initial_window > implementation_->context.limits.maximum_in_flight) {
    return implementation_->Fail(TransferError::kLimitExceeded,
                                 WireErrorCode::kLimitExceeded);
  }
  if (!implementation_->connection_credit->TryReserve(CreditDirection::kInbound,
                                                      initial_window)) {
    return implementation_->RejectForStorage(
        {.error = storage::TransactionError::kTemporaryBudgetExceeded}, now_ms);
  }
  implementation_->initial_window = initial_window;
  implementation_->credit_reserved = true;

  std::unique_ptr<storage::StreamingIntegrityVerifier> verifier =
      implementation_->integrity->CreateFileVerifier(
          implementation_->manifest.transfer_id, 0,
          implementation_->manifest.file_commitment);
  if (verifier == nullptr) {
    return implementation_->RejectForStorage(
        {.error = storage::TransactionError::kIntegrityFailed}, now_ms);
  }

  try {
    implementation_->transaction = std::make_unique<storage::ReceiveTransaction>(
        *implementation_->request, implementation_->temporary_budget,
        std::move(verifier), *implementation_->platform);
  } catch (const std::bad_alloc&) {
    return implementation_->RejectForStorage(
        {.error = storage::TransactionError::kOpenFailed}, now_ms);
  }
  const storage::TransactionResult begun = implementation_->transaction->Begin();
  if (!begun.ok()) {
    return implementation_->RejectForStorage(begun, now_ms);
  }

  implementation_->chunk_size = chunk_size;
  implementation_->available_credit = initial_window;
  TransferUpdate update;
  if (!implementation_->EncodeAccept(update.outbound_frame)) {
    return implementation_->Fail(TransferError::kInternalFailure, WireErrorCode::kBusy);
  }
  implementation_->state = TransferState::kReceivingFile;
  implementation_->data_deadline_ms =
      internal::CheckedDeadline(now_ms, kDataProgressTimeoutMs);
  update.state = implementation_->state;
  return update;
}

TransferUpdate OneFileReceiver::Reject(const WireErrorCode code, const bool retryable,
                                       const std::uint64_t now_ms) {
  const std::lock_guard lock(implementation_->mutex);
  if (internal::IsTerminal(implementation_->state)) {
    return implementation_->Update();
  }
  if (implementation_->state == TransferState::kRejecting) {
    if (implementation_->pending_reject &&
        implementation_->pending_reject_code == code &&
        implementation_->pending_reject_retryable == retryable) {
      return implementation_->Update();
    }
    TransferUpdate update = implementation_->Update();
    update.error = TransferError::kStateViolation;
    update.wire_error = WireErrorCode::kStateViolation;
    return update;
  }
  if (implementation_->state != TransferState::kAwaitingDecision ||
      !LocalRejectCode(code) || (retryable && !internal::WireErrorMayRetry(code))) {
    TransferUpdate update = implementation_->Update();
    update.error = TransferError::kStateViolation;
    update.wire_error = WireErrorCode::kStateViolation;
    return update;
  }
  if (!implementation_->Observe(now_ms)) {
    return implementation_->Fail(TransferError::kStateViolation,
                                 WireErrorCode::kStateViolation);
  }
  TransferUpdate timeout = implementation_->CheckTimeout(now_ms);
  if (timeout.error != TransferError::kNone) {
    return timeout;
  }
  if (!internal::Authorized(implementation_->context)) {
    return implementation_->Fail(TransferError::kUnauthenticated, WireErrorCode::kNone,
                                 true);
  }
  return implementation_->BeginReject(internal::TransferErrorForWire(code), code,
                                      retryable, now_ms);
}

TransferUpdate OneFileReceiver::Advance(const std::uint64_t now_ms) {
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

TransferUpdate OneFileReceiver::Shutdown() {
  const std::lock_guard lock(implementation_->mutex);
  if (internal::IsTerminal(implementation_->state)) {
    return implementation_->Update();
  }
  const bool durable_commit =
      implementation_->transaction != nullptr &&
      implementation_->transaction->state() == storage::TransactionState::kCommitted;
  if (!durable_commit) {
    implementation_->AbortTransaction();
  }
  implementation_->ReleaseCredit();
  implementation_->ReleaseStream();
  implementation_->state =
      durable_commit ? TransferState::kCommitted : TransferState::kFailed;
  return implementation_->Update();
}

TransferState OneFileReceiver::state() const {
  const std::lock_guard lock(implementation_->mutex);
  return implementation_->state;
}

}  // namespace xnn_transfer::core::transfer
