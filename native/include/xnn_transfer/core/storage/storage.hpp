#ifndef XNN_TRANSFER_CORE_STORAGE_STORAGE_HPP_
#define XNN_TRANSFER_CORE_STORAGE_STORAGE_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xnn_transfer::core::storage {

inline constexpr std::size_t kMaxRelativePathBytes = 1'024;
inline constexpr std::size_t kMaxPathComponents = 32;
inline constexpr std::size_t kMaxPathComponentBytes = 255;
inline constexpr std::uint64_t kMaxFileBytes = 17'592'186'044'416;

enum class ValidationError : std::uint8_t {
  kNone,
  kPathEmpty,
  kPathBytesLimit,
  kInvalidUtf8,
  kPathNotNfc,
  kPathAbsolute,
  kPathUnc,
  kPathDriveAbsolute,
  kPathDriveQualified,
  kPathBackslash,
  kPathColonOrAds,
  kPathTrailingSeparator,
  kPathNul,
  kPathC0Control,
  kPathC1Control,
  kPathNoncharacter,
  kPathEmptyComponent,
  kPathDotComponent,
  kPathTraversal,
  kPathReservedComponent,
  kPathComponentCountLimit,
  kPathComponentBytesLimit,
  kDeclaredSizeLimit,
};

class PathValidationResult;
class RequestValidationResult;

class ValidatedReceivePath final {
 public:
  ValidatedReceivePath(const ValidatedReceivePath&) = default;
  ValidatedReceivePath(ValidatedReceivePath&&) = default;
  ValidatedReceivePath& operator=(const ValidatedReceivePath&) = delete;
  ValidatedReceivePath& operator=(ValidatedReceivePath&&) = delete;

  [[nodiscard]] std::string_view utf8() const noexcept { return utf8_; }
  [[nodiscard]] std::span<const std::string> components() const noexcept {
    return components_;
  }

 private:
  friend class PathValidationResult;

  ValidatedReceivePath(std::string utf8, std::vector<std::string> components)
      : utf8_(std::move(utf8)), components_(std::move(components)) {}

  const std::string utf8_;
  const std::vector<std::string> components_;
};

class PathValidationResult final {
 public:
  [[nodiscard]] bool ok() const noexcept { return error_ == ValidationError::kNone; }
  [[nodiscard]] ValidationError error() const noexcept { return error_; }
  [[nodiscard]] const ValidatedReceivePath* path() const noexcept {
    return path_ ? &*path_ : nullptr;
  }

 private:
  friend PathValidationResult ValidateReceivePath(
      std::span<const std::uint8_t> encoded);

  explicit PathValidationResult(ValidationError error) : error_(error) {}
  PathValidationResult(std::string utf8, std::vector<std::string> components)
      : path_(ValidatedReceivePath(std::move(utf8), std::move(components))) {}

  ValidationError error_{ValidationError::kNone};
  std::optional<ValidatedReceivePath> path_;
};

class ValidatedReceiveRequest final {
 public:
  ValidatedReceiveRequest(const ValidatedReceiveRequest&) = default;
  ValidatedReceiveRequest(ValidatedReceiveRequest&&) = default;
  ValidatedReceiveRequest& operator=(const ValidatedReceiveRequest&) = delete;
  ValidatedReceiveRequest& operator=(ValidatedReceiveRequest&&) = delete;

  [[nodiscard]] const ValidatedReceivePath& path() const noexcept { return path_; }
  [[nodiscard]] std::uint64_t declared_size() const noexcept { return declared_size_; }

 private:
  friend class RequestValidationResult;

  ValidatedReceiveRequest(ValidatedReceivePath path, std::uint64_t declared_size)
      : path_(std::move(path)), declared_size_(declared_size) {}

  const ValidatedReceivePath path_;
  const std::uint64_t declared_size_;
};

