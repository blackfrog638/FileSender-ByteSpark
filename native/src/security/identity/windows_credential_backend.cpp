#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <aclapi.h>
#include <sddl.h>
#include <wincred.h>
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform_protected_store_internal.hpp"
#include "xnn_transfer/core/security/identity/windows_credential_store.hpp"

namespace xnn_transfer::core::security::identity {
namespace {

using internal::DecodePlatformProtectedItemEnvelope;
using internal::EncodePlatformProtectedItemEnvelope;
using internal::MakePlatformProtectedStore;
using internal::PlatformProtectedItem;
using internal::PlatformProtectedStoreBackend;
using internal::PlatformStoreLockGuard;
using internal::PlatformStoreOperationLock;

constexpr wchar_t kTargetPrefix[] = L"XnnTransfer/identity/v1/";
constexpr wchar_t kCredentialUser[] = L"XnnTransfer";
constexpr wchar_t kMutexPrefix[] = L"Global\\XnnTransfer.Identity.v1.";
constexpr wchar_t kHexDigits[] = L"0123456789abcdef";

class HandleGuard final {
 public:
  explicit HandleGuard(HANDLE handle = nullptr) : handle_(handle) {}
  ~HandleGuard() {
    if (handle_ != nullptr) {
      CloseHandle(handle_);
    }
  }

  HandleGuard(const HandleGuard&) = delete;
  HandleGuard& operator=(const HandleGuard&) = delete;

