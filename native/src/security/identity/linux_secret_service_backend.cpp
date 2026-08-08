#include <fcntl.h>
#include <libsecret/secret.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "linux_secret_service_store_internal.hpp"
#include "xnn_transfer/core/security/identity/linux_secret_service_store.hpp"

namespace xnn_transfer::core::security::identity {
namespace {

using internal::LinuxSecretServiceBackend;
using internal::LinuxSecretServiceItem;
using internal::LinuxStoreLockGuard;
using internal::LinuxStoreOperationLock;
using internal::MakeLinuxSecretServiceStore;

constexpr char kServiceName[] = "org.freedesktop.secrets";
constexpr char kServicePath[] = "/org/freedesktop/secrets";
constexpr char kServiceInterface[] = "org.freedesktop.Secret.Service";
constexpr char kCollectionInterface[] = "org.freedesktop.Secret.Collection";
constexpr char kItemInterface[] = "org.freedesktop.Secret.Item";
constexpr char kPropertiesInterface[] = "org.freedesktop.DBus.Properties";
constexpr char kBusServiceName[] = "org.freedesktop.DBus";
constexpr char kBusServicePath[] = "/org/freedesktop/DBus";
constexpr char kBusServiceInterface[] = "org.freedesktop.DBus";
constexpr char kSchemaName[] = "com.xnntransfer.identity.v1";
constexpr char kApplicationAttribute[] = "application";
constexpr char kApplicationValue[] = "xnn-transfer";
constexpr char kItemIdAttribute[] = "item-id";
constexpr char kSchemaAttribute[] = "xdg:schema";
constexpr char kItemLabel[] = "XnnTransfer protected identity";
constexpr char kItemLabelProperty[] = "org.freedesktop.Secret.Item.Label";
constexpr char kItemAttributesProperty[] = "org.freedesktop.Secret.Item.Attributes";
constexpr char kRuntimeLockName[] = ".xnn-transfer-identity.lock";
constexpr char kBinaryContentType[] = "application/octet-stream";
constexpr char kRequiredSessionAlgorithm[] = "dh-ietf1024-sha256-aes128-cbc-pkcs7";
constexpr std::array<std::uint8_t, 4> kEnvelopeMagic{'X', 'N', 'S', 'L'};
constexpr std::uint8_t kEnvelopeVersion = 1;
constexpr std::size_t kEnvelopeHeaderSize = 17;

struct GVariantDeleter {
  void operator()(GVariant* value) const noexcept {
    if (value != nullptr) {
      g_variant_unref(value);
    }
  }
};

struct SecretValueDeleter {
  void operator()(SecretValue* value) const noexcept {
    if (value != nullptr) {
      secret_value_unref(value);
    }
  }
};

struct SecretServiceDeleter {
  void operator()(SecretService* value) const noexcept {
    if (value != nullptr) {
      g_object_unref(value);
    }
  }
};

struct StringVectorDeleter {
  void operator()(gchar** value) const noexcept {
    if (value != nullptr) {
      g_strfreev(value);
    }
  }
};

using GVariantPtr = std::unique_ptr<GVariant, GVariantDeleter>;
using SecretValuePtr = std::unique_ptr<SecretValue, SecretValueDeleter>;
using SecretServicePtr = std::unique_ptr<SecretService, SecretServiceDeleter>;
using StringVectorPtr = std::unique_ptr<gchar*, StringVectorDeleter>;

[[nodiscard]] ErrorCode MapGError(const GError* error) noexcept {
  if (error == nullptr) {
    return ErrorCode::kStorageUnavailable;
  }
  if (g_error_matches(error, SECRET_ERROR, SECRET_ERROR_IS_LOCKED)) {
    return ErrorCode::kStorageLocked;
  }
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED) ||
      g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED) ||
      g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_AUTH_FAILED)) {
    return ErrorCode::kPermissionDenied;
  }
  return ErrorCode::kStorageUnavailable;
}

[[nodiscard]] ErrorCode TakeGError(GError* error) noexcept {
  const ErrorCode code = MapGError(error);
  if (error != nullptr) {
    g_error_free(error);
  }
  return code;
}

[[nodiscard]] ErrorCode MapErrno(const int error) noexcept {
  if (error == EACCES || error == EPERM || error == EROFS) {
    return ErrorCode::kPermissionDenied;
  }
  return ErrorCode::kStorageUnavailable;
}

