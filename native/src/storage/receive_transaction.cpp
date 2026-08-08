#include <utility>

#include "xnn_transfer/core/storage/storage.hpp"

namespace xnn_transfer::core::storage {
namespace {

[[nodiscard]] TransactionError ErrorForPlatform(
    const PlatformError error, const TransactionError fallback) noexcept {
  return error == PlatformError::kNoSpace ? TransactionError::kNoSpace : fallback;
}

}  // namespace

TemporaryBudget::TemporaryBudget(const std::uint64_t limit_bytes) noexcept
    : limit_bytes_(limit_bytes) {}

std::uint64_t TemporaryBudget::limit_bytes() const noexcept {
  const std::scoped_lock lock(mutex_);
  return limit_bytes_;
}

std::uint64_t TemporaryBudget::reserved_bytes() const noexcept {
  const std::scoped_lock lock(mutex_);
  return reserved_bytes_;
}

std::uint64_t TemporaryBudget::unresolved_bytes() const noexcept {
  const std::scoped_lock lock(mutex_);
  return unresolved_bytes_;
}

std::size_t TemporaryBudget::unresolved_files() const noexcept {
  const std::scoped_lock lock(mutex_);
  return unresolved_files_;
}

bool TemporaryBudget::TryReserve(const std::uint64_t bytes) noexcept {
  const std::scoped_lock lock(mutex_);
  if (reserved_bytes_ > limit_bytes_ || bytes > limit_bytes_ - reserved_bytes_) {
    return false;
  }
  reserved_bytes_ += bytes;
  return true;
}

void TemporaryBudget::Release(const std::uint64_t bytes) noexcept {
  const std::scoped_lock lock(mutex_);
  reserved_bytes_ -= bytes;
}

void TemporaryBudget::MarkUnresolved(const std::uint64_t bytes) noexcept {
  const std::scoped_lock lock(mutex_);
  unresolved_bytes_ += bytes;
  ++unresolved_files_;
}

ReceiveTransaction::ReceiveTransaction(
    ValidatedReceiveRequest request, std::shared_ptr<TemporaryBudget> temporary_budget,
    std::unique_ptr<StreamingIntegrityVerifier> verifier, PlatformBackend& platform)
    : request_(std::move(request)),
      temporary_budget_(std::move(temporary_budget)),
      verifier_(std::move(verifier)),
      platform_(platform) {}

ReceiveTransaction::~ReceiveTransaction() {
  const TransactionResult ignored = Abort();
  (void)ignored;
}

TransactionResult ReceiveTransaction::Begin() {
  const std::scoped_lock lock(mutex_);
  if (state_ != TransactionState::kCreated) {
    return {.error = TransactionError::kInvalidState};
  }
  if (temporary_budget_ == nullptr || verifier_ == nullptr) {
    state_ = TransactionState::kAborted;
    terminal_result_ = {.error = TransactionError::kInvalidState};
    return terminal_result_;
  }
  if (!temporary_budget_->TryReserve(request_.declared_size())) {
    state_ = TransactionState::kAborted;
    terminal_result_ = {.error = TransactionError::kTemporaryBudgetExceeded};
    return terminal_result_;
  }
  reservation_held_ = true;

  TemporaryFileHandle handle;
  const PlatformResult created =
      platform_.CreateTemporary(request_.path(), request_.declared_size(), handle);
  if (!created.ok() || !handle.valid()) {
    const PlatformError platform_error =
        created.ok() ? PlatformError::kIoFailure : created.error;
    if (handle.valid()) {
      temporary_ = handle;
      temporary_open_ = true;
      return FailAndCleanup(
          ErrorForPlatform(platform_error, TransactionError::kOpenFailed),
          platform_error);
    }

    ReleaseReservation();
    state_ = TransactionState::kAborted;
    terminal_result_ = {
        .error = ErrorForPlatform(platform_error, TransactionError::kOpenFailed),
        .platform_error = platform_error};
    return terminal_result_;
  }

  temporary_ = handle;
  temporary_open_ = true;
  state_ = TransactionState::kReceiving;
  return {};
}

TransactionResult ReceiveTransaction::Write(const std::span<const std::uint8_t> data) {
  const std::scoped_lock lock(mutex_);
  if (state_ != TransactionState::kReceiving) {
    return {.error = TransactionError::kInvalidState};
  }

  const std::uint64_t data_size = static_cast<std::uint64_t>(data.size());
  const std::uint64_t remaining = request_.declared_size() - received_bytes_;
  if (data_size > remaining) {
    return FailAndCleanup(TransactionError::kDataTooLarge);
  }
  if (data.empty()) {
    return {};
  }

  const PlatformWriteResult written = platform_.WriteTemporary(temporary_, data);
  if (!written.ok() || written.bytes_written != data.size()) {
    const TransactionError error =
        ErrorForPlatform(written.error, TransactionError::kWriteFailed);
    return FailAndCleanup(error, written.error);
  }
  if (!verifier_->Update(data)) {
    return FailAndCleanup(TransactionError::kIntegrityFailed);
  }

  received_bytes_ += data_size;
  return {};
}

TransactionResult ReceiveTransaction::SealAndCommit() {
  const std::scoped_lock lock(mutex_);
  if (state_ != TransactionState::kReceiving) {
    return {.error = TransactionError::kInvalidState};
  }
  if (received_bytes_ != request_.declared_size()) {
    return FailAndCleanup(TransactionError::kDataTooShort);
  }
  if (!verifier_->Seal()) {
    return FailAndCleanup(TransactionError::kIntegrityFailed);
  }

  const PlatformResult flushed = platform_.FlushTemporary(temporary_);
  if (!flushed.ok()) {
    return FailAndCleanup(
        ErrorForPlatform(flushed.error, TransactionError::kFlushFailed), flushed.error);
  }

  const PlatformCommitResult committed =
      platform_.CommitTemporary(temporary_, request_.path());
  if (committed.disposition == PlatformCommitDisposition::kCommitted) {
    temporary_open_ = false;
    temporary_ = {};
    ReleaseReservation();
    state_ = TransactionState::kCommitted;
    terminal_result_ = {};
    return terminal_result_;
  }
  if (committed.disposition == PlatformCommitDisposition::kOutcomeUncertain) {
    // Commit consumed the handle, but retain fail-closed accounting until
    // startup cleanup reconciles any orphan left by an uncertain rename.
    temporary_open_ = false;
    temporary_ = {};
    temporary_budget_->MarkUnresolved(request_.declared_size());
    reservation_held_ = false;
    state_ = TransactionState::kOutcomeUncertain;
    terminal_result_ = {.error = TransactionError::kOutcomeUncertain,
                        .platform_error = committed.error};
    return terminal_result_;
  }

  return FailAndCleanup(
      ErrorForPlatform(committed.error, TransactionError::kCommitFailed),
      committed.error);
}

TransactionResult ReceiveTransaction::Abort() {
  const std::scoped_lock lock(mutex_);
  if (state_ == TransactionState::kCreated) {
    state_ = TransactionState::kAborted;
    terminal_result_ = {};
    return terminal_result_;
  }
  if (state_ == TransactionState::kReceiving) {
    return FailAndCleanup(TransactionError::kNone);
  }
  if (state_ == TransactionState::kAborted ||
      state_ == TransactionState::kCleanupUnresolved ||
      state_ == TransactionState::kOutcomeUncertain) {
    return terminal_result_;
  }
  return {.error = TransactionError::kInvalidState};
}

TransactionState ReceiveTransaction::state() const {
  const std::scoped_lock lock(mutex_);
  return state_;
}

std::uint64_t ReceiveTransaction::received_bytes() const {
  const std::scoped_lock lock(mutex_);
  return received_bytes_;
}

TransactionResult ReceiveTransaction::FailAndCleanup(
    const TransactionError error, const PlatformError platform_error) {
  TransactionResult result{.error = error, .platform_error = platform_error};
  if (temporary_open_) {
    const PlatformResult cleanup = platform_.CleanupTemporary(temporary_);
    if (!cleanup.ok()) {
      result.cleanup_error = cleanup.error;
      result.cleanup_unresolved = true;
      if (result.error == TransactionError::kNone) {
        result.error = TransactionError::kCleanupUnresolved;
      }
      temporary_open_ = false;
      temporary_ = {};
      temporary_budget_->MarkUnresolved(request_.declared_size());
      reservation_held_ = false;
      state_ = TransactionState::kCleanupUnresolved;
      terminal_result_ = result;
      return terminal_result_;
    }
    temporary_open_ = false;
    temporary_ = {};
  }
  ReleaseReservation();
  state_ = TransactionState::kAborted;
  terminal_result_ = result;
  return terminal_result_;
}

void ReceiveTransaction::ReleaseReservation() noexcept {
  if (!reservation_held_) {
    return;
  }
  temporary_budget_->Release(request_.declared_size());
  reservation_held_ = false;
}

}  // namespace xnn_transfer::core::storage