  HandleGuard(HandleGuard&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  HandleGuard& operator=(HandleGuard&& other) noexcept {
    if (this != &other) {
      if (handle_ != nullptr) {
        CloseHandle(handle_);
      }
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] HANDLE release() noexcept { return std::exchange(handle_, nullptr); }

 private:
  HANDLE handle_;
};

template <typename T>
class LocalGuard final {
 public:
  explicit LocalGuard(T value = nullptr) : value_(value) {}
  ~LocalGuard() {
    if (value_ != nullptr) {
      LocalFree(value_);
    }
  }

  LocalGuard(const LocalGuard&) = delete;
  LocalGuard& operator=(const LocalGuard&) = delete;

  [[nodiscard]] T get() const noexcept { return value_; }

 private:
  T value_;
};

void ClearCredentialBlob(PCREDENTIALW credential) noexcept {
  if (credential != nullptr && credential->CredentialBlob != nullptr &&
      credential->CredentialBlobSize != 0) {
    SecureZeroMemory(credential->CredentialBlob, credential->CredentialBlobSize);
  }
}

class CredentialGuard final {
 public:
  explicit CredentialGuard(PCREDENTIALW credential) : credential_(credential) {}
  ~CredentialGuard() {
    if (credential_ != nullptr) {
      ClearCredentialBlob(credential_);
      CredFree(credential_);
    }
  }

  CredentialGuard(const CredentialGuard&) = delete;
  CredentialGuard& operator=(const CredentialGuard&) = delete;

  [[nodiscard]] PCREDENTIALW get() const noexcept { return credential_; }

 private:
  PCREDENTIALW credential_;
};

class CredentialArrayGuard final {
 public:
  CredentialArrayGuard(const DWORD count, PCREDENTIALW* credentials)
      : count_(count), credentials_(credentials) {}
  ~CredentialArrayGuard() {
    if (credentials_ != nullptr) {
      for (DWORD index = 0; index < count_; ++index) {
        ClearCredentialBlob(credentials_[index]);
      }
      CredFree(credentials_);
    }
  }

  CredentialArrayGuard(const CredentialArrayGuard&) = delete;
  CredentialArrayGuard& operator=(const CredentialArrayGuard&) = delete;

 private:
  DWORD count_;
  PCREDENTIALW* credentials_;
};

[[nodiscard]] ErrorCode MapWindowsError(const DWORD error) noexcept {
  switch (error) {
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
      return ErrorCode::kPermissionDenied;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
      return ErrorCode::kCapacityExceeded;
    default:
      return ErrorCode::kStorageUnavailable;
  }
}

[[nodiscard]] Result<std::wstring> EncodeTarget(const std::string_view item_id) {
  try {
    std::wstring target(kTargetPrefix);
    target.reserve(target.size() + item_id.size() * 2);
    for (const char value : item_id) {
      const auto byte = static_cast<unsigned char>(value);
      target.push_back(kHexDigits[byte >> 4U]);
      target.push_back(kHexDigits[byte & 0x0fU]);
    }
    return Result<std::wstring>::Success(std::move(target));
  } catch (const std::bad_alloc&) {
    return Result<std::wstring>::Failure(ErrorCode::kCapacityExceeded);
  }
}

[[nodiscard]] int DecodeHexDigit(const wchar_t value) noexcept {
  if (value >= L'0' && value <= L'9') {
    return static_cast<int>(value - L'0');
  }
  if (value >= L'a' && value <= L'f') {
    return static_cast<int>(value - L'a' + 10);
  }
  return -1;
}

[[nodiscard]] Result<ProtectedItemId> DecodeTarget(const wchar_t* target) {
  if (target == nullptr) {
    return Result<ProtectedItemId>::Failure(ErrorCode::kCorruptRecord);
  }
  const std::size_t prefix_length = std::size(kTargetPrefix) - 1;
  const std::size_t target_length = std::wcslen(target);
  if (target_length <= prefix_length ||
      std::wmemcmp(target, kTargetPrefix, prefix_length) != 0) {
    return Result<ProtectedItemId>::Failure(ErrorCode::kCorruptRecord);
  }
  const std::size_t encoded_length = target_length - prefix_length;
  if ((encoded_length % 2) != 0 || encoded_length > kMaxProtectedItemIdBytes * 2) {
    return Result<ProtectedItemId>::Failure(ErrorCode::kCorruptRecord);
  }

  try {
    ProtectedItemId item_id;
    item_id.reserve(encoded_length / 2);
    for (std::size_t index = 0; index < encoded_length; index += 2) {
      const int high = DecodeHexDigit(target[prefix_length + index]);
      const int low = DecodeHexDigit(target[prefix_length + index + 1]);
      if (high < 0 || low < 0) {
        return Result<ProtectedItemId>::Failure(ErrorCode::kCorruptRecord);
      }
      item_id.push_back(static_cast<char>(static_cast<unsigned>(high << 4) |
                                          static_cast<unsigned>(low)));
    }
    if (item_id.empty() || item_id.find('\0') != std::string::npos) {
      return Result<ProtectedItemId>::Failure(ErrorCode::kCorruptRecord);
    }
    return Result<ProtectedItemId>::Success(std::move(item_id));
  } catch (const std::bad_alloc&) {
    return Result<ProtectedItemId>::Failure(ErrorCode::kCapacityExceeded);
  }
}

[[nodiscard]] Result<PlatformProtectedItem> DecodeCredential(
    const CREDENTIALW& credential, const std::optional<std::string_view> requested_id) {
  if (credential.Type != CRED_TYPE_GENERIC ||
      credential.Persist != CRED_PERSIST_LOCAL_MACHINE ||
      credential.AttributeCount != 0 || credential.CredentialBlob == nullptr ||
      credential.CredentialBlobSize == 0 || credential.UserName == nullptr ||
      std::wcscmp(credential.UserName, kCredentialUser) != 0) {
    return Result<PlatformProtectedItem>::Failure(ErrorCode::kCorruptRecord);
  }

  auto item_id_result = DecodeTarget(credential.TargetName);
  if (!item_id_result.ok()) {
    return Result<PlatformProtectedItem>::Failure(item_id_result.error());
  }
  ProtectedItemId item_id = std::move(item_id_result).value();
  if (requested_id.has_value() && item_id != *requested_id) {
    return Result<PlatformProtectedItem>::Failure(ErrorCode::kCorruptRecord);
  }

  return DecodePlatformProtectedItemEnvelope(
      std::move(item_id),
      std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(credential.CredentialBlob),
          credential.CredentialBlobSize));
}

class WindowsCredentialBackend final : public PlatformProtectedStoreBackend {
 public:
  Result<std::vector<PlatformProtectedItem>> Load(
      const std::optional<std::string_view> item_id) override {
    if (item_id.has_value()) {
      return LoadOne(*item_id);
    }

    DWORD count = 0;
    PCREDENTIALW* raw_credentials = nullptr;
    if (!CredEnumerateW(L"XnnTransfer/identity/v1/*", 0, &count, &raw_credentials)) {
      const DWORD error = GetLastError();
      if (error == ERROR_NOT_FOUND) {
        return Result<std::vector<PlatformProtectedItem>>::Success({});
      }
      return Result<std::vector<PlatformProtectedItem>>::Failure(
          MapWindowsError(error));
    }
    CredentialArrayGuard credentials(count, raw_credentials);
    if (raw_credentials == nullptr ||
        count > static_cast<DWORD>((kMaxPeerRecords + 1) * 2)) {
      return Result<std::vector<PlatformProtectedItem>>::Failure(
          ErrorCode::kCapacityExceeded);
    }

    try {
      std::vector<PlatformProtectedItem> items;
      items.reserve(count);
      for (DWORD index = 0; index < count; ++index) {
        if (raw_credentials[index] == nullptr) {
          return Result<std::vector<PlatformProtectedItem>>::Failure(
              ErrorCode::kCorruptRecord);
        }
        auto decoded = DecodeCredential(*raw_credentials[index], std::nullopt);
        if (!decoded.ok()) {
          return Result<std::vector<PlatformProtectedItem>>::Failure(decoded.error());
        }
        items.push_back(std::move(decoded).value());
      }
      return Result<std::vector<PlatformProtectedItem>>::Success(std::move(items));
    } catch (const std::bad_alloc&) {
      return Result<std::vector<PlatformProtectedItem>>::Failure(
          ErrorCode::kCapacityExceeded);
    }
  }