[[nodiscard]] std::uint32_t ReadU32(
    const std::span<const std::uint8_t> bytes) noexcept {
  std::uint32_t value = 0;
  for (const std::uint8_t byte : bytes.first(4)) {
    value =
        static_cast<std::uint32_t>((value << 8U) | static_cast<std::uint32_t>(byte));
  }
  return value;
}

[[nodiscard]] std::uint64_t ReadU64(
    const std::span<const std::uint8_t> bytes) noexcept {
  std::uint64_t value = 0;
  for (const std::uint8_t byte : bytes.first(8)) {
    value = (value << 8U) | static_cast<std::uint64_t>(byte);
  }
  return value;
}

void WriteU32(const std::span<std::uint8_t> bytes, const std::uint32_t value) noexcept {
  bytes[0] = static_cast<std::uint8_t>(value >> 24U);
  bytes[1] = static_cast<std::uint8_t>(value >> 16U);
  bytes[2] = static_cast<std::uint8_t>(value >> 8U);
  bytes[3] = static_cast<std::uint8_t>(value);
}

void WriteU64(const std::span<std::uint8_t> bytes, const std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < 8; ++index) {
    const std::size_t shift = (7 - index) * 8;
    bytes[index] = static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift));
  }
}

[[nodiscard]] bool ConstantTimeEqual(
    const std::span<const std::uint8_t> left,
    const std::span<const std::uint8_t> right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  std::uint8_t difference = 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    difference = static_cast<std::uint8_t>(
        difference | static_cast<std::uint8_t>(left[index] ^ right[index]));
  }
  return difference == 0;
}

class RuntimeLockGuard final : public LinuxStoreLockGuard {
 public:
  explicit RuntimeLockGuard(const int descriptor) : descriptor_(descriptor) {}
  ~RuntimeLockGuard() override {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
  }

  RuntimeLockGuard(const RuntimeLockGuard&) = delete;
  RuntimeLockGuard& operator=(const RuntimeLockGuard&) = delete;

 private:
  int descriptor_;
};

class RuntimeDirectoryLock final : public LinuxStoreOperationLock {
 public:
  explicit RuntimeDirectoryLock(std::string runtime_directory)
      : runtime_directory_(std::move(runtime_directory)) {}

  Result<std::unique_ptr<LinuxStoreLockGuard>> Acquire() override {
    if (runtime_directory_.empty() || runtime_directory_.front() != '/' ||
        runtime_directory_.find('\0') != std::string::npos) {
      return Result<std::unique_ptr<LinuxStoreLockGuard>>::Failure(
          ErrorCode::kStorageUnavailable);
    }

    const int directory = open(runtime_directory_.c_str(),
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) {
      return Result<std::unique_ptr<LinuxStoreLockGuard>>::Failure(MapErrno(errno));
    }

    struct stat directory_status{};
    if (fstat(directory, &directory_status) != 0 ||
        !S_ISDIR(directory_status.st_mode) || directory_status.st_uid != geteuid() ||
        (directory_status.st_mode & 0077) != 0) {
      close(directory);
      return Result<std::unique_ptr<LinuxStoreLockGuard>>::Failure(
          ErrorCode::kPermissionDenied);
    }

    const int descriptor = openat(directory, kRuntimeLockName,
                                  O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    const int open_error = errno;
    close(directory);
    if (descriptor < 0) {
      return Result<std::unique_ptr<LinuxStoreLockGuard>>::Failure(
          MapErrno(open_error));
    }

    struct stat lock_status{};
    if (fstat(descriptor, &lock_status) != 0 || !S_ISREG(lock_status.st_mode) ||
        lock_status.st_uid != geteuid() || lock_status.st_nlink != 1 ||
        (lock_status.st_mode & 0777) != 0600) {
      close(descriptor);
      return Result<std::unique_ptr<LinuxStoreLockGuard>>::Failure(
          ErrorCode::kPermissionDenied);
    }

    int result = 0;
    do {
      result = flock(descriptor, LOCK_EX);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
      const ErrorCode error = MapErrno(errno);
      close(descriptor);
      return Result<std::unique_ptr<LinuxStoreLockGuard>>::Failure(error);
    }

    try {
      return Result<std::unique_ptr<LinuxStoreLockGuard>>::Success(
          std::make_unique<RuntimeLockGuard>(descriptor));
    } catch (const std::bad_alloc&) {
      close(descriptor);
      return Result<std::unique_ptr<LinuxStoreLockGuard>>::Failure(
          ErrorCode::kCapacityExceeded);
    }
  }

 private:
  std::string runtime_directory_;
};

class LibsecretBackend final : public LinuxSecretServiceBackend {
 public:
  LibsecretBackend(SecretServicePtr service, std::string owner,
                   std::string collection_path)
      : service_(std::move(service)),
        owner_(std::move(owner)),
        collection_path_(std::move(collection_path)),
        connection_(g_dbus_proxy_get_connection(G_DBUS_PROXY(service_.get()))) {}