class RequestValidationResult final {
 public:
  [[nodiscard]] bool ok() const noexcept { return error_ == ValidationError::kNone; }
  [[nodiscard]] ValidationError error() const noexcept { return error_; }
  [[nodiscard]] const ValidatedReceiveRequest* request() const noexcept {
    return request_ ? &*request_ : nullptr;
  }

 private:
  friend RequestValidationResult ValidateReceiveRequest(
      std::span<const std::uint8_t> encoded_path, std::uint64_t declared_size,
      std::uint64_t local_max_file_bytes);

  explicit RequestValidationResult(ValidationError error) : error_(error) {}
  RequestValidationResult(ValidatedReceivePath path, std::uint64_t declared_size)
      : request_(ValidatedReceiveRequest(std::move(path), declared_size)) {}

  ValidationError error_{ValidationError::kNone};
  std::optional<ValidatedReceiveRequest> request_;
};

[[nodiscard]] PathValidationResult ValidateReceivePath(
    std::span<const std::uint8_t> encoded);

[[nodiscard]] RequestValidationResult ValidateReceiveRequest(
    std::span<const std::uint8_t> encoded_path, std::uint64_t declared_size,
    std::uint64_t local_max_file_bytes = kMaxFileBytes);

class TemporaryBudget final {
 public:
  explicit TemporaryBudget(std::uint64_t limit_bytes) noexcept;

  [[nodiscard]] std::uint64_t limit_bytes() const noexcept;
  // Reserved bytes include unresolved orphan bytes and remain fail-closed.
  [[nodiscard]] std::uint64_t reserved_bytes() const noexcept;
  [[nodiscard]] std::uint64_t unresolved_bytes() const noexcept;
  [[nodiscard]] std::size_t unresolved_files() const noexcept;

 private:
  friend class ReceiveTransaction;

  [[nodiscard]] bool TryReserve(std::uint64_t bytes) noexcept;
  void Release(std::uint64_t bytes) noexcept;
  void MarkUnresolved(std::uint64_t bytes) noexcept;

  mutable std::mutex mutex_{};
  std::uint64_t limit_bytes_{};
  std::uint64_t reserved_bytes_{};
  std::uint64_t unresolved_bytes_{};
  std::size_t unresolved_files_{};
};

struct TemporaryFileHandle {
  std::uint64_t value{};

  [[nodiscard]] bool valid() const noexcept { return value != 0; }
};

enum class PlatformError : std::uint8_t {
  kNone,
  kNoSpace,
  kPermissionDenied,
  kDestinationExists,
  kInvalidRoot,
  kBusy,
  kUnsupported,
  kIoFailure,
};

struct PlatformResult {
  PlatformError error{PlatformError::kNone};

  [[nodiscard]] bool ok() const noexcept { return error == PlatformError::kNone; }
};

struct PlatformWriteResult {
  PlatformError error{PlatformError::kNone};
  std::size_t bytes_written{};

  [[nodiscard]] bool ok() const noexcept { return error == PlatformError::kNone; }
};

enum class PlatformCommitDisposition : std::uint8_t {
  kCommitted,
  kNotCommitted,
  kOutcomeUncertain,
};

struct PlatformCommitResult {
  PlatformCommitDisposition disposition{PlatformCommitDisposition::kNotCommitted};
  PlatformError error{PlatformError::kNone};
};

class PlatformBackend {
 public:
  virtual ~PlatformBackend() = default;

  // Failure must leave output invalid or transfer a valid cleanup handle.
  [[nodiscard]] virtual PlatformResult CreateTemporary(const ValidatedReceivePath& path,
                                                       std::uint64_t declared_size,
                                                       TemporaryFileHandle& output) = 0;
  [[nodiscard]] virtual PlatformWriteResult WriteTemporary(
      TemporaryFileHandle handle, std::span<const std::uint8_t> data) = 0;
  [[nodiscard]] virtual PlatformResult FlushTemporary(TemporaryFileHandle handle) = 0;

  // Committed and outcome-uncertain results consume the handle. A
  // not-committed result leaves it available for CleanupTemporary.
  [[nodiscard]] virtual PlatformCommitResult CommitTemporary(
      TemporaryFileHandle handle, const ValidatedReceivePath& destination) = 0;

