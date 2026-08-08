#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <Aclapi.h>
#include <Windows.h>
#include <bcrypt.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xnn_transfer/core/storage/storage.hpp"

namespace xnn_transfer::core::storage {
namespace {

constexpr std::wstring_view kTemporaryDirectory = L".xnn-transfer-tmp";
constexpr std::wstring_view kLockFile = L".lock";
constexpr std::wstring_view kTemporaryPrefix = L"part-";
constexpr std::size_t kMaxStaleTemporaries = 4'096;
constexpr ULONG kObjectCaseInsensitive = 0x00000040UL;
constexpr ULONG kObjectDontReparse = 0x00001000UL;
constexpr ULONG kFileDirectoryFile = 0x00000001UL;
constexpr ULONG kFileSynchronousIoNonAlert = 0x00000020UL;
constexpr ULONG kFileNonDirectoryFile = 0x00000040UL;
constexpr ULONG kFileOpenForBackupIntent = 0x00004000UL;
constexpr ULONG kFileOpenReparsePoint = 0x00200000UL;
constexpr ULONG kFileOpen = 0x00000001UL;
constexpr ULONG kFileCreate = 0x00000002UL;
constexpr ULONG kFileOpenIf = 0x00000003UL;
constexpr ULONG kFileDispositionDelete = 0x00000001UL;
constexpr ULONG kFileDispositionPosixSemantics = 0x00000002UL;
constexpr ULONG kFileDispositionIgnoreReadonly = 0x00000010UL;
constexpr FILE_INFO_BY_HANDLE_CLASS kFileDispositionInfoEx =
    static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);
constexpr FILE_INFO_BY_HANDLE_CLASS kFileRenameInfoEx =
    static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);
constexpr FILE_INFORMATION_CLASS kNativeFileRenameInformationEx =
    static_cast<FILE_INFORMATION_CLASS>(65);

constexpr ACCESS_MASK kDirectoryAccess =
    FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_READ_ATTRIBUTES |
    FILE_WRITE_ATTRIBUTES | FILE_TRAVERSE | READ_CONTROL | SYNCHRONIZE;
constexpr ACCESS_MASK kRegularFileAccess =
    FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE | READ_CONTROL | SYNCHRONIZE;
constexpr ACCESS_MASK kCleanupAccess =
    DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;
constexpr ULONG kDirectoryShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;

using NtCreateFileFunction = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
                                              PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG,
                                              ULONG, ULONG, ULONG, PVOID, ULONG);
using NtFlushBuffersFileExFunction = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG,
                                                      PIO_STATUS_BLOCK);
using NtSetInformationFileFunction = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID,
                                                      ULONG, FILE_INFORMATION_CLASS);
using RtlNtStatusToDosErrorFunction = ULONG(NTAPI*)(NTSTATUS);

[[nodiscard]] bool NtSucceeded(const NTSTATUS status) noexcept { return status >= 0; }

class UniqueHandle final {
 public:
  UniqueHandle() = default;
  explicit UniqueHandle(const HANDLE handle) noexcept : handle_(handle) {}
  ~UniqueHandle() { static_cast<void>(Reset()); }

  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.Release()) {}
  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
      static_cast<void>(Reset());
      handle_ = other.Release();
    }
    return *this;
  }

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] bool valid() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  [[nodiscard]] HANDLE Release() noexcept {
    const HANDLE result = handle_;
    handle_ = INVALID_HANDLE_VALUE;
    return result;
  }

  [[nodiscard]] bool Reset(const HANDLE replacement = INVALID_HANDLE_VALUE) noexcept {
    bool closed = true;
    if (valid()) {
      closed = ::CloseHandle(handle_) != FALSE;
    }
    handle_ = replacement;
    return closed;
  }

 private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
};