  Result<void> Put(const PlatformProtectedItem& item) override {
    auto target_result = EncodeTarget(item.item_id);
    if (!target_result.ok()) {
      return Result<void>::Failure(target_result.error());
    }
    auto envelope_result = EncodePlatformProtectedItemEnvelope(item);
    if (!envelope_result.ok()) {
      return Result<void>::Failure(envelope_result.error());
    }
    std::wstring target = std::move(target_result).value();
    SecretBuffer envelope = std::move(envelope_result).value();
    if (envelope.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
      return Result<void>::Failure(ErrorCode::kCapacityExceeded);
    }

    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target.data();
    credential.CredentialBlobSize = static_cast<DWORD>(envelope.size());
    credential.CredentialBlob = envelope.mutable_bytes().data();
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<wchar_t*>(kCredentialUser);
    if (!CredWriteW(&credential, 0)) {
      return Result<void>::Failure(MapWindowsError(GetLastError()));
    }

    auto verification = LoadOne(item.item_id);
    if (!verification.ok()) {
      return Result<void>::Failure(verification.error());
    }
    const std::vector<PlatformProtectedItem>& items = verification.value();
    if (items.size() != 1 || items.front().revision != item.revision ||
        items.front().payload.size() != item.payload.size()) {
      return Result<void>::Failure(ErrorCode::kCorruptRecord);
    }
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < item.payload.size(); ++index) {
      difference = static_cast<std::uint8_t>(
          difference | static_cast<std::uint8_t>(items.front().payload.bytes()[index] ^
                                                 item.payload.bytes()[index]));
    }
    if (difference != 0) {
      return Result<void>::Failure(ErrorCode::kCorruptRecord);
    }
    return Result<void>::Success();
  }

