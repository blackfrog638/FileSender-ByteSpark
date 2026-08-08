#include <CoreFoundation/CoreFoundation.h>
#include <Security/SecItem.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform_protected_store_internal.hpp"
#include "xnn_transfer/core/security/identity/macos_keychain_store.hpp"

namespace xnn_transfer::core::security::identity {
namespace {

using internal::DecodePlatformProtectedItemEnvelope;
using internal::EncodePlatformProtectedItemEnvelope;
using internal::MakePlatformProtectedStore;
using internal::PlatformProtectedItem;
using internal::PlatformProtectedStoreBackend;

constexpr char kHexDigits[] = "0123456789abcdef";

template <typename T>
class ScopedCF final {
 public:
  explicit ScopedCF(T value = nullptr) : value_(value) {}
  ~ScopedCF() {
    if (value_ != nullptr) {
      CFRelease(value_);
    }
  }

  ScopedCF(const ScopedCF&) = delete;
  ScopedCF& operator=(const ScopedCF&) = delete;

  ScopedCF(ScopedCF&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
  ScopedCF& operator=(ScopedCF&& other) noexcept {
    if (this != &other) {
      if (value_ != nullptr) {
        CFRelease(value_);
      }
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] T get() const noexcept { return value_; }

 private:
  T value_;
};

[[nodiscard]] ErrorCode MapKeychainError(const OSStatus status) noexcept {
  switch (status) {
    case errSecInteractionNotAllowed:
      return ErrorCode::kStorageLocked;
    case errSecAuthFailed:
    case errSecUserCanceled:
    case errSecMissingEntitlement:
      return ErrorCode::kPermissionDenied;
    case errSecDecode:
      return ErrorCode::kCorruptRecord;
    default:
      return ErrorCode::kStorageUnavailable;
  }
}

[[nodiscard]] ScopedCF<CFStringRef> MakeString(const std::string_view value) {
  if (value.empty()) {
    return ScopedCF<CFStringRef>();
  }
  return ScopedCF<CFStringRef>(CFStringCreateWithBytes(
      kCFAllocatorDefault, reinterpret_cast<const UInt8*>(value.data()),
      static_cast<CFIndex>(value.size()), kCFStringEncodingUTF8, false));
}

[[nodiscard]] Result<std::string> EncodeAccount(const std::string_view item_id) {
  try {
    std::string account;
    account.reserve(item_id.size() * 2);
    for (const char value : item_id) {
      const auto byte = static_cast<unsigned char>(value);
      account.push_back(kHexDigits[byte >> 4U]);
      account.push_back(kHexDigits[byte & 0x0fU]);
    }
    return Result<std::string>::Success(std::move(account));
  } catch (const std::bad_alloc&) {
    return Result<std::string>::Failure(ErrorCode::kCapacityExceeded);
  }
}

[[nodiscard]] int DecodeHexDigit(const char value) noexcept {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  return -1;
}

[[nodiscard]] Result<ProtectedItemId> DecodeAccount(const CFStringRef account) {
  if (account == nullptr || CFGetTypeID(account) != CFStringGetTypeID()) {
    return Result<ProtectedItemId>::Failure(ErrorCode::kCorruptRecord);
  }
  const CFIndex length = CFStringGetLength(account);
  if (length <= 0 || (length % 2) != 0 ||
      length > static_cast<CFIndex>(kMaxProtectedItemIdBytes * 2)) {
    return Result<ProtectedItemId>::Failure(ErrorCode::kCorruptRecord);
  }

  std::array<char, kMaxProtectedItemIdBytes * 2 + 1> encoded{};
  if (!CFStringGetCString(account, encoded.data(), static_cast<CFIndex>(encoded.size()),
                          kCFStringEncodingASCII)) {
    return Result<ProtectedItemId>::Failure(ErrorCode::kCorruptRecord);
  }

  try {
    ProtectedItemId item_id;
    item_id.reserve(static_cast<std::size_t>(length / 2));
    for (CFIndex index = 0; index < length; index += 2) {
      const int high = DecodeHexDigit(encoded[static_cast<std::size_t>(index)]);
      const int low = DecodeHexDigit(encoded[static_cast<std::size_t>(index + 1)]);
      if (high < 0 || low < 0) {
        return Result<ProtectedItemId>::Failure(ErrorCode::kCorruptRecord);
      }
      item_id.push_back(static_cast<char>(static_cast<unsigned>(high << 4) |
                                          static_cast<unsigned>(low)));
    }
    if (item_id.find('\0') != std::string::npos) {
      return Result<ProtectedItemId>::Failure(ErrorCode::kCorruptRecord);
    }
    return Result<ProtectedItemId>::Success(std::move(item_id));
  } catch (const std::bad_alloc&) {
    return Result<ProtectedItemId>::Failure(ErrorCode::kCapacityExceeded);
  }
}

[[nodiscard]] CFStringRef AuthenticationUiFailValue() noexcept {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  // macOS 10.14 requires this compatibility value to forbid authentication UI.
  return kSecUseAuthenticationUIFail;
#pragma clang diagnostic pop
}

[[nodiscard]] ScopedCF<CFMutableDictionaryRef> MakeBaseQuery(
    const std::optional<std::string_view> item_id) {
  ScopedCF<CFMutableDictionaryRef> query(
      CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                &kCFTypeDictionaryValueCallBacks));
  if (query.get() == nullptr) {
    return query;
  }

  CFDictionarySetValue(query.get(), kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query.get(), kSecAttrService,
                       CFSTR("com.xnntransfer.identity.v1"));
  CFDictionarySetValue(query.get(), kSecAttrSynchronizable, kCFBooleanFalse);
  CFDictionarySetValue(query.get(), kSecUseAuthenticationUI,
                       AuthenticationUiFailValue());
  if (item_id.has_value()) {
    auto account_result = EncodeAccount(*item_id);
    if (!account_result.ok()) {
      return ScopedCF<CFMutableDictionaryRef>();
    }
    ScopedCF<CFStringRef> account = MakeString(account_result.value());
    if (account.get() == nullptr) {
      return ScopedCF<CFMutableDictionaryRef>();
    }
    CFDictionarySetValue(query.get(), kSecAttrAccount, account.get());
  }
  return query;
}

[[nodiscard]] bool IsEqualString(CFTypeRef value, CFStringRef expected) noexcept {
  return value != nullptr && CFGetTypeID(value) == CFStringGetTypeID() &&
         CFEqual(value, expected);
}

[[nodiscard]] Result<PlatformProtectedItem> DecodeKeychainItem(
    const CFDictionaryRef attributes,
    const std::optional<std::string_view> requested_id) {
  if (attributes == nullptr || CFGetTypeID(attributes) != CFDictionaryGetTypeID()) {
    return Result<PlatformProtectedItem>::Failure(ErrorCode::kCorruptRecord);
  }

  const auto account =
      static_cast<CFStringRef>(CFDictionaryGetValue(attributes, kSecAttrAccount));
  auto item_id_result = DecodeAccount(account);
  if (!item_id_result.ok()) {
    return Result<PlatformProtectedItem>::Failure(item_id_result.error());
  }
  ProtectedItemId item_id = std::move(item_id_result).value();
  if (requested_id.has_value() && item_id != *requested_id) {
    return Result<PlatformProtectedItem>::Failure(ErrorCode::kCorruptRecord);
  }

  if (!IsEqualString(CFDictionaryGetValue(attributes, kSecAttrService),
                     CFSTR("com.xnntransfer.identity.v1"))) {
    return Result<PlatformProtectedItem>::Failure(ErrorCode::kCorruptRecord);
  }

  const CFTypeRef accessible = CFDictionaryGetValue(attributes, kSecAttrAccessible);
  if (accessible != nullptr &&
      !IsEqualString(accessible, kSecAttrAccessibleWhenUnlockedThisDeviceOnly)) {
    return Result<PlatformProtectedItem>::Failure(ErrorCode::kCorruptRecord);
  }
  const CFTypeRef synchronizable =
      CFDictionaryGetValue(attributes, kSecAttrSynchronizable);
  if (synchronizable != nullptr && synchronizable != kCFBooleanFalse) {
    return Result<PlatformProtectedItem>::Failure(ErrorCode::kCorruptRecord);
  }

  const auto data =
      static_cast<CFDataRef>(CFDictionaryGetValue(attributes, kSecValueData));
  if (data == nullptr || CFGetTypeID(data) != CFDataGetTypeID()) {
    return Result<PlatformProtectedItem>::Failure(ErrorCode::kCorruptRecord);
  }
  const CFIndex length = CFDataGetLength(data);
  const UInt8* bytes = CFDataGetBytePtr(data);
  if (length < 0 || bytes == nullptr) {
    return Result<PlatformProtectedItem>::Failure(ErrorCode::kCorruptRecord);
  }
  return DecodePlatformProtectedItemEnvelope(
      std::move(item_id),
      std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes),
                                    static_cast<std::size_t>(length)));
}

class MacosKeychainBackend final : public PlatformProtectedStoreBackend {
 public:
  Result<std::vector<PlatformProtectedItem>> Load(
      const std::optional<std::string_view> item_id) override {
    ScopedCF<CFMutableDictionaryRef> query = MakeBaseQuery(item_id);
    if (query.get() == nullptr) {
      return Result<std::vector<PlatformProtectedItem>>::Failure(
          ErrorCode::kCapacityExceeded);
    }
    CFDictionarySetValue(query.get(), kSecReturnAttributes, kCFBooleanTrue);
    CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query.get(), kSecMatchLimit,
                         item_id.has_value() ? kSecMatchLimitOne : kSecMatchLimitAll);