class NativeApi final {
 public:
  [[nodiscard]] bool Initialize() noexcept {
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
      return false;
    }
    nt_create_file_ =
        reinterpret_cast<NtCreateFileFunction>(::GetProcAddress(ntdll, "NtCreateFile"));
    nt_flush_buffers_file_ex_ = reinterpret_cast<NtFlushBuffersFileExFunction>(
        ::GetProcAddress(ntdll, "NtFlushBuffersFileEx"));
    nt_set_information_file_ = reinterpret_cast<NtSetInformationFileFunction>(
        ::GetProcAddress(ntdll, "NtSetInformationFile"));
    rtl_nt_status_to_dos_error_ = reinterpret_cast<RtlNtStatusToDosErrorFunction>(
        ::GetProcAddress(ntdll, "RtlNtStatusToDosError"));
    return nt_create_file_ != nullptr && nt_flush_buffers_file_ex_ != nullptr &&
           nt_set_information_file_ != nullptr &&
           rtl_nt_status_to_dos_error_ != nullptr;
  }

  [[nodiscard]] NtCreateFileFunction create_file() const noexcept {
    return nt_create_file_;
  }
  [[nodiscard]] NtFlushBuffersFileExFunction flush_file() const noexcept {
    return nt_flush_buffers_file_ex_;
  }
  [[nodiscard]] NtSetInformationFileFunction set_information() const noexcept {
    return nt_set_information_file_;
  }
  [[nodiscard]] DWORD DosError(const NTSTATUS status) const noexcept {
    return static_cast<DWORD>(rtl_nt_status_to_dos_error_(status));
  }

 private:
  NtCreateFileFunction nt_create_file_{};
  NtFlushBuffersFileExFunction nt_flush_buffers_file_ex_{};
  NtSetInformationFileFunction nt_set_information_file_{};
  RtlNtStatusToDosErrorFunction rtl_nt_status_to_dos_error_{};
};

[[nodiscard]] PlatformError ErrorFromWindows(const DWORD error) noexcept {
  switch (error) {
    case ERROR_SUCCESS:
      return PlatformError::kNone;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
    case ERROR_NOT_ENOUGH_QUOTA:
      return PlatformError::kNoSpace;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_WRITE_PROTECT:
      return PlatformError::kPermissionDenied;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
      return PlatformError::kDestinationExists;
    case ERROR_INVALID_NAME:
    case ERROR_DIRECTORY:
    case ERROR_CANT_ACCESS_FILE:
    case ERROR_INVALID_REPARSE_DATA:
    case ERROR_REPARSE_TAG_INVALID:
    case ERROR_REPARSE_TAG_MISMATCH:
      return PlatformError::kInvalidRoot;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_BUSY:
      return PlatformError::kBusy;
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION:
    case ERROR_CALL_NOT_IMPLEMENTED:
      return PlatformError::kUnsupported;
    default:
      return PlatformError::kIoFailure;
  }
}

[[nodiscard]] bool IsUnsupportedInformationError(const DWORD error) noexcept {
  return error == ERROR_INVALID_PARAMETER || error == ERROR_INVALID_FUNCTION ||
         error == ERROR_NOT_SUPPORTED || error == ERROR_CALL_NOT_IMPLEMENTED;
}

[[nodiscard]] bool IsMissingError(const DWORD error) noexcept {
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

[[nodiscard]] bool ConvertUtf8Strict(const std::string_view utf8,
                                     std::wstring& output) {
  output.clear();
  if (utf8.empty() || utf8.find('\0') != std::string_view::npos ||
      utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  const int input_size = static_cast<int>(utf8.size());
  const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                             input_size, nullptr, 0);
  if (required <= 0) {
    return false;
  }
  output.resize(static_cast<std::size_t>(required));
  return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), input_size,
                               output.data(), required) == required;
}

class PrivateSecurity final {
 public:
  [[nodiscard]] bool Initialize() {
    HANDLE raw_token = INVALID_HANDLE_VALUE;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw_token) == FALSE) {
      return false;
    }
    UniqueHandle token(raw_token);

    DWORD token_bytes = 0;
    static_cast<void>(
        ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &token_bytes));
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || token_bytes == 0) {
      return false;
    }
    std::vector<std::byte> token_storage(token_bytes);
    if (::GetTokenInformation(token.get(), TokenUser, token_storage.data(), token_bytes,
                              &token_bytes) == FALSE) {
      return false;
    }
    const auto* const token_user =
        reinterpret_cast<const TOKEN_USER*>(token_storage.data());
    if (::IsValidSid(token_user->User.Sid) == FALSE) {
      return false;
    }

    const DWORD sid_bytes = ::GetLengthSid(token_user->User.Sid);
    owner_sid_.resize(sid_bytes);
    if (::CopySid(sid_bytes, owner_sid_.data(), token_user->User.Sid) == FALSE) {
      return false;
    }

    const DWORD acl_bytes = static_cast<DWORD>(
        sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + sid_bytes);
    acl_.resize(acl_bytes);
    auto* const acl = reinterpret_cast<PACL>(acl_.data());
    if (::InitializeAcl(acl, acl_bytes, ACL_REVISION) == FALSE ||
        ::AddAccessAllowedAceEx(acl, ACL_REVISION,
                                OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE,
                                FILE_ALL_ACCESS, owner_sid_.data()) == FALSE ||
        ::InitializeSecurityDescriptor(&descriptor_, SECURITY_DESCRIPTOR_REVISION) ==
            FALSE ||
        ::SetSecurityDescriptorOwner(&descriptor_, owner_sid_.data(), FALSE) == FALSE ||
        ::SetSecurityDescriptorDacl(&descriptor_, TRUE, acl, FALSE) == FALSE ||
        ::SetSecurityDescriptorControl(&descriptor_, SE_DACL_PROTECTED,
                                       SE_DACL_PROTECTED) == FALSE) {
      return false;
    }
    return true;
  }

  [[nodiscard]] PSECURITY_DESCRIPTOR descriptor() noexcept { return &descriptor_; }
  [[nodiscard]] PSID owner_sid() noexcept { return owner_sid_.data(); }

 private:
  std::vector<std::byte> owner_sid_{};
  std::vector<std::byte> acl_{};
  SECURITY_DESCRIPTOR descriptor_{};
};