  Result<void> Delete(const std::string_view item_id) override {
    auto target_result = EncodeTarget(item_id);
    if (!target_result.ok()) {
      return Result<void>::Failure(target_result.error());
    }
    std::wstring target = std::move(target_result).value();
    if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
      const DWORD error = GetLastError();
      return Result<void>::Failure(error == ERROR_NOT_FOUND
                                       ? ErrorCode::kRevisionConflict
                                       : MapWindowsError(error));
    }
    auto verification = LoadOne(item_id);
    if (!verification.ok()) {
      return Result<void>::Failure(verification.error());
    }
    if (!verification.value().empty()) {
      return Result<void>::Failure(ErrorCode::kStorageUnavailable);
    }
    return Result<void>::Success();
  }

 private:
  [[nodiscard]] static Result<std::vector<PlatformProtectedItem>> LoadOne(
      const std::string_view item_id) {
    auto target_result = EncodeTarget(item_id);
    if (!target_result.ok()) {
      return Result<std::vector<PlatformProtectedItem>>::Failure(target_result.error());
    }
    std::wstring target = std::move(target_result).value();
    PCREDENTIALW raw_credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &raw_credential)) {
      const DWORD error = GetLastError();
      if (error == ERROR_NOT_FOUND) {
        return Result<std::vector<PlatformProtectedItem>>::Success({});
      }
      return Result<std::vector<PlatformProtectedItem>>::Failure(
          MapWindowsError(error));
    }
    CredentialGuard credential(raw_credential);
    if (raw_credential == nullptr) {
      return Result<std::vector<PlatformProtectedItem>>::Failure(
          ErrorCode::kStorageUnavailable);
    }
    auto decoded = DecodeCredential(*raw_credential, item_id);
    if (!decoded.ok()) {
      return Result<std::vector<PlatformProtectedItem>>::Failure(decoded.error());
    }
    try {
      std::vector<PlatformProtectedItem> items;
      items.push_back(std::move(decoded).value());
      return Result<std::vector<PlatformProtectedItem>>::Success(std::move(items));
    } catch (const std::bad_alloc&) {
      return Result<std::vector<PlatformProtectedItem>>::Failure(
          ErrorCode::kCapacityExceeded);
    }
  }
};

[[nodiscard]] Result<std::vector<std::byte>> ReadCurrentTokenUser() {
  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
    return Result<std::vector<std::byte>>::Failure(MapWindowsError(GetLastError()));
  }
  HandleGuard token(raw_token);

  DWORD required = 0;
  if (GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required) ||
      GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
    return Result<std::vector<std::byte>>::Failure(ErrorCode::kStorageUnavailable);
  }
  try {
    std::vector<std::byte> buffer(required);
    if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), required,
                             &required)) {
      return Result<std::vector<std::byte>>::Failure(MapWindowsError(GetLastError()));
    }
    return Result<std::vector<std::byte>>::Success(std::move(buffer));
  } catch (const std::bad_alloc&) {
    return Result<std::vector<std::byte>>::Failure(ErrorCode::kCapacityExceeded);
  }
}

class WindowsMutexGuard final : public PlatformStoreLockGuard {
 public:
  explicit WindowsMutexGuard(HANDLE mutex) : mutex_(mutex) {}
  ~WindowsMutexGuard() override {
    if (mutex_ != nullptr) {
      ReleaseMutex(mutex_);
      CloseHandle(mutex_);
    }
  }

  WindowsMutexGuard(const WindowsMutexGuard&) = delete;
  WindowsMutexGuard& operator=(const WindowsMutexGuard&) = delete;

 private:
  HANDLE mutex_;
};