  Result<std::vector<LinuxSecretServiceItem>> Load(
      const std::optional<std::string_view> item_id) override {
    try {
      Result<void> readiness = EnsureReady();
      if (!readiness.ok()) {
        return Result<std::vector<LinuxSecretServiceItem>>::Failure(readiness.error());
      }

      auto path_result = Search(item_id);
      if (!path_result.ok()) {
        return Result<std::vector<LinuxSecretServiceItem>>::Failure(
            path_result.error());
      }
      std::vector<std::string> paths = std::move(path_result).value();
      std::vector<LinuxSecretServiceItem> items;
      items.reserve(paths.size());
      for (const std::string& path : paths) {
        auto item_result = LoadItem(path, item_id);
        if (!item_result.ok()) {
          return Result<std::vector<LinuxSecretServiceItem>>::Failure(
              item_result.error());
        }
        items.push_back(std::move(item_result).value());
      }
      Result<void> owner_result = CheckOwner();
      if (!owner_result.ok()) {
        return Result<std::vector<LinuxSecretServiceItem>>::Failure(
            owner_result.error());
      }
      return Result<std::vector<LinuxSecretServiceItem>>::Success(std::move(items));
    } catch (const std::bad_alloc&) {
      return Result<std::vector<LinuxSecretServiceItem>>::Failure(
          ErrorCode::kCapacityExceeded);
    }
  }

  Result<void> Put(const LinuxSecretServiceItem& item) override {
    Result<void> readiness = EnsureReady();
    if (!readiness.ok()) {
      return readiness;
    }

    auto envelope_result = EncodeEnvelope(item);
    if (!envelope_result.ok()) {
      return Result<void>::Failure(envelope_result.error());
    }
    SecretBuffer envelope = std::move(envelope_result).value();
    SecretValuePtr value(
        secret_value_new(reinterpret_cast<const gchar*>(envelope.bytes().data()),
                         static_cast<gssize>(envelope.size()), kBinaryContentType));
    if (value == nullptr) {
      return Result<void>::Failure(ErrorCode::kStorageUnavailable);
    }
    GVariant* encoded_secret =
        secret_service_encode_dbus_secret(service_.get(), value.get());
    if (encoded_secret == nullptr) {
      return Result<void>::Failure(ErrorCode::kStorageUnavailable);
    }

    GVariantBuilder attributes;
    g_variant_builder_init(&attributes, G_VARIANT_TYPE("a{ss}"));
    g_variant_builder_add(&attributes, "{ss}", kSchemaAttribute, kSchemaName);
    g_variant_builder_add(&attributes, "{ss}", kApplicationAttribute,
                          kApplicationValue);
    g_variant_builder_add(&attributes, "{ss}", kItemIdAttribute, item.item_id.c_str());

    GVariantBuilder properties;
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&properties, "{sv}", kItemLabelProperty,
                          g_variant_new_string(kItemLabel));
    g_variant_builder_add(&properties, "{sv}", kItemAttributesProperty,
                          g_variant_builder_end(&attributes));

    GVariant* parameters = g_variant_new(
        "(@a{sv}@(oayays)b)", g_variant_builder_end(&properties), encoded_secret, TRUE);
    auto response_result = Call(collection_path_, kCollectionInterface, "CreateItem",
                                parameters, G_VARIANT_TYPE("(oo)"));
    if (!response_result.ok()) {
      return Result<void>::Failure(response_result.error());
    }

    const gchar* created_path = nullptr;
    const gchar* prompt_path = nullptr;
    g_variant_get(response_result.value().get(), "(&o&o)", &created_path, &prompt_path);
    if (prompt_path == nullptr || std::strcmp(prompt_path, "/") != 0) {
      return Result<void>::Failure(ErrorCode::kStorageLocked);
    }
    if (created_path == nullptr || std::strcmp(created_path, "/") == 0) {
      return Result<void>::Failure(ErrorCode::kStorageUnavailable);
    }

    auto verification_result = Load(item.item_id);
    if (!verification_result.ok()) {
      return Result<void>::Failure(verification_result.error());
    }
    std::vector<LinuxSecretServiceItem> verified =
        std::move(verification_result).value();
    if (verified.size() != 1 || verified.front().revision != item.revision ||
        !ConstantTimeEqual(verified.front().payload.bytes(), item.payload.bytes())) {
      return Result<void>::Failure(ErrorCode::kCorruptRecord);
    }
    return Result<void>::Success();
  }