[[nodiscard]] PlatformError ValidatePrivateSecurity(const HANDLE handle,
                                                    const PSID expected_owner) {
  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD result = ::GetSecurityInfo(
      handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      &owner, nullptr, &dacl, nullptr, &descriptor);
  const std::unique_ptr<void, decltype(&::LocalFree)> descriptor_owner(descriptor,
                                                                       &::LocalFree);
  if (result != ERROR_SUCCESS) {
    return ErrorFromWindows(result);
  }
  if (owner == nullptr || ::EqualSid(owner, expected_owner) == FALSE ||
      dacl == nullptr || ::IsValidAcl(dacl) == FALSE) {
    return PlatformError::kInvalidRoot;
  }

  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  if (::GetSecurityDescriptorControl(descriptor, &control, &revision) == FALSE ||
      (control & SE_DACL_PROTECTED) == 0) {
    return PlatformError::kInvalidRoot;
  }

  ACL_SIZE_INFORMATION acl_information{};
  if (::GetAclInformation(dacl, &acl_information, sizeof(acl_information),
                          AclSizeInformation) == FALSE) {
    return PlatformError::kInvalidRoot;
  }
  ACCESS_MASK granted = 0;
  for (DWORD index = 0; index < acl_information.AceCount; ++index) {
    void* raw_ace = nullptr;
    if (::GetAce(dacl, index, &raw_ace) == FALSE || raw_ace == nullptr) {
      return PlatformError::kInvalidRoot;
    }
    const auto* const header = static_cast<const ACE_HEADER*>(raw_ace);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE ||
        (header->AceFlags & INHERITED_ACE) != 0) {
      return PlatformError::kInvalidRoot;
    }
    const auto* const ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
    PSID const sid = const_cast<DWORD*>(&ace->SidStart);
    if (::IsValidSid(sid) == FALSE || ::EqualSid(sid, expected_owner) == FALSE) {
      return PlatformError::kInvalidRoot;
    }
    granted |= ace->Mask;
  }
  return (granted & FILE_ALL_ACCESS) == FILE_ALL_ACCESS ? PlatformError::kNone
                                                        : PlatformError::kInvalidRoot;
}

enum class ExpectedObjectType : std::uint8_t {
  kDirectory,
  kRegular,
  kAny,
};

[[nodiscard]] PlatformError ValidateObject(const HANDLE handle,
                                           const ExpectedObjectType expected_type,
                                           const bool require_one_link) {
  BY_HANDLE_FILE_INFORMATION information{};
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (::GetFileInformationByHandle(handle, &information) == FALSE ||
      ::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) ==
          FALSE) {
    return ErrorFromWindows(::GetLastError());
  }
  if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || tag.ReparseTag != 0) {
    return PlatformError::kInvalidRoot;
  }
  const bool directory = (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  if ((expected_type == ExpectedObjectType::kDirectory && !directory) ||
      (expected_type == ExpectedObjectType::kRegular && directory) ||
      (require_one_link && information.nNumberOfLinks != 1)) {
    return PlatformError::kInvalidRoot;
  }
  return PlatformError::kNone;
}

