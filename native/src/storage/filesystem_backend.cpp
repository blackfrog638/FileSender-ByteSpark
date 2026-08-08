#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "xnn_transfer/core/storage/storage.hpp"

#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace xnn_transfer::core::storage {

#if !defined(_WIN32)
namespace {

constexpr std::string_view kTemporaryDirectory = ".xnn-transfer-tmp";
constexpr std::string_view kLockFile = ".lock";
constexpr std::string_view kTemporaryPrefix = "part-";

[[nodiscard]] PlatformError ErrorFromErrno(const int error) noexcept {
  switch (error) {
    case ENOSPC:
#if defined(EDQUOT)
    case EDQUOT:
#endif
      return PlatformError::kNoSpace;
    case EACCES:
    case EPERM:
    case EROFS:
      return PlatformError::kPermissionDenied;
    case EEXIST:
      return PlatformError::kDestinationExists;
    case EBUSY:
    case EWOULDBLOCK:
      return PlatformError::kBusy;
    default:
      return PlatformError::kIoFailure;
  }
}

[[nodiscard]] bool StartsWith(const std::string_view value,
                              const std::string_view prefix) noexcept {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] PlatformResult CleanupStaleFiles(const int temporary_directory_fd) {
  const int scan_fd = ::dup(temporary_directory_fd);
  if (scan_fd < 0) {
    return {.error = ErrorFromErrno(errno)};
  }
  DIR* const directory = ::fdopendir(scan_fd);
  if (directory == nullptr) {
    const int error = errno;
    ::close(scan_fd);
    return {.error = ErrorFromErrno(error)};
  }

  PlatformError first_error = PlatformError::kNone;
  errno = 0;
  while (dirent* const entry = ::readdir(directory)) {
    const std::string_view name(entry->d_name);
    if (!StartsWith(name, kTemporaryPrefix)) {
      continue;
    }
    if (::unlinkat(temporary_directory_fd, entry->d_name, 0) != 0 && errno != ENOENT &&
        first_error == PlatformError::kNone) {
      first_error = ErrorFromErrno(errno);
    }
    errno = 0;
  }
  if (errno != 0 && first_error == PlatformError::kNone) {
    first_error = ErrorFromErrno(errno);
  }
  if (::closedir(directory) != 0 && first_error == PlatformError::kNone) {
    first_error = ErrorFromErrno(errno);
  }
  return {.error = first_error};
}

struct OpenTemporary {
  int fd{-1};
  std::string name{};
};

struct DestinationParent {
  int fd{-1};
  std::string filename{};
  PlatformError error{PlatformError::kNone};
};

class PosixFilesystemBackend final : public PlatformBackend {
 public:
  PosixFilesystemBackend(const int root_fd, const int temporary_directory_fd,
                         const int lock_fd) noexcept
      : root_fd_(root_fd),
        temporary_directory_fd_(temporary_directory_fd),
        lock_fd_(lock_fd),
        name_seed_(static_cast<std::uint64_t>(
                       std::chrono::steady_clock::now().time_since_epoch().count()) ^
                   static_cast<std::uint64_t>(::getpid())) {}

  ~PosixFilesystemBackend() override {
    const std::scoped_lock lock(mutex_);
    for (const auto& [handle, temporary] : temporaries_) {
      static_cast<void>(handle);
      if (temporary.fd >= 0) {
        ::close(temporary.fd);
      }
      if (!temporary.name.empty()) {
        ::unlinkat(temporary_directory_fd_, temporary.name.c_str(), 0);
      }
    }
    temporaries_.clear();
    if (lock_fd_ >= 0) {
      ::close(lock_fd_);
    }
    if (temporary_directory_fd_ >= 0) {
      ::close(temporary_directory_fd_);
    }
    if (root_fd_ >= 0) {
      ::close(root_fd_);
    }
  }