  Result<void> Delete(const std::string_view item_id) override {
    Result<void> readiness = EnsureReady();
    if (!readiness.ok()) {
      return readiness;
    }
    auto path_result = Search(item_id);
    if (!path_result.ok()) {
      return Result<void>::Failure(path_result.error());
    }
    std::vector<std::string> paths = std::move(path_result).value();
    if (paths.size() != 1) {
      return Result<void>::Failure(paths.empty() ? ErrorCode::kRevisionConflict
                                                 : ErrorCode::kCorruptRecord);
    }

    auto response_result = Call(paths.front(), kItemInterface, "Delete",
                                g_variant_new("()"), G_VARIANT_TYPE("(o)"));
    if (!response_result.ok()) {
      return Result<void>::Failure(response_result.error());
    }
    const gchar* prompt_path = nullptr;
    g_variant_get(response_result.value().get(), "(&o)", &prompt_path);
    if (prompt_path == nullptr || std::strcmp(prompt_path, "/") != 0) {
      return Result<void>::Failure(ErrorCode::kStorageLocked);
    }

    auto verification_result = Search(item_id);
    if (!verification_result.ok()) {
      return Result<void>::Failure(verification_result.error());
    }
    if (!verification_result.value().empty()) {
      return Result<void>::Failure(ErrorCode::kStorageUnavailable);
    }
    return Result<void>::Success();
  }

  [[nodiscard]] Result<void> Initialize() { return EnsureReady(); }

 private:
  [[nodiscard]] Result<GVariantPtr> Call(const std::string_view object_path,
                                         const char* interface_name,
                                         const char* method_name, GVariant* parameters,
                                         const GVariantType* reply_type) const {
    try {
      const std::string path(object_path);
      GError* error = nullptr;
      GVariant* response = g_dbus_connection_call_sync(
          connection_, owner_.c_str(), path.c_str(), interface_name, method_name,
          parameters, reply_type, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, nullptr, &error);
      if (response == nullptr) {
        return Result<GVariantPtr>::Failure(TakeGError(error));
      }
      return Result<GVariantPtr>::Success(GVariantPtr(response));
    } catch (const std::bad_alloc&) {
      return Result<GVariantPtr>::Failure(ErrorCode::kCapacityExceeded);
    }
  }

  [[nodiscard]] Result<void> CheckOwner() const {
    GError* error = nullptr;
    GVariant* response = g_dbus_connection_call_sync(
        connection_, kBusServiceName, kBusServicePath, kBusServiceInterface,
        "GetNameOwner", g_variant_new("(s)", kServiceName), G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, nullptr, &error);
    if (response == nullptr) {
      return Result<void>::Failure(TakeGError(error));
    }
    GVariantPtr owned_response(response);
    const gchar* owner = nullptr;
    g_variant_get(response, "(&s)", &owner);
    if (owner == nullptr || owner_ != owner) {
      return Result<void>::Failure(ErrorCode::kStorageUnavailable);
    }
    return Result<void>::Success();
  }

  [[nodiscard]] Result<std::string> ReadDefaultCollection() const {
    auto response_result =
        Call(kServicePath, kServiceInterface, "ReadAlias",
             g_variant_new("(s)", SECRET_COLLECTION_DEFAULT), G_VARIANT_TYPE("(o)"));
    if (!response_result.ok()) {
      return Result<std::string>::Failure(response_result.error());
    }
    const gchar* path = nullptr;
    g_variant_get(response_result.value().get(), "(&o)", &path);
    if (path == nullptr || std::strcmp(path, "/") == 0) {
      return Result<std::string>::Failure(ErrorCode::kStorageUnavailable);
    }
    try {
      return Result<std::string>::Success(std::string(path));
    } catch (const std::bad_alloc&) {
      return Result<std::string>::Failure(ErrorCode::kCapacityExceeded);
    }
  }