[[nodiscard]] bool IsLocalFixedNtfs(const HANDLE root) {
  std::array<wchar_t, 32> filesystem{};
  if (::GetVolumeInformationByHandleW(root, nullptr, 0, nullptr, nullptr, nullptr,
                                      filesystem.data(),
                                      static_cast<DWORD>(filesystem.size())) == FALSE ||
      ::_wcsicmp(filesystem.data(), L"NTFS") != 0) {
    return false;
  }

  const DWORD required = ::GetFinalPathNameByHandleW(
      root, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
  if (required == 0) {
    return false;
  }
  std::wstring final_path(static_cast<std::size_t>(required), L'\0');
  const DWORD written = ::GetFinalPathNameByHandleW(
      root, final_path.data(), required, FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
  if (written == 0 || written >= required) {
    return false;
  }
  final_path.resize(written);
  const std::size_t volume_end = final_path.find(L"}\\");
  if (!final_path.starts_with(L"\\\\?\\Volume{") || volume_end == std::wstring::npos) {
    return false;
  }
  const std::wstring volume_root = final_path.substr(0, volume_end + 2);
  return ::GetDriveTypeW(volume_root.c_str()) == DRIVE_FIXED;
}

struct RelativeOpenResult {
  UniqueHandle handle{};
  DWORD error{ERROR_SUCCESS};
};

[[nodiscard]] RelativeOpenResult OpenRelative(
    const NativeApi& api, const HANDLE root, const std::wstring_view name,
    const ACCESS_MASK desired_access, const ULONG share_access, const ULONG disposition,
    const ULONG create_options, PSECURITY_DESCRIPTOR security_descriptor,
    const bool dont_reparse = true) {
  if (name.empty() ||
      name.size() > static_cast<std::size_t>(std::numeric_limits<USHORT>::max() /
                                             sizeof(wchar_t))) {
    return {.error = ERROR_INVALID_NAME};
  }
  UNICODE_STRING unicode_name{};
  unicode_name.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
  unicode_name.MaximumLength = unicode_name.Length;
  unicode_name.Buffer = const_cast<PWSTR>(name.data());

  OBJECT_ATTRIBUTES attributes{};
  attributes.Length = sizeof(attributes);
  attributes.RootDirectory = root;
  attributes.ObjectName = &unicode_name;
  attributes.Attributes =
      kObjectCaseInsensitive | (dont_reparse ? kObjectDontReparse : 0UL);
  attributes.SecurityDescriptor = security_descriptor;

  HANDLE raw_handle = INVALID_HANDLE_VALUE;
  IO_STATUS_BLOCK status_block{};
  const NTSTATUS status = api.create_file()(
      &raw_handle, desired_access, &attributes, &status_block, nullptr,
      FILE_ATTRIBUTE_NORMAL, share_access, disposition,
      create_options | kFileSynchronousIoNonAlert | kFileOpenReparsePoint, nullptr, 0);
  if (!NtSucceeded(status)) {
    return {.error = api.DosError(status)};
  }
  return {
      .handle = UniqueHandle(raw_handle),
      .error = ERROR_SUCCESS,
  };
}

[[nodiscard]] PlatformError FlushDirectory(const NativeApi& api,
                                           const HANDLE directory) {
  IO_STATUS_BLOCK status_block{};
  const NTSTATUS status = api.flush_file()(directory, 0, nullptr, 0, &status_block);
  return NtSucceeded(status) ? PlatformError::kNone
                             : ErrorFromWindows(api.DosError(status));
}

struct DispositionInformationEx {
  ULONG flags;
};

[[nodiscard]] PlatformError MarkForDeletion(const HANDLE handle) {
  DispositionInformationEx disposition{.flags = kFileDispositionDelete |
                                                kFileDispositionPosixSemantics |
                                                kFileDispositionIgnoreReadonly};
  if (::SetFileInformationByHandle(handle, kFileDispositionInfoEx, &disposition,
                                   sizeof(disposition)) != FALSE) {
    return PlatformError::kNone;
  }
  const DWORD extended_error = ::GetLastError();
  if (!IsUnsupportedInformationError(extended_error)) {
    return ErrorFromWindows(extended_error);
  }

  FILE_DISPOSITION_INFO fallback{.DeleteFile = TRUE};
  if (::SetFileInformationByHandle(handle, FileDispositionInfo, &fallback,
                                   sizeof(fallback)) == FALSE) {
    return ErrorFromWindows(::GetLastError());
  }
  return PlatformError::kNone;
}

[[nodiscard]] bool StartsWith(const std::wstring_view value,
                              const std::wstring_view prefix) noexcept {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] bool IsSingleComponent(const std::wstring_view name) noexcept {
  return !name.empty() && name != L"." && name != L".." &&
         name.find_first_of(L"\\/:") == std::wstring_view::npos;
}

[[nodiscard]] PlatformResult CleanupStaleFiles(const NativeApi& api,
                                               const HANDLE temporary_directory) {
  std::vector<std::wstring> stale_names;
  alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, 64 * 1024> buffer{};
  bool restart = true;
  while (true) {
    const FILE_INFO_BY_HANDLE_CLASS information_class =
        restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;
    restart = false;
    if (::GetFileInformationByHandleEx(temporary_directory, information_class,
                                       buffer.data(),
                                       static_cast<DWORD>(buffer.size())) == FALSE) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_NO_MORE_FILES || error == ERROR_HANDLE_EOF) {
        break;
      }
      return {.error = ErrorFromWindows(error)};
    }

    std::size_t offset = 0;
    while (true) {
      if (offset + offsetof(FILE_ID_BOTH_DIR_INFO, FileName) > buffer.size()) {
        return {.error = PlatformError::kIoFailure};
      }
      const auto* const entry =
          reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
      const std::size_t name_bytes = entry->FileNameLength;
      if (name_bytes % sizeof(wchar_t) != 0 ||
          name_bytes >
              buffer.size() - offset - offsetof(FILE_ID_BOTH_DIR_INFO, FileName)) {
        return {.error = PlatformError::kIoFailure};
      }
      const std::wstring_view name(entry->FileName, name_bytes / sizeof(wchar_t));
      if (StartsWith(name, kTemporaryPrefix) && IsSingleComponent(name)) {
        if (stale_names.size() >= kMaxStaleTemporaries) {
          return {.error = PlatformError::kBusy};
        }
        stale_names.emplace_back(name);
      }
      if (entry->NextEntryOffset == 0) {
        break;
      }
      if (entry->NextEntryOffset >
          buffer.size() - offset - offsetof(FILE_ID_BOTH_DIR_INFO, FileName)) {
        return {.error = PlatformError::kIoFailure};
      }
      offset += entry->NextEntryOffset;
    }
  }

  PlatformError first_error = PlatformError::kNone;
  for (const std::wstring& name : stale_names) {
    RelativeOpenResult opened =
        OpenRelative(api, temporary_directory, name, kCleanupAccess, FILE_SHARE_READ,
                     kFileOpen, kFileOpenForBackupIntent, nullptr, false);
    if (!opened.handle.valid()) {
      if (!IsMissingError(opened.error) && first_error == PlatformError::kNone) {
        first_error = ErrorFromWindows(opened.error);
      }
      continue;
    }
    const PlatformError deletion_error = MarkForDeletion(opened.handle.get());
    if (deletion_error != PlatformError::kNone && first_error == PlatformError::kNone) {
      first_error = deletion_error;
    }
    if (!opened.handle.Reset() && first_error == PlatformError::kNone) {
      first_error = ErrorFromWindows(::GetLastError());
    }
  }
  const PlatformError flush_error = FlushDirectory(api, temporary_directory);
  if (first_error == PlatformError::kNone) {
    first_error = flush_error;
  }
  return {.error = first_error};
}