class WindowsCredentialMutexLock final : public PlatformStoreOperationLock {
 public:
  Result<std::unique_ptr<PlatformStoreLockGuard>> Acquire() override {
    auto token_result = ReadCurrentTokenUser();
    if (!token_result.ok()) {
      return Result<std::unique_ptr<PlatformStoreLockGuard>>::Failure(
          token_result.error());
    }
    std::vector<std::byte> token = std::move(token_result).value();
    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(token.data());
    if (!IsValidSid(token_user->User.Sid)) {
      return Result<std::unique_ptr<PlatformStoreLockGuard>>::Failure(
          ErrorCode::kStorageUnavailable);
    }

    LPWSTR raw_sid = nullptr;
    if (!ConvertSidToStringSidW(token_user->User.Sid, &raw_sid)) {
      return Result<std::unique_ptr<PlatformStoreLockGuard>>::Failure(
          MapWindowsError(GetLastError()));
    }
    LocalGuard<LPWSTR> sid(raw_sid);

    try {
      const std::wstring security_text = L"O:" + std::wstring(raw_sid) +
                                         L"D:P(A;;GA;;;" + std::wstring(raw_sid) + L")";
      PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
              security_text.c_str(), SDDL_REVISION_1, &raw_descriptor, nullptr)) {
        return Result<std::unique_ptr<PlatformStoreLockGuard>>::Failure(
            MapWindowsError(GetLastError()));
      }
      LocalGuard<PSECURITY_DESCRIPTOR> descriptor(raw_descriptor);
      SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), raw_descriptor,
                                     FALSE};
      const std::wstring mutex_name = kMutexPrefix + std::wstring(raw_sid);
      HandleGuard mutex(CreateMutexW(&attributes, FALSE, mutex_name.c_str()));
      if (mutex.get() == nullptr) {
        return Result<std::unique_ptr<PlatformStoreLockGuard>>::Failure(
            MapWindowsError(GetLastError()));
      }

      PSID owner = nullptr;
      PSECURITY_DESCRIPTOR raw_current_descriptor = nullptr;
      const DWORD security_error =
          GetSecurityInfo(mutex.get(), SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION,
                          &owner, nullptr, nullptr, nullptr, &raw_current_descriptor);
      LocalGuard<PSECURITY_DESCRIPTOR> current_descriptor(raw_current_descriptor);
      if (security_error != ERROR_SUCCESS || owner == nullptr ||
          !EqualSid(owner, token_user->User.Sid)) {
        return Result<std::unique_ptr<PlatformStoreLockGuard>>::Failure(
            security_error == ERROR_SUCCESS ? ErrorCode::kPermissionDenied
                                            : MapWindowsError(security_error));
      }

      const DWORD wait_result = WaitForSingleObject(mutex.get(), INFINITE);
      if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
        return Result<std::unique_ptr<PlatformStoreLockGuard>>::Failure(
            ErrorCode::kStorageUnavailable);
      }
      return Result<std::unique_ptr<PlatformStoreLockGuard>>::Success(
          std::make_unique<WindowsMutexGuard>(mutex.release()));
    } catch (const std::bad_alloc&) {
      return Result<std::unique_ptr<PlatformStoreLockGuard>>::Failure(
          ErrorCode::kCapacityExceeded);
    }
  }
};

}  // namespace

Result<std::unique_ptr<ProtectedStore>> CreateWindowsCredentialProtectedStore() {
  try {
    std::unique_ptr<ProtectedStore> store =
        MakePlatformProtectedStore(std::make_unique<WindowsCredentialBackend>(),
                                   std::make_unique<WindowsCredentialMutexLock>());
    if (store == nullptr) {
      return Result<std::unique_ptr<ProtectedStore>>::Failure(
          ErrorCode::kStorageUnavailable);
    }
    return Result<std::unique_ptr<ProtectedStore>>::Success(std::move(store));
  } catch (const std::bad_alloc&) {
    return Result<std::unique_ptr<ProtectedStore>>::Failure(
        ErrorCode::kCapacityExceeded);
  }
}

}  // namespace xnn_transfer::core::security::identity