  [[nodiscard]] Result<GVariantPtr> GetProperty(const std::string_view object_path,
                                                const char* interface_name,
                                                const char* property_name) const {
    auto response_result = Call(object_path, kPropertiesInterface, "Get",
                                g_variant_new("(ss)", interface_name, property_name),
                                G_VARIANT_TYPE("(v)"));
    if (!response_result.ok()) {
      return Result<GVariantPtr>::Failure(response_result.error());
    }
    GVariant* boxed = g_variant_get_child_value(response_result.value().get(), 0);
    if (boxed == nullptr || !g_variant_is_of_type(boxed, G_VARIANT_TYPE_VARIANT)) {
      if (boxed != nullptr) {
        g_variant_unref(boxed);
      }
      return Result<GVariantPtr>::Failure(ErrorCode::kStorageUnavailable);
    }
    GVariantPtr owned_boxed(boxed);
    GVariant* value = g_variant_get_variant(boxed);
    if (value == nullptr) {
      return Result<GVariantPtr>::Failure(ErrorCode::kStorageUnavailable);
    }
    return Result<GVariantPtr>::Success(GVariantPtr(value));
  }

  [[nodiscard]] Result<bool> IsLocked(const std::string_view object_path,
                                      const char* interface_name) const {
    auto property_result = GetProperty(object_path, interface_name, "Locked");
    if (!property_result.ok()) {
      return Result<bool>::Failure(property_result.error());
    }
    if (!g_variant_is_of_type(property_result.value().get(), G_VARIANT_TYPE_BOOLEAN)) {
      return Result<bool>::Failure(ErrorCode::kStorageUnavailable);
    }
    return Result<bool>::Success(g_variant_get_boolean(property_result.value().get()) !=
                                 FALSE);
  }

  [[nodiscard]] Result<void> EnsureReady() const {
    Result<void> owner_result = CheckOwner();
    if (!owner_result.ok()) {
      return owner_result;
    }
    auto collection_result = ReadDefaultCollection();
    if (!collection_result.ok()) {
      return Result<void>::Failure(collection_result.error());
    }
    if (collection_result.value() != collection_path_) {
      return Result<void>::Failure(ErrorCode::kStorageUnavailable);
    }
    auto locked_result = IsLocked(collection_path_, kCollectionInterface);
    if (!locked_result.ok()) {
      return Result<void>::Failure(locked_result.error());
    }
    if (locked_result.value()) {
      return Result<void>::Failure(ErrorCode::kStorageLocked);
    }
    return Result<void>::Success();
  }

  [[nodiscard]] Result<std::vector<std::string>> Search(
      const std::optional<std::string_view> item_id) const {
    std::string owned_item_id;
    try {
      if (item_id.has_value()) {
        owned_item_id.assign(*item_id);
      }
    } catch (const std::bad_alloc&) {
      return Result<std::vector<std::string>>::Failure(ErrorCode::kCapacityExceeded);
    }

    GVariantBuilder attributes;
    g_variant_builder_init(&attributes, G_VARIANT_TYPE("a{ss}"));
    g_variant_builder_add(&attributes, "{ss}", kSchemaAttribute, kSchemaName);
    g_variant_builder_add(&attributes, "{ss}", kApplicationAttribute,
                          kApplicationValue);
    if (item_id.has_value()) {
      g_variant_builder_add(&attributes, "{ss}", kItemIdAttribute,
                            owned_item_id.c_str());
    }
    auto response_result =
        Call(collection_path_, kCollectionInterface, "SearchItems",
             g_variant_new("(@a{ss})", g_variant_builder_end(&attributes)),
             G_VARIANT_TYPE("(ao)"));
    if (!response_result.ok()) {
      return Result<std::vector<std::string>>::Failure(response_result.error());
    }

    gchar** raw_paths = nullptr;
    g_variant_get(response_result.value().get(), "(^ao)", &raw_paths);
    StringVectorPtr paths(raw_paths);
    try {
      std::vector<std::string> result;
      if (raw_paths == nullptr) {
        return Result<std::vector<std::string>>::Success(std::move(result));
      }
      for (std::size_t index = 0; raw_paths[index] != nullptr; ++index) {
        auto locked_result = IsLocked(raw_paths[index], kItemInterface);
        if (!locked_result.ok()) {
          return Result<std::vector<std::string>>::Failure(locked_result.error());
        }
        if (locked_result.value()) {
          return Result<std::vector<std::string>>::Failure(ErrorCode::kStorageLocked);
        }
        result.emplace_back(raw_paths[index]);
      }
      return Result<std::vector<std::string>>::Success(std::move(result));
    } catch (const std::bad_alloc&) {
      return Result<std::vector<std::string>>::Failure(ErrorCode::kCapacityExceeded);
    }
  }