  [[nodiscard]] PlatformResult CreateTemporary(const ValidatedReceivePath& path,
                                               const std::uint64_t declared_size,
                                               TemporaryFileHandle& output) override {
    static_cast<void>(path);
    const std::scoped_lock lock(mutex_);
    output = {};

    struct statvfs filesystem{};
    if (::fstatvfs(temporary_directory_fd_, &filesystem) != 0) {
      return {.error = ErrorFromErrno(errno)};
    }
    const std::uint64_t block_size = static_cast<std::uint64_t>(filesystem.f_frsize);
    if (block_size == 0) {
      return {.error = PlatformError::kIoFailure};
    }
    const std::uint64_t required_blocks =
        (declared_size / block_size) + (declared_size % block_size == 0 ? 0U : 1U);
    if (required_blocks > static_cast<std::uint64_t>(filesystem.f_bavail)) {
      return {.error = PlatformError::kNoSpace};
    }

    for (std::size_t attempt = 0; attempt < 128; ++attempt) {
      const std::uint64_t token = name_seed_ + next_name_++;
      std::array<char, 32> buffer{};
      const int length = std::snprintf(buffer.data(), buffer.size(), "part-%016llx",
                                       static_cast<unsigned long long>(token));
      if (length <= 0 || static_cast<std::size_t>(length) >= buffer.size()) {
        return {.error = PlatformError::kIoFailure};
      }
      const std::string name(buffer.data(), static_cast<std::size_t>(length));
      const int fd = ::openat(temporary_directory_fd_, name.c_str(),
                              O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                              S_IRUSR | S_IWUSR);
      if (fd < 0) {
        if (errno == EEXIST) {
          continue;
        }
        return {.error = ErrorFromErrno(errno)};
      }

      struct stat metadata{};
      if (::fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
          metadata.st_nlink != 1) {
        const int error = errno == 0 ? EIO : errno;
        ::close(fd);
        ::unlinkat(temporary_directory_fd_, name.c_str(), 0);
        return {.error = ErrorFromErrno(error)};
      }

      const std::uint64_t handle = NextHandle();
      temporaries_.emplace(handle, OpenTemporary{.fd = fd, .name = name});
      output = {.value = handle};
      return {};
    }
    return {.error = PlatformError::kBusy};
  }

  [[nodiscard]] PlatformWriteResult WriteTemporary(
      const TemporaryFileHandle handle,
      const std::span<const std::uint8_t> data) override {
    const std::scoped_lock lock(mutex_);
    const auto iterator = temporaries_.find(handle.value);
    if (iterator == temporaries_.end()) {
      return {.error = PlatformError::kIoFailure};
    }

    std::size_t total = 0;
    while (total < data.size()) {
      const std::size_t remaining = data.size() - total;
      const std::size_t chunk = std::min(
          remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
      const ssize_t written = ::write(iterator->second.fd, data.data() + total, chunk);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        return {.error = ErrorFromErrno(errno), .bytes_written = total};
      }
      if (written == 0) {
        return {.error = PlatformError::kIoFailure, .bytes_written = total};
      }
      total += static_cast<std::size_t>(written);
    }
    return {.bytes_written = total};
  }

  [[nodiscard]] PlatformResult FlushTemporary(
      const TemporaryFileHandle handle) override {
    const std::scoped_lock lock(mutex_);
    const auto iterator = temporaries_.find(handle.value);
    if (iterator == temporaries_.end()) {
      return {.error = PlatformError::kIoFailure};
    }
    if (::fsync(iterator->second.fd) != 0) {
      return {.error = ErrorFromErrno(errno)};
    }
    return {};
  }