  // This call always consumes the handle. Success means the temporary object
  // was removed. On failure, the backend owns an inaccessible orphan and must
  // durably track it for startup cleanup.
  [[nodiscard]] virtual PlatformResult CleanupTemporary(TemporaryFileHandle handle) = 0;
};

struct FilesystemBackendOpenResult {
  std::unique_ptr<PlatformBackend> backend{};
  PlatformError error{PlatformError::kNone};

  [[nodiscard]] bool ok() const noexcept {
    return error == PlatformError::kNone && backend != nullptr;
  }
};

// Opens one destination root and removes stale XnnTransfer temporaries while
// holding an exclusive per-root backend lock. Unsupported platforms fail
// closed.
[[nodiscard]] FilesystemBackendOpenResult OpenFilesystemBackend(
    std::string_view destination_root_utf8);

class StreamingIntegrityVerifier {
 public:
  virtual ~StreamingIntegrityVerifier() = default;

  [[nodiscard]] virtual bool Update(std::span<const std::uint8_t> data) = 0;
  [[nodiscard]] virtual bool Seal() = 0;
};

enum class TransactionState : std::uint8_t {
  kCreated,
  kReceiving,
  kCommitted,
  kAborted,
  kCleanupUnresolved,
  kOutcomeUncertain,
};

enum class TransactionError : std::uint8_t {
  kNone,
  kInvalidState,
  kTemporaryBudgetExceeded,
  kNoSpace,
  kOpenFailed,
  kDataTooLarge,
  kDataTooShort,
  kWriteFailed,
  kIntegrityFailed,
  kFlushFailed,
  kCommitFailed,
  kCleanupUnresolved,
  kOutcomeUncertain,
};

struct TransactionResult {
  TransactionError error{TransactionError::kNone};
  PlatformError platform_error{PlatformError::kNone};
  PlatformError cleanup_error{PlatformError::kNone};
  bool cleanup_unresolved{};

  [[nodiscard]] bool ok() const noexcept { return error == TransactionError::kNone; }
};

class ReceiveTransaction final {
 public:
  ReceiveTransaction(ValidatedReceiveRequest request,
                     std::shared_ptr<TemporaryBudget> temporary_budget,
                     std::unique_ptr<StreamingIntegrityVerifier> verifier,
                     PlatformBackend& platform);
  ~ReceiveTransaction();

  ReceiveTransaction(const ReceiveTransaction&) = delete;
  ReceiveTransaction& operator=(const ReceiveTransaction&) = delete;
  ReceiveTransaction(ReceiveTransaction&&) = delete;
  ReceiveTransaction& operator=(ReceiveTransaction&&) = delete;

  [[nodiscard]] TransactionResult Begin();
  [[nodiscard]] TransactionResult Write(std::span<const std::uint8_t> data);
  [[nodiscard]] TransactionResult SealAndCommit();
  [[nodiscard]] TransactionResult Abort();

  [[nodiscard]] TransactionState state() const;
  [[nodiscard]] std::uint64_t received_bytes() const;

 private:
  [[nodiscard]] TransactionResult FailAndCleanup(
      TransactionError error, PlatformError platform_error = PlatformError::kNone);
  void ReleaseReservation() noexcept;

  mutable std::mutex mutex_{};
  ValidatedReceiveRequest request_;
  std::shared_ptr<TemporaryBudget> temporary_budget_{};
  std::unique_ptr<StreamingIntegrityVerifier> verifier_{};
  PlatformBackend& platform_;
  TemporaryFileHandle temporary_{};
  TransactionState state_{TransactionState::kCreated};
  std::uint64_t received_bytes_{};
  bool reservation_held_{};
  bool temporary_open_{};
  TransactionResult terminal_result_{};
};

}  // namespace xnn_transfer::core::storage

#endif  // XNN_TRANSFER_CORE_STORAGE_STORAGE_HPP_