  [[nodiscard]] Result<ProtectedItemId> ReadItemId(const std::string_view path) const {
    auto property_result = GetProperty(path, kItemInterface, "Attributes");
    if (!property_result.ok()) {
      return Result<ProtectedItemId>::Failure(property_result.error());
    }
    GVariant* attributes = property_result.value().get();
    if (!g_variant_is_of_type(attributes, G_VARIANT_TYPE("a{ss}"))) {
      return Result<ProtectedItemId>::Failure(ErrorCode::kCorruptRecord);
    }

    std::string schema;
    std::string application;
    std::string item_id;
    std::size_t count = 0;
    GVariantIter iterator;
    g_variant_iter_init(&iterator, attributes);
    const gchar* key = nullptr;
    const gchar* value = nullptr;
    try {
      while (g_variant_iter_next(&iterator, "{&s&s}", &key, &value)) {
        ++count;
        if (std::strcmp(key, kSchemaAttribute) == 0) {
          schema = value;
        } else if (std::strcmp(key, kApplicationAttribute) == 0) {
          application = value;
        } else if (std::strcmp(key, kItemIdAttribute) == 0) {
          item_id = value;
        } else {
          return Result<ProtectedItemId>::Failure(ErrorCode::kCorruptRecord);
        }
      }
    } catch (const std::bad_alloc&) {
      return Result<ProtectedItemId>::Failure(ErrorCode::kCapacityExceeded);
    }
    if (count != 3 || schema != kSchemaName || application != kApplicationValue ||
        item_id.empty() || item_id.size() > kMaxProtectedItemIdBytes) {
      return Result<ProtectedItemId>::Failure(ErrorCode::kCorruptRecord);
    }
    return Result<ProtectedItemId>::Success(std::move(item_id));
  }