  [[nodiscard]] PlatformCommitResult CommitTemporary(
      const TemporaryFileHandle handle,
      const ValidatedReceivePath& destination) override {
    const std::scoped_lock lock(mutex_);
    const auto iterator = temporaries_.find(handle.value);
    if (iterator == temporaries_.end()) {
      return {.error = PlatformError::kIoFailure};
    }

    DestinationParent parent = OpenDestinationParent(destination);
    if (parent.fd < 0) {
      return {.error = parent.error};
    }

    if (::linkat(temporary_directory_fd_, iterator->second.name.c_str(), parent.fd,
                 parent.filename.c_str(), 0) != 0) {
      const PlatformError error = ErrorFromErrno(errno);
      ::close(parent.fd);
      return {.error = error};
    }

    PlatformError uncertain_error = PlatformError::kNone;
    if (::fsync(parent.fd) != 0) {
      uncertain_error = ErrorFromErrno(errno);
    }
    if (::close(parent.fd) != 0 && uncertain_error == PlatformError::kNone) {
      uncertain_error = ErrorFromErrno(errno);
    }
    if (::unlinkat(temporary_directory_fd_, iterator->second.name.c_str(), 0) != 0 &&
        errno != ENOENT && uncertain_error == PlatformError::kNone) {
      uncertain_error = ErrorFromErrno(errno);
    }
    if (::fsync(temporary_directory_fd_) != 0 &&
        uncertain_error == PlatformError::kNone) {
      uncertain_error = ErrorFromErrno(errno);
    }
    if (::close(iterator->second.fd) != 0 && uncertain_error == PlatformError::kNone) {
      uncertain_error = ErrorFromErrno(errno);
    }
    temporaries_.erase(iterator);

    if (uncertain_error != PlatformError::kNone) {
      return {.disposition = PlatformCommitDisposition::kOutcomeUncertain,
              .error = uncertain_error};
    }
    return {.disposition = PlatformCommitDisposition::kCommitted};
  }

  [[nodiscard]] PlatformResult CleanupTemporary(
      const TemporaryFileHandle handle) override {
    const std::scoped_lock lock(mutex_);
    const auto iterator = temporaries_.find(handle.value);
    if (iterator == temporaries_.end()) {
      return {.error = PlatformError::kIoFailure};
    }

    OpenTemporary temporary = std::move(iterator->second);
    temporaries_.erase(iterator);
    PlatformError first_error = PlatformError::kNone;
    if (::close(temporary.fd) != 0) {
      first_error = ErrorFromErrno(errno);
    }
    if (::unlinkat(temporary_directory_fd_, temporary.name.c_str(), 0) != 0 &&
        errno != ENOENT && first_error == PlatformError::kNone) {
      first_error = ErrorFromErrno(errno);
    }
    if (::fsync(temporary_directory_fd_) != 0 && first_error == PlatformError::kNone) {
      first_error = ErrorFromErrno(errno);
    }
    return {.error = first_error};
  }

 private:
  [[nodiscard]] std::uint64_t NextHandle() noexcept {
    while (next_handle_ == 0 || temporaries_.contains(next_handle_)) {
      ++next_handle_;
    }
    return next_handle_++;
  }

