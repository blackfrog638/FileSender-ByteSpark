#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include "platform_protected_store_internal.hpp"

namespace xnn_transfer::core::security::identity::internal {
namespace {

constexpr char kRuntimeLockName[] = ".xnn-transfer-identity.lock";

[[nodiscard]] ErrorCode MapErrno(const int error) noexcept {
  if (error == EACCES || error == EPERM || error == EROFS) {
    return ErrorCode::kPermissionDenied;
  }
  return ErrorCode::kStorageUnavailable;
}

class PosixLockGuard final : public PlatformStoreLockGuard {
 public:
  explicit PosixLockGuard(const int descriptor) : descriptor_(descriptor) {}
  ~PosixLockGuard() override {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
  }

  PosixLockGuard(const PosixLockGuard&) = delete;
  PosixLockGuard& operator=(const PosixLockGuard&) = delete;

 private:
  int descriptor_;
};

class PosixDirectoryLock final : public PlatformStoreOperationLock {
 public:
  explicit PosixDirectoryLock(std::string runtime_directory)
      : runtime_directory_(std::move(runtime_directory)) {}

  Result<std::unique_ptr<PlatformStoreLockGuard>> Acquire() override {
    if (runtime_directory_.empty() || runtime_directory_.front() != '/' ||
        runtime_directory_.find('\0') != std::string::npos) {
      return Result<std::unique_ptr<PlatformStoreLockGuard>>::Failure(
          ErrorCode::kStorageUnavailable);
    }

    const int directory = open(runtime_directory_.c_str(),
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) {
      return Result<std::unique_ptr<PlatformStoreLockGuard>>::Failure(MapErrno(errno));
    }

    struct stat directory_status{};
    if (fstat(directory, &directory_status) != 0 ||
        !S_ISDIR(directory_status.st_mode) || directory_status.st_uid != geteuid() ||
        (directory_status.st_mode & 0077) != 0) {
      close(directory);
      return Result<std::unique_ptr<PlatformStoreLockGuard>>::Failure(
          ErrorCode::kPermissionDenied);
    }

    const int descriptor = openat(directory, kRuntimeLockName,
                                  O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    const int open_error = errno;
    close(directory);
    if (descriptor < 0) {
      return Result<std::unique_ptr<PlatformStoreLockGuard>>::Failure(
          MapErrno(open_error));
    }

    struct stat lock_status{};
    if (fstat(descriptor, &lock_status) != 0 || !S_ISREG(lock_status.st_mode) ||
        lock_status.st_uid != geteuid() || lock_status.st_nlink != 1 ||
        (lock_status.st_mode & 0777) != 0600) {
      close(descriptor);
      return Result<std::unique_ptr<PlatformStoreLockGuard>>::Failure(
          ErrorCode::kPermissionDenied);
    }

    int result = 0;
    do {
      result = flock(descriptor, LOCK_EX);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
      const ErrorCode error = MapErrno(errno);
      close(descriptor);
      return Result<std::unique_ptr<PlatformStoreLockGuard>>::Failure(error);
    }

    try {
      return Result<std::unique_ptr<PlatformStoreLockGuard>>::Success(
          std::make_unique<PosixLockGuard>(descriptor));
    } catch (const std::bad_alloc&) {
      close(descriptor);
      return Result<std::unique_ptr<PlatformStoreLockGuard>>::Failure(
          ErrorCode::kCapacityExceeded);
    }
  }

 private:
  std::string runtime_directory_;
};

}  // namespace

std::unique_ptr<PlatformStoreOperationLock> MakePosixDirectoryLock(
    std::string runtime_directory) {
  return std::make_unique<PosixDirectoryLock>(std::move(runtime_directory));
}

}  // namespace xnn_transfer::core::security::identity::internal