  [[nodiscard]] Result<LinuxSecretServiceItem> LoadItem(
      const std::string_view path,
      const std::optional<std::string_view> requested_id) const {
    auto id_result = ReadItemId(path);
    if (!id_result.ok()) {
      return Result<LinuxSecretServiceItem>::Failure(id_result.error());
    }
    ProtectedItemId item_id = std::move(id_result).value();
    if (requested_id.has_value() && item_id != *requested_id) {
      return Result<LinuxSecretServiceItem>::Failure(ErrorCode::kCorruptRecord);
    }

    GError* error = nullptr;
    SecretValue* raw_value = secret_service_get_secret_for_dbus_path_sync(
        service_.get(), std::string(path).c_str(), nullptr, &error);
    if (raw_value == nullptr) {
      if (error == nullptr) {
        return Result<LinuxSecretServiceItem>::Failure(ErrorCode::kStorageLocked);
      }
      return Result<LinuxSecretServiceItem>::Failure(TakeGError(error));
    }
    SecretValuePtr value(raw_value);
    Result<void> owner_result = CheckOwner();
    if (!owner_result.ok()) {
      return Result<LinuxSecretServiceItem>::Failure(owner_result.error());
    }

    const gchar* content_type = secret_value_get_content_type(value.get());
    gsize length = 0;
    const gchar* data = secret_value_get(value.get(), &length);
    if (content_type == nullptr || std::strcmp(content_type, kBinaryContentType) != 0 ||
        data == nullptr) {
      return Result<LinuxSecretServiceItem>::Failure(ErrorCode::kCorruptRecord);
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
    return DecodeEnvelope(item_id, std::span<const std::uint8_t>(bytes, length));
  }

  [[nodiscard]] static Result<LinuxSecretServiceItem> DecodeEnvelope(
      ProtectedItemId item_id, const std::span<const std::uint8_t> envelope) {
    if (envelope.size() < kEnvelopeHeaderSize ||
        envelope.size() > kEnvelopeHeaderSize + kMaxProtectedItemPayloadSize ||
        !std::equal(kEnvelopeMagic.begin(), kEnvelopeMagic.end(), envelope.begin()) ||
        envelope[4] != kEnvelopeVersion) {
      return Result<LinuxSecretServiceItem>::Failure(ErrorCode::kCorruptRecord);
    }
    const std::uint64_t revision = ReadU64(envelope.subspan(5, 8));
    const std::uint32_t payload_size = ReadU32(envelope.subspan(13, 4));
    if (revision == 0 || payload_size != envelope.size() - kEnvelopeHeaderSize) {
      return Result<LinuxSecretServiceItem>::Failure(ErrorCode::kCorruptRecord);
    }
    try {
      SecretBuffer payload(envelope.subspan(kEnvelopeHeaderSize, payload_size));
      return Result<LinuxSecretServiceItem>::Success(
          LinuxSecretServiceItem(std::move(item_id), revision, std::move(payload)));
    } catch (const std::bad_alloc&) {
      return Result<LinuxSecretServiceItem>::Failure(ErrorCode::kCapacityExceeded);
    }
  }

  [[nodiscard]] static Result<SecretBuffer> EncodeEnvelope(
      const LinuxSecretServiceItem& item) {
    if (item.revision == 0 || item.payload.size() > kMaxProtectedItemPayloadSize) {
      return Result<SecretBuffer>::Failure(ErrorCode::kInvalidArgument);
    }
    try {
      SecretBuffer envelope(kEnvelopeHeaderSize + item.payload.size());
      std::span<std::uint8_t> output = envelope.mutable_bytes();
      std::copy(kEnvelopeMagic.begin(), kEnvelopeMagic.end(), output.begin());
      output[4] = kEnvelopeVersion;
      WriteU64(output.subspan(5, 8), item.revision);
      WriteU32(output.subspan(13, 4), static_cast<std::uint32_t>(item.payload.size()));
      std::copy(item.payload.bytes().begin(), item.payload.bytes().end(),
                output.begin() + static_cast<std::ptrdiff_t>(kEnvelopeHeaderSize));
      return Result<SecretBuffer>::Success(std::move(envelope));
    } catch (const std::bad_alloc&) {
      return Result<SecretBuffer>::Failure(ErrorCode::kCapacityExceeded);
    }
  }

  SecretServicePtr service_;
  std::string owner_;
  std::string collection_path_;
  GDBusConnection* connection_;
};

[[nodiscard]] Result<std::uint32_t> QueryBusUint(GDBusConnection* connection,
                                                 const std::string_view method,
                                                 const std::string_view owner) {
  GError* error = nullptr;
  GVariant* response = g_dbus_connection_call_sync(
      connection, kBusServiceName, kBusServicePath, kBusServiceInterface,
      std::string(method).c_str(), g_variant_new("(s)", std::string(owner).c_str()),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, nullptr, &error);
  if (response == nullptr) {
    return Result<std::uint32_t>::Failure(TakeGError(error));
  }
  GVariantPtr owned_response(response);
  guint32 value = 0;
  g_variant_get(response, "(u)", &value);
  return Result<std::uint32_t>::Success(value);
}

[[nodiscard]] Result<std::string> ReadExecutablePath(const std::uint32_t process_id) {
  try {
    const std::string link = "/proc/" + std::to_string(process_id) + "/exe";
    std::array<char, PATH_MAX + 1> path{};
    const ssize_t length = readlink(link.c_str(), path.data(), path.size() - 1);
    if (length < 0) {
      return Result<std::string>::Failure(MapErrno(errno));
    }
    if (length == 0 || static_cast<std::size_t>(length) >= path.size() - 1) {
      return Result<std::string>::Failure(ErrorCode::kStorageUnavailable);
    }
    path[static_cast<std::size_t>(length)] = '\0';
    if (path[0] != '/') {
      return Result<std::string>::Failure(ErrorCode::kStorageUnavailable);
    }
    return Result<std::string>::Success(
        std::string(path.data(), static_cast<std::size_t>(length)));
  } catch (const std::bad_alloc&) {
    return Result<std::string>::Failure(ErrorCode::kCapacityExceeded);
  }
}

[[nodiscard]] Result<std::unique_ptr<LinuxSecretServiceBackend>> CreateBackend(
    const LinuxSecretServiceBackendQualifier& qualifier) {
  if (!qualifier) {
    return Result<std::unique_ptr<LinuxSecretServiceBackend>>::Failure(
        ErrorCode::kStorageUnavailable);
  }

  GError* error = nullptr;
  SecretService* raw_service = secret_service_open_sync(
      SECRET_TYPE_SERVICE, kServiceName, SECRET_SERVICE_OPEN_SESSION, nullptr, &error);
  if (raw_service == nullptr) {
    return Result<std::unique_ptr<LinuxSecretServiceBackend>>::Failure(
        TakeGError(error));
  }
  SecretServicePtr service(raw_service);
  const gchar* session_algorithm = secret_service_get_session_algorithms(service.get());
  if (session_algorithm == nullptr ||
      std::strcmp(session_algorithm, kRequiredSessionAlgorithm) != 0) {
    return Result<std::unique_ptr<LinuxSecretServiceBackend>>::Failure(
        ErrorCode::kStorageUnavailable);
  }
  GDBusProxy* proxy = G_DBUS_PROXY(service.get());
  const gchar* raw_owner = g_dbus_proxy_get_name_owner(proxy);
  if (raw_owner == nullptr || raw_owner[0] == '\0') {
    return Result<std::unique_ptr<LinuxSecretServiceBackend>>::Failure(
        ErrorCode::kStorageUnavailable);
  }

  try {
    const std::string owner(raw_owner);
    GDBusConnection* connection = g_dbus_proxy_get_connection(proxy);
    auto user_result = QueryBusUint(connection, "GetConnectionUnixUser", owner);
    if (!user_result.ok()) {
      return Result<std::unique_ptr<LinuxSecretServiceBackend>>::Failure(
          user_result.error());
    }
    auto process_result = QueryBusUint(connection, "GetConnectionUnixProcessID", owner);
    if (!process_result.ok()) {
      return Result<std::unique_ptr<LinuxSecretServiceBackend>>::Failure(
          process_result.error());
    }
    if (user_result.value() != static_cast<std::uint32_t>(geteuid())) {
      return Result<std::unique_ptr<LinuxSecretServiceBackend>>::Failure(
          ErrorCode::kPermissionDenied);
    }
    auto executable_result = ReadExecutablePath(process_result.value());
    if (!executable_result.ok()) {
      return Result<std::unique_ptr<LinuxSecretServiceBackend>>::Failure(
          executable_result.error());
    }

    LinuxSecretServiceBackendIdentity identity{user_result.value(),
                                               process_result.value(),
                                               std::move(executable_result).value()};
    bool qualified = false;
    try {
      qualified = qualifier(identity);
    } catch (...) {
      qualified = false;
    }
    if (!qualified) {
      return Result<std::unique_ptr<LinuxSecretServiceBackend>>::Failure(
          ErrorCode::kStorageUnavailable);
    }

    GError* alias_error = nullptr;
    GVariant* alias_response = g_dbus_connection_call_sync(
        connection, owner.c_str(), kServicePath, kServiceInterface, "ReadAlias",
        g_variant_new("(s)", SECRET_COLLECTION_DEFAULT), G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, nullptr, &alias_error);
    if (alias_response == nullptr) {
      return Result<std::unique_ptr<LinuxSecretServiceBackend>>::Failure(
          TakeGError(alias_error));
    }
    GVariantPtr owned_alias_response(alias_response);
    const gchar* raw_collection_path = nullptr;
    g_variant_get(alias_response, "(&o)", &raw_collection_path);
    if (raw_collection_path == nullptr || std::strcmp(raw_collection_path, "/") == 0) {
      return Result<std::unique_ptr<LinuxSecretServiceBackend>>::Failure(
          ErrorCode::kStorageUnavailable);
    }

    auto backend = std::make_unique<LibsecretBackend>(std::move(service), owner,
                                                      std::string(raw_collection_path));
    Result<void> initialization = backend->Initialize();
    if (!initialization.ok()) {
      return Result<std::unique_ptr<LinuxSecretServiceBackend>>::Failure(
          initialization.error());
    }
    std::unique_ptr<LinuxSecretServiceBackend> result = std::move(backend);
    return Result<std::unique_ptr<LinuxSecretServiceBackend>>::Success(
        std::move(result));
  } catch (const std::bad_alloc&) {
    return Result<std::unique_ptr<LinuxSecretServiceBackend>>::Failure(
        ErrorCode::kCapacityExceeded);
  }
}

}  // namespace