    CFTypeRef raw_result = nullptr;
    const OSStatus status = SecItemCopyMatching(query.get(), &raw_result);
    if (status == errSecItemNotFound) {
      return Result<std::vector<PlatformProtectedItem>>::Success({});
    }
    if (status != errSecSuccess || raw_result == nullptr) {
      if (raw_result != nullptr) {
        CFRelease(raw_result);
      }
      return Result<std::vector<PlatformProtectedItem>>::Failure(
          MapKeychainError(status));
    }
    ScopedCF<CFTypeRef> result(raw_result);

    try {
      std::vector<PlatformProtectedItem> items;
      if (item_id.has_value()) {
        if (CFGetTypeID(raw_result) != CFDictionaryGetTypeID()) {
          return Result<std::vector<PlatformProtectedItem>>::Failure(
              ErrorCode::kCorruptRecord);
        }
        auto decoded =
            DecodeKeychainItem(static_cast<CFDictionaryRef>(raw_result), item_id);
        if (!decoded.ok()) {
          return Result<std::vector<PlatformProtectedItem>>::Failure(decoded.error());
        }
        items.push_back(std::move(decoded).value());
      } else {
        if (CFGetTypeID(raw_result) != CFArrayGetTypeID()) {
          return Result<std::vector<PlatformProtectedItem>>::Failure(
              ErrorCode::kCorruptRecord);
        }
        const auto array = static_cast<CFArrayRef>(raw_result);
        const CFIndex count = CFArrayGetCount(array);
        if (count < 0 || count > static_cast<CFIndex>((kMaxPeerRecords + 1) * 2)) {
          return Result<std::vector<PlatformProtectedItem>>::Failure(
              ErrorCode::kCapacityExceeded);
        }
        items.reserve(static_cast<std::size_t>(count));
        for (CFIndex index = 0; index < count; ++index) {
          const auto attributes =
              static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(array, index));
          auto decoded = DecodeKeychainItem(attributes, std::nullopt);
          if (!decoded.ok()) {
            return Result<std::vector<PlatformProtectedItem>>::Failure(decoded.error());
          }
          items.push_back(std::move(decoded).value());
        }
      }
      return Result<std::vector<PlatformProtectedItem>>::Success(std::move(items));
    } catch (const std::bad_alloc&) {
      return Result<std::vector<PlatformProtectedItem>>::Failure(
          ErrorCode::kCapacityExceeded);
    }
  }

  Result<void> Put(const PlatformProtectedItem& item) override {
    auto envelope_result = EncodePlatformProtectedItemEnvelope(item);
    if (!envelope_result.ok()) {
      return Result<void>::Failure(envelope_result.error());
    }
    SecretBuffer envelope = std::move(envelope_result).value();
    ScopedCF<CFDataRef> data(CFDataCreateWithBytesNoCopy(
        kCFAllocatorDefault, envelope.bytes().data(),
        static_cast<CFIndex>(envelope.size()), kCFAllocatorNull));
    if (data.get() == nullptr) {
      return Result<void>::Failure(ErrorCode::kCapacityExceeded);
    }

    ScopedCF<CFMutableDictionaryRef> query = MakeBaseQuery(item.item_id);
    if (query.get() == nullptr) {
      return Result<void>::Failure(ErrorCode::kCapacityExceeded);
    }
    ScopedCF<CFMutableDictionaryRef> update(CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks));
    if (update.get() == nullptr) {
      return Result<void>::Failure(ErrorCode::kCapacityExceeded);
    }
    CFDictionarySetValue(update.get(), kSecValueData, data.get());
    CFDictionarySetValue(update.get(), kSecAttrAccessible,
                         kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
    CFDictionarySetValue(update.get(), kSecAttrSynchronizable, kCFBooleanFalse);
    CFDictionarySetValue(update.get(), kSecAttrLabel,
                         CFSTR("XnnTransfer protected identity"));

    OSStatus status = SecItemUpdate(query.get(), update.get());
    if (status == errSecItemNotFound) {
      CFDictionarySetValue(query.get(), kSecValueData, data.get());
      CFDictionarySetValue(query.get(), kSecAttrAccessible,
                           kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
      CFDictionarySetValue(query.get(), kSecAttrLabel,
                           CFSTR("XnnTransfer protected identity"));
      status = SecItemAdd(query.get(), nullptr);
      if (status == errSecDuplicateItem) {
        CFDictionaryRemoveValue(query.get(), kSecValueData);
        CFDictionaryRemoveValue(query.get(), kSecAttrAccessible);
        CFDictionaryRemoveValue(query.get(), kSecAttrLabel);
        status = SecItemUpdate(query.get(), update.get());
      }
    }
    if (status != errSecSuccess) {
      return Result<void>::Failure(MapKeychainError(status));
    }

    auto verification = Load(item.item_id);
    if (!verification.ok()) {
      return Result<void>::Failure(verification.error());
    }
    const std::vector<PlatformProtectedItem>& items = verification.value();
    if (items.size() != 1 || items.front().revision != item.revision ||
        items.front().payload.bytes().size() != item.payload.size()) {
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
    ScopedCF<CFMutableDictionaryRef> query = MakeBaseQuery(item_id);
    if (query.get() == nullptr) {
      return Result<void>::Failure(ErrorCode::kCapacityExceeded);
    }
    const OSStatus status = SecItemDelete(query.get());
    if (status == errSecItemNotFound) {
      return Result<void>::Failure(ErrorCode::kRevisionConflict);
    }
    if (status != errSecSuccess) {
      return Result<void>::Failure(MapKeychainError(status));
    }
    auto verification = Load(item_id);
    if (!verification.ok()) {
      return Result<void>::Failure(verification.error());
    }
    if (!verification.value().empty()) {
      return Result<void>::Failure(ErrorCode::kStorageUnavailable);
    }
    return Result<void>::Success();
  }
};

[[nodiscard]] Result<std::string> ReadUserTemporaryDirectory() {
  const std::size_t required = confstr(_CS_DARWIN_USER_TEMP_DIR, nullptr, 0);
  if (required <= 1) {
    return Result<std::string>::Failure(ErrorCode::kStorageUnavailable);
  }
  try {
    std::string directory(required, '\0');
    const std::size_t written =
        confstr(_CS_DARWIN_USER_TEMP_DIR, directory.data(), directory.size());
    if (written != required || directory.back() != '\0') {
      return Result<std::string>::Failure(ErrorCode::kStorageUnavailable);
    }
    directory.pop_back();
    while (directory.size() > 1 && directory.back() == '/') {
      directory.pop_back();
    }
    return Result<std::string>::Success(std::move(directory));
  } catch (const std::bad_alloc&) {
    return Result<std::string>::Failure(ErrorCode::kCapacityExceeded);
  }
}

}  // namespace

Result<std::unique_ptr<ProtectedStore>> CreateMacosKeychainProtectedStore() {
  auto directory_result = ReadUserTemporaryDirectory();
  if (!directory_result.ok()) {
    return Result<std::unique_ptr<ProtectedStore>>::Failure(directory_result.error());
  }
  try {
    std::unique_ptr<ProtectedStore> store = MakePlatformProtectedStore(
        std::make_unique<MacosKeychainBackend>(),
        internal::MakePosixDirectoryLock(std::move(directory_result).value()));
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