  [[nodiscard]] DestinationParent OpenDestinationParent(
      const ValidatedReceivePath& destination) const {
    const std::span<const std::string> components = destination.components();
    if (components.empty()) {
      return {.error = PlatformError::kIoFailure};
    }

    int current = ::dup(root_fd_);
    if (current < 0) {
      return {.error = ErrorFromErrno(errno)};
    }
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
      const std::string& component = components[index];
      int next = ::openat(current, component.c_str(),
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (next < 0 && errno == ENOENT) {
        if (::mkdirat(current, component.c_str(), S_IRWXU) != 0 && errno != EEXIST) {
          const PlatformError error = ErrorFromErrno(errno);
          ::close(current);
          return {.error = error};
        }
        next = ::openat(current, component.c_str(),
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      }
      if (next < 0) {
        const PlatformError error = errno == ELOOP || errno == ENOTDIR
                                        ? PlatformError::kInvalidRoot
                                        : ErrorFromErrno(errno);
        ::close(current);
        return {.error = error};
      }
      ::close(current);
      current = next;
    }

    return {
        .fd = current, .filename = components.back(), .error = PlatformError::kNone};
  }

  mutable std::mutex mutex_{};
  int root_fd_{-1};
  int temporary_directory_fd_{-1};
  int lock_fd_{-1};
  std::unordered_map<std::uint64_t, OpenTemporary> temporaries_{};
  std::uint64_t next_handle_{1};
  std::uint64_t name_seed_{};
  std::uint64_t next_name_{1};
};

[[nodiscard]] FilesystemBackendOpenResult OpenPosixBackend(
    const std::string_view destination_root_utf8) {
  if (destination_root_utf8.empty() ||
      destination_root_utf8.find('\0') != std::string_view::npos) {
    return {.error = PlatformError::kInvalidRoot};
  }
  const std::string root_path(destination_root_utf8);
  const int root_fd =
      ::open(root_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (root_fd < 0) {
    return {.error = errno == ELOOP || errno == ENOTDIR ? PlatformError::kInvalidRoot
                                                        : ErrorFromErrno(errno)};
  }

  if (::mkdirat(root_fd, kTemporaryDirectory.data(), S_IRWXU) != 0 && errno != EEXIST) {
    const PlatformError error = ErrorFromErrno(errno);
    ::close(root_fd);
    return {.error = error};
  }
  const int temporary_directory_fd =
      ::openat(root_fd, kTemporaryDirectory.data(),
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (temporary_directory_fd < 0) {
    const PlatformError error = errno == ELOOP || errno == ENOTDIR
                                    ? PlatformError::kInvalidRoot
                                    : ErrorFromErrno(errno);
    ::close(root_fd);
    return {.error = error};
  }

  struct stat temporary_metadata{};
  if (::fstat(temporary_directory_fd, &temporary_metadata) != 0 ||
      !S_ISDIR(temporary_metadata.st_mode) ||
      temporary_metadata.st_uid != ::geteuid() ||
      (temporary_metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    ::close(temporary_directory_fd);
    ::close(root_fd);
    return {.error = PlatformError::kInvalidRoot};
  }

  const int lock_fd =
      ::openat(temporary_directory_fd, kLockFile.data(),
               O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (lock_fd < 0) {
    const PlatformError error = ErrorFromErrno(errno);
    ::close(temporary_directory_fd);
    ::close(root_fd);
    return {.error = error};
  }
  struct stat lock_metadata{};
  if (::fstat(lock_fd, &lock_metadata) != 0 || !S_ISREG(lock_metadata.st_mode) ||
      lock_metadata.st_uid != ::geteuid() || lock_metadata.st_nlink != 1) {
    ::close(lock_fd);
    ::close(temporary_directory_fd);
    ::close(root_fd);
    return {.error = PlatformError::kInvalidRoot};
  }
  if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    const PlatformError error =
        errno == EWOULDBLOCK ? PlatformError::kBusy : ErrorFromErrno(errno);
    ::close(lock_fd);
    ::close(temporary_directory_fd);
    ::close(root_fd);
    return {.error = error};
  }

  const PlatformResult cleanup = CleanupStaleFiles(temporary_directory_fd);
  if (!cleanup.ok()) {
    ::close(lock_fd);
    ::close(temporary_directory_fd);
    ::close(root_fd);
    return {.error = cleanup.error};
  }

  return {.backend = std::make_unique<PosixFilesystemBackend>(
              root_fd, temporary_directory_fd, lock_fd)};
}

}  // namespace
#endif

FilesystemBackendOpenResult OpenFilesystemBackend(
    const std::string_view destination_root_utf8) {
#if defined(_WIN32)
  static_cast<void>(destination_root_utf8);
  return {.error = PlatformError::kUnsupported};
#else
  return OpenPosixBackend(destination_root_utf8);
#endif
}

}  // namespace xnn_transfer::core::storage