namespace internal {

std::unique_ptr<LinuxStoreOperationLock> MakeLinuxRuntimeDirectoryLock(
    std::string runtime_directory) {
  return std::make_unique<RuntimeDirectoryLock>(std::move(runtime_directory));
}

}  // namespace internal

Result<std::unique_ptr<ProtectedStore>> CreateLinuxSecretServiceProtectedStore(
    LinuxSecretServiceBackendQualifier qualifier) {
  if (!qualifier) {
    return Result<std::unique_ptr<ProtectedStore>>::Failure(
        ErrorCode::kStorageUnavailable);
  }
  auto backend_result = CreateBackend(qualifier);
  if (!backend_result.ok()) {
    return Result<std::unique_ptr<ProtectedStore>>::Failure(backend_result.error());
  }
  const char* runtime_directory = std::getenv("XDG_RUNTIME_DIR");
  if (runtime_directory == nullptr) {
    return Result<std::unique_ptr<ProtectedStore>>::Failure(
        ErrorCode::kStorageUnavailable);
  }
  try {
    auto operation_lock = internal::MakeLinuxRuntimeDirectoryLock(runtime_directory);
    std::unique_ptr<ProtectedStore> store = MakeLinuxSecretServiceStore(
        std::move(backend_result).value(), std::move(operation_lock));
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