struct OpenTemporary {
  UniqueHandle handle{};
};

struct DestinationParent {
  std::vector<UniqueHandle> ancestors{};
  std::wstring filename{};
  PlatformError error{PlatformError::kNone};

  [[nodiscard]] HANDLE handle(const HANDLE root) const noexcept {
    return ancestors.empty() ? root : ancestors.back().get();
  }
};

[[nodiscard]] bool GenerateTemporaryName(std::wstring& name) {
  std::array<UCHAR, 16> random{};
  if (::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    return false;
  }
  constexpr std::wstring_view digits = L"0123456789abcdef";
  name.assign(kTemporaryPrefix);
  name.reserve(kTemporaryPrefix.size() + random.size() * 2);
  for (const UCHAR byte : random) {
    name.push_back(digits[(byte >> 4U) & 0x0fU]);
    name.push_back(digits[byte & 0x0fU]);
  }
  return true;
}

[[nodiscard]] PlatformError CheckFreeSpace(const HANDLE directory,
                                           const std::uint64_t required_bytes) {
  const DWORD required = ::GetFinalPathNameByHandleW(
      directory, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (required == 0) {
    return ErrorFromWindows(::GetLastError());
  }
  std::wstring path(static_cast<std::size_t>(required), L'\0');
  const DWORD written = ::GetFinalPathNameByHandleW(
      directory, path.data(), required, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (written == 0 || written >= required) {
    return ErrorFromWindows(::GetLastError());
  }
  path.resize(written);

  ULARGE_INTEGER available{};
  if (::GetDiskFreeSpaceExW(path.c_str(), &available, nullptr, nullptr) == FALSE) {
    return ErrorFromWindows(::GetLastError());
  }
  return required_bytes <= available.QuadPart ? PlatformError::kNone
                                              : PlatformError::kNoSpace;
}

class WindowsFilesystemBackend final : public PlatformBackend {
 public:
  WindowsFilesystemBackend(std::unique_ptr<NativeApi> api,
                           std::unique_ptr<PrivateSecurity> security, UniqueHandle root,
                           UniqueHandle temporary_directory, UniqueHandle lock) noexcept
      : api_(std::move(api)),
        security_(std::move(security)),
        root_(std::move(root)),
        temporary_directory_(std::move(temporary_directory)),
        lock_(std::move(lock)) {}

  ~WindowsFilesystemBackend() override {
    const std::scoped_lock lock(mutex_);
    for (auto& [handle, temporary] : temporaries_) {
      static_cast<void>(handle);
      static_cast<void>(MarkForDeletion(temporary.handle.get()));
      static_cast<void>(temporary.handle.Reset());
    }
    temporaries_.clear();
    static_cast<void>(FlushDirectory(*api_, temporary_directory_.get()));
  }

  [[nodiscard]] PlatformResult CreateTemporary(const ValidatedReceivePath& path,
                                               const std::uint64_t declared_size,
                                               TemporaryFileHandle& output) override {
    static_cast<void>(path);
    const std::scoped_lock lock(mutex_);
    output = {};

    const PlatformError space_error =
        CheckFreeSpace(temporary_directory_.get(), declared_size);
    if (space_error != PlatformError::kNone) {
      return {.error = space_error};
    }

    for (std::size_t attempt = 0; attempt < 128; ++attempt) {
      std::wstring name;
      if (!GenerateTemporaryName(name)) {
        return {.error = PlatformError::kIoFailure};
      }
      RelativeOpenResult opened =
          OpenRelative(*api_, temporary_directory_.get(), name, kRegularFileAccess, 0,
                       kFileCreate, kFileNonDirectoryFile, security_->descriptor());
      if (!opened.handle.valid()) {
        if (opened.error == ERROR_FILE_EXISTS || opened.error == ERROR_ALREADY_EXISTS) {
          continue;
        }
        return {.error = ErrorFromWindows(opened.error)};
      }
      const PlatformError object_error =
          ValidateObject(opened.handle.get(), ExpectedObjectType::kRegular, true);
      if (object_error != PlatformError::kNone) {
        static_cast<void>(MarkForDeletion(opened.handle.get()));
        return {.error = object_error};
      }

      const std::uint64_t handle = NextHandle();
      temporaries_.emplace(handle, OpenTemporary{.handle = std::move(opened.handle)});
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
      const DWORD chunk = static_cast<DWORD>(std::min(
          remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
      DWORD written = 0;
      if (::WriteFile(iterator->second.handle.get(), data.data() + total, chunk,
                      &written, nullptr) == FALSE) {
        return {
            .error = ErrorFromWindows(::GetLastError()),
            .bytes_written = total,
        };
      }
      if (written == 0) {
        return {.error = PlatformError::kIoFailure, .bytes_written = total};
      }
      total += written;
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
    if (::FlushFileBuffers(iterator->second.handle.get()) == FALSE) {
      return {.error = ErrorFromWindows(::GetLastError())};
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
    if (parent.error != PlatformError::kNone) {
      return {.error = parent.error};
    }
    const DWORD rename_error = RenameNoReplace(
        iterator->second.handle.get(), parent.handle(root_.get()), parent.filename);
    if (rename_error != ERROR_SUCCESS) {
      return {.error = ErrorFromWindows(rename_error)};
    }

    PlatformError uncertain_error = FlushDirectory(*api_, parent.handle(root_.get()));
    const PlatformError temporary_flush =
        FlushDirectory(*api_, temporary_directory_.get());
    if (uncertain_error == PlatformError::kNone) {
      uncertain_error = temporary_flush;
    }
    if (!iterator->second.handle.Reset() && uncertain_error == PlatformError::kNone) {
      uncertain_error = ErrorFromWindows(::GetLastError());
    }
    temporaries_.erase(iterator);

    if (uncertain_error != PlatformError::kNone) {
      return {
          .disposition = PlatformCommitDisposition::kOutcomeUncertain,
          .error = uncertain_error,
      };
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

    PlatformError first_error = MarkForDeletion(iterator->second.handle.get());
    if (!iterator->second.handle.Reset() && first_error == PlatformError::kNone) {
      first_error = ErrorFromWindows(::GetLastError());
    }
    temporaries_.erase(iterator);
    const PlatformError flush_error = FlushDirectory(*api_, temporary_directory_.get());
    if (first_error == PlatformError::kNone) {
      first_error = flush_error;
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
      const ValidatedReceivePath& destination) {
    const std::span<const std::string> components = destination.components();
    if (components.empty()) {
      return {.error = PlatformError::kIoFailure};
    }

    DestinationParent result;
    result.ancestors.reserve(components.size() - 1);
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
      std::wstring component;
      if (!ConvertUtf8Strict(components[index], component) ||
          !IsSingleComponent(component)) {
        result.error = PlatformError::kInvalidRoot;
        return result;
      }
      const HANDLE current = result.handle(root_.get());
      RelativeOpenResult opened = OpenRelative(
          *api_, current, component, kDirectoryAccess, kDirectoryShareAccess,
          kFileOpenIf, kFileDirectoryFile | kFileOpenForBackupIntent,
          security_->descriptor());
      if (!opened.handle.valid()) {
        result.error =
            opened.error == ERROR_DIRECTORY || opened.error == ERROR_CANT_ACCESS_FILE
                ? PlatformError::kInvalidRoot
                : ErrorFromWindows(opened.error);
        return result;
      }
      const PlatformError object_error =
          ValidateObject(opened.handle.get(), ExpectedObjectType::kDirectory, false);
      if (object_error != PlatformError::kNone) {
        result.error = object_error;
        return result;
      }
      const PlatformError flush_error = FlushDirectory(*api_, current);
      if (flush_error != PlatformError::kNone) {
        result.error = flush_error;
        return result;
      }
      result.ancestors.push_back(std::move(opened.handle));
    }

    if (!ConvertUtf8Strict(components.back(), result.filename) ||
        !IsSingleComponent(result.filename)) {
      result.error = PlatformError::kInvalidRoot;
    }
    return result;
  }

  [[nodiscard]] DWORD RenameNoReplace(const HANDLE file, const HANDLE parent,
                                      const std::wstring_view filename) const {
    const std::size_t filename_bytes = filename.size() * sizeof(wchar_t);
    const std::size_t buffer_bytes = sizeof(FILE_RENAME_INFO) + filename_bytes;
    if (filename_bytes > std::numeric_limits<DWORD>::max() ||
        buffer_bytes > std::numeric_limits<DWORD>::max()) {
      return ERROR_INVALID_NAME;
    }

    std::vector<std::byte> buffer(buffer_bytes);
    auto* const information = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    information->Flags = 0;
    information->RootDirectory = parent;
    information->FileNameLength = static_cast<DWORD>(filename_bytes);
    std::memcpy(information->FileName, filename.data(), filename_bytes);
    if (::SetFileInformationByHandle(file, kFileRenameInfoEx, information,
                                     static_cast<DWORD>(buffer.size())) != FALSE) {
      return ERROR_SUCCESS;
    }

    const DWORD win32_error = ::GetLastError();
    // #region debug-point A:win32-rename
    std::fprintf(stderr,
                 "[DEBUG] FileRenameInfoEx failed: error=%lu filename_bytes=%llu "
                 "buffer_bytes=%llu struct_bytes=%llu\n",
                 static_cast<unsigned long>(win32_error),
                 static_cast<unsigned long long>(filename_bytes),
                 static_cast<unsigned long long>(buffer_bytes),
                 static_cast<unsigned long long>(sizeof(FILE_RENAME_INFO)));
    // #endregion
    if (!IsUnsupportedInformationError(win32_error)) {
      return win32_error;
    }
    IO_STATUS_BLOCK status_block{};
    const NTSTATUS status = api_->set_information()(file, &status_block, information,
                                                    static_cast<ULONG>(buffer.size()),
                                                    kNativeFileRenameInformationEx);
    const DWORD native_error = NtSucceeded(status) ? ERROR_SUCCESS
                                                    : api_->DosError(status);
    // #region debug-point B:native-rename
    if (!NtSucceeded(status)) {
      std::fprintf(stderr,
                   "[DEBUG] NtSetInformationFile failed: status=%ld dos_error=%lu "
                   "io_status=%lld\n",
                   static_cast<long>(status),
                   static_cast<unsigned long>(native_error),
                   static_cast<long long>(status_block.Status));
    }
    // #endregion
    return native_error;
  }

  mutable std::mutex mutex_{};
  std::unique_ptr<NativeApi> api_{};
  std::unique_ptr<PrivateSecurity> security_{};
  UniqueHandle root_{};
  UniqueHandle temporary_directory_{};
  UniqueHandle lock_{};
  std::unordered_map<std::uint64_t, OpenTemporary> temporaries_{};
  std::uint64_t next_handle_{1};
};

}  // namespace

FilesystemBackendOpenResult OpenWindowsFilesystemBackend(
    const std::string_view destination_root_utf8) {
  std::wstring root_path;
  if (!ConvertUtf8Strict(destination_root_utf8, root_path)) {
    return {.error = PlatformError::kInvalidRoot};
  }

  auto api = std::make_unique<NativeApi>();
  if (!api->Initialize()) {
    return {.error = PlatformError::kUnsupported};
  }
  auto security = std::make_unique<PrivateSecurity>();
  if (!security->Initialize()) {
    return {.error = PlatformError::kIoFailure};
  }

  UniqueHandle root(::CreateFileW(
      root_path.c_str(), kDirectoryAccess, kDirectoryShareAccess, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!root.valid()) {
    const DWORD error = ::GetLastError();
    return {
        .error = IsMissingError(error) || error == ERROR_DIRECTORY
                     ? PlatformError::kInvalidRoot
                     : ErrorFromWindows(error),
    };
  }
  const PlatformError root_error =
      ValidateObject(root.get(), ExpectedObjectType::kDirectory, false);
  if (root_error != PlatformError::kNone) {
    return {.error = root_error};
  }
  if (!IsLocalFixedNtfs(root.get())) {
    return {.error = PlatformError::kUnsupported};
  }

  RelativeOpenResult temporary = OpenRelative(
      *api, root.get(), kTemporaryDirectory, kDirectoryAccess, kDirectoryShareAccess,
      kFileOpenIf, kFileDirectoryFile | kFileOpenForBackupIntent,
      security->descriptor());
  if (!temporary.handle.valid()) {
    return {.error = ErrorFromWindows(temporary.error)};
  }
  PlatformError temporary_error =
      ValidateObject(temporary.handle.get(), ExpectedObjectType::kDirectory, false);
  if (temporary_error == PlatformError::kNone) {
    temporary_error =
        ValidatePrivateSecurity(temporary.handle.get(), security->owner_sid());
  }
  if (temporary_error != PlatformError::kNone) {
    return {.error = temporary_error};
  }
  const PlatformError root_flush_error = FlushDirectory(*api, root.get());
  if (root_flush_error != PlatformError::kNone) {
    return {.error = root_flush_error};
  }

  RelativeOpenResult lock =
      OpenRelative(*api, temporary.handle.get(), kLockFile, kRegularFileAccess, 0,
                   kFileOpenIf, kFileNonDirectoryFile, security->descriptor());
  if (!lock.handle.valid()) {
    return {
        .error = lock.error == ERROR_SHARING_VIOLATION ? PlatformError::kBusy
                                                       : ErrorFromWindows(lock.error),
    };
  }
  PlatformError lock_error =
      ValidateObject(lock.handle.get(), ExpectedObjectType::kRegular, true);
  if (lock_error == PlatformError::kNone) {
    lock_error = ValidatePrivateSecurity(lock.handle.get(), security->owner_sid());
  }
  if (lock_error != PlatformError::kNone) {
    return {.error = lock_error};
  }
  const PlatformError lock_parent_flush_error =
      FlushDirectory(*api, temporary.handle.get());
  if (lock_parent_flush_error != PlatformError::kNone) {
    return {.error = lock_parent_flush_error};
  }

  const PlatformResult cleanup = CleanupStaleFiles(*api, temporary.handle.get());
  if (!cleanup.ok()) {
    return {.error = cleanup.error};
  }
  return {.backend = std::make_unique<WindowsFilesystemBackend>(
              std::move(api), std::move(security), std::move(root),
              std::move(temporary.handle), std::move(lock.handle))};
}

}  // namespace xnn_transfer::core::storage
