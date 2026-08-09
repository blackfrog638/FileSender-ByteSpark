#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "platform_protected_store_internal.hpp"
#include "xnn_transfer/core/security/identity/crypto.hpp"
#include "xnn_transfer/core/security/identity/identity_repository.hpp"

#if defined(__linux__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#define SECRET_API_SUBJECT_TO_CHANGE
#include <libsecret/secret.h>
#include <signal.h>
#include <sys/wait.h>

#include "xnn_transfer/core/security/identity/linux_secret_service_store.hpp"
#endif

#if defined(__APPLE__)
#include "xnn_transfer/core/security/identity/macos_keychain_store.hpp"
#endif

#if defined(_WIN32)
#include <process.h>

#include "xnn_transfer/core/security/identity/windows_credential_store.hpp"
#endif

namespace {

using xnn_transfer::core::security::identity::ErrorCode;
using xnn_transfer::core::security::identity::ErrorCodeName;
using xnn_transfer::core::security::identity::IdentityRepository;
using xnn_transfer::core::security::identity::kMaxProtectedItemIdBytes;
using xnn_transfer::core::security::identity::kMaxProtectedItemPayloadSize;
using xnn_transfer::core::security::identity::OpenSslIdentityCrypto;
using xnn_transfer::core::security::identity::PeerCommit;
using xnn_transfer::core::security::identity::PeerPublicKeyValidator;
using xnn_transfer::core::security::identity::ProtectedItem;
using xnn_transfer::core::security::identity::ProtectedItemId;
using xnn_transfer::core::security::identity::ProtectedItemMetadata;
using xnn_transfer::core::security::identity::ProtectedStore;
using xnn_transfer::core::security::identity::PublicKey;
using xnn_transfer::core::security::identity::Result;
using xnn_transfer::core::security::identity::SecretBuffer;
using xnn_transfer::core::security::identity::internal::
    DecodePlatformProtectedItemEnvelope;
using xnn_transfer::core::security::identity::internal::
    EncodePlatformProtectedItemEnvelope;
using xnn_transfer::core::security::identity::internal::MakePlatformProtectedStore;
using xnn_transfer::core::security::identity::internal::PlatformProtectedItem;
using xnn_transfer::core::security::identity::internal::PlatformProtectedStoreBackend;
using xnn_transfer::core::security::identity::internal::PlatformStoreLockGuard;
using xnn_transfer::core::security::identity::internal::PlatformStoreOperationLock;

int failures = 0;

void Expect(const bool condition, const std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

template <typename T>
void ExpectError(const Result<T>& result, const ErrorCode expected,
                 const std::string_view message) {
  if (result.ok() || result.error() != expected) {
    std::cerr << "FAIL: " << message << " (expected " << ErrorCodeName(expected)
              << ", got " << (result.ok() ? "SUCCESS" : ErrorCodeName(result.error()))
              << ")\n";
    ++failures;
  }
}

template <typename T>
[[nodiscard]] bool ExpectSuccess(const Result<T>& result,
                                 const std::string_view message) {
  if (result.ok()) {
    return true;
  }
  std::cerr << "FAIL: " << message << " (got " << ErrorCodeName(result.error())
            << ")\n";
  ++failures;
  return false;
}

SecretBuffer ByteBuffer(const std::uint8_t value) {
  const std::array<std::uint8_t, 1> bytes{value};
  return SecretBuffer(bytes);
}

void TestPlatformItemEnvelope() {
  PlatformProtectedItem item("root", 7, ByteBuffer(0x42));
  auto encoded = EncodePlatformProtectedItemEnvelope(item);
  Expect(encoded.ok(), "platform item envelope encodes");
  if (!encoded.ok()) {
    return;
  }

  auto decoded = DecodePlatformProtectedItemEnvelope("root", encoded.value().bytes());
  Expect(decoded.ok() && decoded.value().item_id == "root" &&
             decoded.value().revision == 7 &&
             decoded.value().payload.bytes()[0] == 0x42,
         "platform item envelope round-trips");

  std::vector<std::uint8_t> corrupt(encoded.value().bytes().begin(),
                                    encoded.value().bytes().end());
  corrupt[0] ^= 0xff;
  ExpectError(DecodePlatformProtectedItemEnvelope("root", corrupt),
              ErrorCode::kCorruptRecord, "platform item magic corruption fails closed");
  corrupt.assign(encoded.value().bytes().begin(), encoded.value().bytes().end() - 1);
  ExpectError(DecodePlatformProtectedItemEnvelope("root", corrupt),
              ErrorCode::kCorruptRecord, "platform item truncation fails closed");

  PlatformProtectedItem invalid("root", 0, ByteBuffer(0x42));
  ExpectError(EncodePlatformProtectedItemEnvelope(invalid), ErrorCode::kInvalidArgument,
              "platform item envelope rejects revision zero");
}

struct StoredItem {
  std::uint64_t revision{};
  std::vector<std::uint8_t> payload{};
};

struct FakeState {
  std::map<std::string, StoredItem> items{};
  std::atomic<int> lock_depth{0};
  std::atomic<std::size_t> lock_acquisitions{0};
  std::atomic<std::size_t> backend_calls{0};
  std::atomic<bool> unlocked_backend_access{false};
  std::atomic<ErrorCode> next_lock_error{ErrorCode::kNone};
  ErrorCode next_load_error{ErrorCode::kNone};
  ErrorCode next_put_error{ErrorCode::kNone};
  ErrorCode next_delete_error{ErrorCode::kNone};
  bool duplicate_load{};
  std::mutex operation_mutex{};
};

class FakeGuard final : public PlatformStoreLockGuard {
 public:
  FakeGuard(std::shared_ptr<FakeState> state, std::unique_lock<std::mutex> lock)
      : state_(std::move(state)), lock_(std::move(lock)) {
    state_->lock_depth.fetch_add(1);
  }

  ~FakeGuard() override { state_->lock_depth.fetch_sub(1); }

 private:
  std::shared_ptr<FakeState> state_;
  std::unique_lock<std::mutex> lock_;
};

class FakeLock final : public PlatformStoreOperationLock {
 public:
  explicit FakeLock(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

  Result<std::unique_ptr<PlatformStoreLockGuard>> Acquire() override {
    const ErrorCode error = state_->next_lock_error.exchange(ErrorCode::kNone);
    if (error != ErrorCode::kNone) {
      return Result<std::unique_ptr<PlatformStoreLockGuard>>::Failure(error);
    }
    state_->lock_acquisitions.fetch_add(1);
    std::unique_lock<std::mutex> lock(state_->operation_mutex);
    std::unique_ptr<PlatformStoreLockGuard> guard =
        std::make_unique<FakeGuard>(state_, std::move(lock));
    return Result<std::unique_ptr<PlatformStoreLockGuard>>::Success(std::move(guard));
  }

 private:
  std::shared_ptr<FakeState> state_;
};

class FakeBackend final : public PlatformProtectedStoreBackend {
 public:
  explicit FakeBackend(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

  Result<std::vector<PlatformProtectedItem>> Load(
      const std::optional<std::string_view> item_id) override {
    RecordCall();
    if (state_->next_load_error != ErrorCode::kNone) {
      const ErrorCode error = std::exchange(state_->next_load_error, ErrorCode::kNone);
      return Result<std::vector<PlatformProtectedItem>>::Failure(error);
    }

    std::vector<PlatformProtectedItem> result;
    for (const auto& [id, item] : state_->items) {
      if (item_id.has_value() && id != *item_id) {
        continue;
      }
      result.emplace_back(id, item.revision,
                          SecretBuffer(std::span<const std::uint8_t>(item.payload)));
      if (state_->duplicate_load) {
        result.emplace_back(id, item.revision,
                            SecretBuffer(std::span<const std::uint8_t>(item.payload)));
      }
    }
    return Result<std::vector<PlatformProtectedItem>>::Success(std::move(result));
  }

  Result<void> Put(const PlatformProtectedItem& item) override {
    RecordCall();
    if (state_->next_put_error != ErrorCode::kNone) {
      const ErrorCode error = std::exchange(state_->next_put_error, ErrorCode::kNone);
      return Result<void>::Failure(error);
    }
    state_->items[item.item_id] = StoredItem{
        item.revision, std::vector<std::uint8_t>(item.payload.bytes().begin(),
                                                 item.payload.bytes().end())};
    return Result<void>::Success();
  }

  Result<void> Delete(const std::string_view item_id) override {
    RecordCall();
    if (state_->next_delete_error != ErrorCode::kNone) {
      const ErrorCode error =
          std::exchange(state_->next_delete_error, ErrorCode::kNone);
      return Result<void>::Failure(error);
    }
    state_->items.erase(std::string(item_id));
    return Result<void>::Success();
  }

 private:
  void RecordCall() {
    state_->backend_calls.fetch_add(1);
    if (state_->lock_depth.load() != 1) {
      state_->unlocked_backend_access.store(true);
    }
  }

  std::shared_ptr<FakeState> state_;
};

struct Fixture {
  std::shared_ptr<FakeState> state{std::make_shared<FakeState>()};
  std::unique_ptr<ProtectedStore> store{MakePlatformProtectedStore(
      std::make_unique<FakeBackend>(state), std::make_unique<FakeLock>(state))};
};

void TestCasLifecycleAndOrdering() {
  Fixture fixture;
  Expect(fixture.store != nullptr, "fake platform protected store is created");

  auto empty = fixture.store->Enumerate();
  Expect(empty.ok() && empty.value().empty(), "empty backend enumerates as empty");

  Expect(
      fixture.store
          ->CompareExchangePut("root", std::nullopt, ProtectedItem(1, ByteBuffer(0x11)))
          .ok(),
      "absent item is created at revision one");
  ExpectError(fixture.store->CompareExchangePut("root", std::nullopt,
                                                ProtectedItem(1, ByteBuffer(0x12))),
              ErrorCode::kRevisionConflict, "second absent-item create conflicts");
  Expect(
      fixture.store->CompareExchangePut("root", 1, ProtectedItem(2, ByteBuffer(0x22)))
          .ok(),
      "matching revision is replaced");
  ExpectError(
      fixture.store->CompareExchangePut("root", 1, ProtectedItem(2, ByteBuffer(0x23))),
      ErrorCode::kRevisionConflict, "stale replacement conflicts");

  Expect(fixture.store
             ->CompareExchangePut("peer/z", std::nullopt,
                                  ProtectedItem(1, ByteBuffer(0x33)))
             .ok(),
         "second item is created");
  auto metadata = fixture.store->Enumerate();
  Expect(metadata.ok() && metadata.value().size() == 2 &&
             metadata.value()[0].item_id == "peer/z" &&
             metadata.value()[1].item_id == "root",
         "enumeration is deterministic and sorted");

  auto root = fixture.store->Get("root");
  Expect(root.ok() && root.value().has_value() && root.value()->revision == 2 &&
             root.value()->payload.bytes()[0] == 0x22,
         "get returns the protected payload and revision");
  ExpectError(fixture.store->CompareExchangeDelete("root", 1),
              ErrorCode::kRevisionConflict, "stale delete conflicts");
  Expect(fixture.store->CompareExchangeDelete("root", 2).ok(),
         "matching delete succeeds");
  auto missing = fixture.store->Get("root");
  Expect(missing.ok() && !missing.value().has_value(), "deleted item is absent");
  Expect(!fixture.state->unlocked_backend_access.load(),
         "every backend call occurs under the operation lock");
}

void TestInputAndBackendFailures() {
  Fixture fixture;
  ExpectError(fixture.store->Get(""), ErrorCode::kInvalidArgument,
              "empty item identifier is rejected");
  ExpectError(fixture.store->Get(std::string("bad\0id", 6)),
              ErrorCode::kInvalidArgument, "embedded NUL item identifier is rejected");
  ExpectError(fixture.store->Get(std::string(kMaxProtectedItemIdBytes + 1, 'x')),
              ErrorCode::kCapacityExceeded, "oversized item identifier is rejected");
  ExpectError(fixture.store->CompareExchangePut(
                  "root", std::nullopt,
                  ProtectedItem(1, SecretBuffer(kMaxProtectedItemPayloadSize + 1))),
              ErrorCode::kCapacityExceeded, "oversized protected payload is rejected");
  ExpectError(fixture.store->CompareExchangePut("root", std::nullopt,
                                                ProtectedItem(2, ByteBuffer(0x01))),
              ErrorCode::kInvalidArgument, "create requires revision one");
  ExpectError(
      fixture.store->CompareExchangePut("root", 1, ProtectedItem(3, ByteBuffer(0x01))),
      ErrorCode::kInvalidArgument, "replace requires exactly the next revision");
  ExpectError(fixture.store->CompareExchangeDelete("root", 0),
              ErrorCode::kInvalidArgument, "delete rejects revision zero");

  const std::size_t calls_before_lock_failure = fixture.state->backend_calls.load();
  fixture.state->next_lock_error.store(ErrorCode::kStorageLocked);
  ExpectError(fixture.store->Enumerate(), ErrorCode::kStorageLocked,
              "operation-lock failure is preserved");
  Expect(fixture.state->backend_calls.load() == calls_before_lock_failure,
         "lock failure prevents backend access");

  fixture.state->next_load_error = ErrorCode::kPermissionDenied;
  ExpectError(fixture.store->Get("root"), ErrorCode::kPermissionDenied,
              "backend read error is preserved");

  fixture.state->next_put_error = ErrorCode::kStorageUnavailable;
  ExpectError(fixture.store->CompareExchangePut("root", std::nullopt,
                                                ProtectedItem(1, ByteBuffer(0x44))),
              ErrorCode::kStorageUnavailable, "backend put error is preserved");
  Expect(fixture.state->items.empty(),
         "failed backend put does not mutate fake durable state");
}

void TestCorruptAndDuplicateItemsFailClosed() {
  Fixture fixture;
  fixture.state->items.emplace("root", StoredItem{0, std::vector<std::uint8_t>{0x01}});
  ExpectError(fixture.store->Get("root"), ErrorCode::kCorruptRecord,
              "zero backend revision is corrupt");
  fixture.state->items["root"].revision = 1;
  fixture.state->duplicate_load = true;
  ExpectError(fixture.store->Get("root"), ErrorCode::kCorruptRecord,
              "duplicate keyed items fail closed");
  ExpectError(fixture.store->Enumerate(), ErrorCode::kCorruptRecord,
              "duplicate enumeration fails closed");
}

void TestConcurrentFirstWriteIsAtomic() {
  Fixture fixture;
  constexpr std::size_t kWriters = 24;
  std::atomic<bool> start{false};
  std::atomic<std::size_t> successes{0};
  std::atomic<std::size_t> conflicts{0};
  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (std::size_t index = 0; index < kWriters; ++index) {
    writers.emplace_back([&, index]() {
      while (!start.load()) {
        std::this_thread::yield();
      }
      auto result = fixture.store->CompareExchangePut(
          "root", std::nullopt,
          ProtectedItem(1, ByteBuffer(static_cast<std::uint8_t>(index))));
      if (result.ok()) {
        successes.fetch_add(1);
      } else if (result.error() == ErrorCode::kRevisionConflict) {
        conflicts.fetch_add(1);
      }
    });
  }
  start.store(true);
  for (std::thread& writer : writers) {
    writer.join();
  }
  Expect(successes.load() == 1 && conflicts.load() == kWriters - 1,
         "concurrent first writers produce one winner");
  Expect(
      fixture.state->items.size() == 1 && fixture.state->items.at("root").revision == 1,
      "concurrent first write leaves one revision-one item");
  Expect(!fixture.state->unlocked_backend_access.load(),
         "concurrent operations remain serialized");
}

#if defined(__linux__) || defined(__APPLE__)
using xnn_transfer::core::security::identity::internal::MakePosixDirectoryLock;

#if defined(__linux__)
using xnn_transfer::core::security::identity::CreateLinuxSecretServiceProtectedStore;
using xnn_transfer::core::security::identity::IsQualifiedGnomeKeyringBackendIdentity;
using xnn_transfer::core::security::identity::kQualifiedGnomeKeyringExecutablePath;
using xnn_transfer::core::security::identity::LinuxSecretServiceBackendIdentity;

constexpr char kSecretServiceName[] = "org.freedesktop.secrets";
constexpr char kSecretServicePath[] = "/org/freedesktop/secrets";
constexpr char kSecretServiceInterface[] = "org.freedesktop.Secret.Service";
constexpr char kCollectionInterface[] = "org.freedesktop.Secret.Collection";
constexpr char kItemInterface[] = "org.freedesktop.Secret.Item";
constexpr char kPropertiesInterface[] = "org.freedesktop.DBus.Properties";
constexpr char kBusServiceName[] = "org.freedesktop.DBus";
constexpr char kBusServicePath[] = "/org/freedesktop/DBus";
constexpr char kBusServiceInterface[] = "org.freedesktop.DBus";
constexpr char kQualifiedCollectionPath[] = "/org/freedesktop/secrets/collection/login";
constexpr char kSchemaAttribute[] = "xdg:schema";
constexpr char kSchemaName[] = "com.xnntransfer.identity.v1";
constexpr char kApplicationAttribute[] = "application";
constexpr char kApplicationValue[] = "xnn-transfer";
constexpr char kItemIdAttribute[] = "item-id";
constexpr char kItemLabelProperty[] = "org.freedesktop.Secret.Item.Label";
constexpr char kItemAttributesProperty[] = "org.freedesktop.Secret.Item.Attributes";
constexpr char kItemLabel[] = "XnnTransfer protected identity";
constexpr char kBinaryContentType[] = "application/octet-stream";
constexpr char kGnomeKeyringReturnedContentType[] = "text/plain";
constexpr int kWorkerConflictExit = 10;
constexpr int kWorkerFailureExit = 20;

std::string linux_test_program;

struct GVariantDeleter {
  void operator()(GVariant* value) const noexcept {
    if (value != nullptr) {
      g_variant_unref(value);
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

struct SecretValueDeleter {
  void operator()(SecretValue* value) const noexcept {
    if (value != nullptr) {
      secret_value_unref(value);
    }
  }
};

struct GCharDeleter {
  void operator()(gchar* value) const noexcept {
    if (value != nullptr) {
      g_free(value);
    }
  }
};

using GVariantPtr = std::unique_ptr<GVariant, GVariantDeleter>;
using SecretServicePtr = std::unique_ptr<SecretService, SecretServiceDeleter>;
using SecretValuePtr = std::unique_ptr<SecretValue, SecretValueDeleter>;
using GCharPtr = std::unique_ptr<gchar, GCharDeleter>;

class LinuxSecretServiceInspector final {
 public:
  static std::unique_ptr<LinuxSecretServiceInspector> Open() {
    GError* error = nullptr;
    SecretService* raw_service =
        secret_service_open_sync(SECRET_TYPE_SERVICE, kSecretServiceName,
                                 SECRET_SERVICE_OPEN_SESSION, nullptr, &error);
    if (raw_service == nullptr) {
      if (error != nullptr) {
        g_error_free(error);
      }
      return nullptr;
    }
    SecretServicePtr service(raw_service);
    GDBusProxy* proxy = G_DBUS_PROXY(service.get());
    GCharPtr owner(g_dbus_proxy_get_name_owner(proxy));
    if (owner == nullptr || owner.get()[0] == '\0') {
      return nullptr;
    }
    auto inspector = std::unique_ptr<LinuxSecretServiceInspector>(
        new LinuxSecretServiceInspector(std::move(service), owner.get()));
    auto alias = inspector->Call(
        kSecretServicePath, kSecretServiceInterface, "ReadAlias",
        g_variant_new("(s)", SECRET_COLLECTION_DEFAULT), G_VARIANT_TYPE("(o)"));
    if (alias == nullptr) {
      return nullptr;
    }
    const gchar* collection_path = nullptr;
    g_variant_get(alias.get(), "(&o)", &collection_path);
    if (collection_path == nullptr || collection_path[0] == '\0') {
      return nullptr;
    }
    inspector->collection_path_ = collection_path;
    return inspector;
  }

  [[nodiscard]] std::uint32_t OwnerProcessId() const {
    auto response = Call(
        kBusServicePath, kBusServiceInterface, "GetConnectionUnixProcessID",
        g_variant_new("(s)", owner_.c_str()), G_VARIANT_TYPE("(u)"), kBusServiceName);
    if (response == nullptr) {
      return 0;
    }
    guint32 process_id = 0;
    g_variant_get(response.get(), "(u)", &process_id);
    return process_id;
  }

  [[nodiscard]] const std::string& collection_path() const noexcept {
    return collection_path_;
  }

  [[nodiscard]] std::vector<std::string> Search(
      const std::optional<std::string_view> item_id = std::nullopt) const {
    GVariantBuilder attributes;
    g_variant_builder_init(&attributes, G_VARIANT_TYPE("a{ss}"));
    g_variant_builder_add(&attributes, "{ss}", kSchemaAttribute, kSchemaName);
    g_variant_builder_add(&attributes, "{ss}", kApplicationAttribute,
                          kApplicationValue);
    std::string owned_item_id;
    if (item_id.has_value()) {
      owned_item_id = *item_id;
      g_variant_builder_add(&attributes, "{ss}", kItemIdAttribute,
                            owned_item_id.c_str());
    }
    auto response = Call(collection_path_, kCollectionInterface, "SearchItems",
                         g_variant_new("(@a{ss})", g_variant_builder_end(&attributes)),
                         G_VARIANT_TYPE("(ao)"));
    if (response == nullptr) {
      return {};
    }
    gchar** raw_paths = nullptr;
    g_variant_get(response.get(), "(^ao)", &raw_paths);
    std::vector<std::string> paths;
    if (raw_paths != nullptr) {
      for (std::size_t index = 0; raw_paths[index] != nullptr; ++index) {
        paths.emplace_back(raw_paths[index]);
      }
      g_strfreev(raw_paths);
    }
    return paths;
  }

  [[nodiscard]] std::optional<std::map<std::string, std::string>> Attributes(
      const std::string_view item_path) const {
    auto response = Call(item_path, kPropertiesInterface, "Get",
                         g_variant_new("(ss)", kItemInterface, "Attributes"),
                         G_VARIANT_TYPE("(v)"));
    if (response == nullptr) {
      return std::nullopt;
    }
    GVariant* boxed = g_variant_get_child_value(response.get(), 0);
    if (boxed == nullptr) {
      return std::nullopt;
    }
    GVariantPtr owned_boxed(boxed);
    GVariant* raw_attributes = g_variant_get_variant(boxed);
    if (raw_attributes == nullptr ||
        !g_variant_is_of_type(raw_attributes, G_VARIANT_TYPE("a{ss}"))) {
      if (raw_attributes != nullptr) {
        g_variant_unref(raw_attributes);
      }
      return std::nullopt;
    }
    GVariantPtr attributes(raw_attributes);
    std::map<std::string, std::string> result;
    GVariantIter iterator;
    g_variant_iter_init(&iterator, attributes.get());
    const gchar* key = nullptr;
    const gchar* value = nullptr;
    while (g_variant_iter_next(&iterator, "{&s&s}", &key, &value)) {
      result.emplace(key, value);
    }
    return result;
  }

  [[nodiscard]] std::vector<std::uint8_t> RawSecret(
      const std::string_view item_path) const {
    GError* error = nullptr;
    SecretValue* raw_value = secret_service_get_secret_for_dbus_path_sync(
        service_.get(), std::string(item_path).c_str(), nullptr, &error);
    if (raw_value == nullptr) {
      if (error != nullptr) {
        g_error_free(error);
      }
      return {};
    }
    SecretValuePtr value(raw_value);
    const gchar* content_type = secret_value_get_content_type(value.get());
    gsize length = 0;
    const gchar* data = secret_value_get(value.get(), &length);
    if (content_type == nullptr ||
        std::strcmp(content_type, kGnomeKeyringReturnedContentType) != 0 ||
        data == nullptr) {
      return {};
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
    return std::vector<std::uint8_t>(bytes, bytes + length);
  }

  [[nodiscard]] bool ReplaceRawSecret(
      const std::string& item_id,
      const std::span<const std::uint8_t> raw_secret) const {
    SecretValuePtr value(
        secret_value_new(reinterpret_cast<const gchar*>(raw_secret.data()),
                         static_cast<gssize>(raw_secret.size()), kBinaryContentType));
    if (value == nullptr) {
      return false;
    }
    GVariant* encoded_secret =
        secret_service_encode_dbus_secret(service_.get(), value.get());
    if (encoded_secret == nullptr) {
      return false;
    }

    GVariantBuilder attributes;
    g_variant_builder_init(&attributes, G_VARIANT_TYPE("a{ss}"));
    g_variant_builder_add(&attributes, "{ss}", kSchemaAttribute, kSchemaName);
    g_variant_builder_add(&attributes, "{ss}", kApplicationAttribute,
                          kApplicationValue);
    g_variant_builder_add(&attributes, "{ss}", kItemIdAttribute, item_id.c_str());

    GVariantBuilder properties;
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&properties, "{sv}", kItemLabelProperty,
                          g_variant_new_string(kItemLabel));
    g_variant_builder_add(&properties, "{sv}", kItemAttributesProperty,
                          g_variant_builder_end(&attributes));
    auto response =
        Call(collection_path_, kCollectionInterface, "CreateItem",
             g_variant_new("(@a{sv}@(oayays)b)", g_variant_builder_end(&properties),
                           encoded_secret, TRUE),
             G_VARIANT_TYPE("(oo)"));
    if (response == nullptr) {
      return false;
    }
    const gchar* created_path = nullptr;
    const gchar* prompt_path = nullptr;
    g_variant_get(response.get(), "(&o&o)", &created_path, &prompt_path);
    return created_path != nullptr && std::strcmp(created_path, "/") != 0 &&
           prompt_path != nullptr && std::strcmp(prompt_path, "/") == 0;
  }

  [[nodiscard]] bool DeleteAllApplicationItems() const {
    for (const std::string& path : Search()) {
      auto response = Call(path, kItemInterface, "Delete", g_variant_new("()"),
                           G_VARIANT_TYPE("(o)"));
      if (response == nullptr) {
        return false;
      }
      const gchar* prompt_path = nullptr;
      g_variant_get(response.get(), "(&o)", &prompt_path);
      if (prompt_path == nullptr || std::strcmp(prompt_path, "/") != 0) {
        return false;
      }
    }
    return Search().empty();
  }

  [[nodiscard]] bool LockCollection() const {
    const gchar* paths[] = {collection_path_.c_str(), nullptr};
    auto response = Call(kSecretServicePath, kSecretServiceInterface, "Lock",
                         g_variant_new("(^ao)", paths), G_VARIANT_TYPE("(aoo)"));
    if (response == nullptr) {
      return false;
    }
    gchar** locked = nullptr;
    const gchar* prompt_path = nullptr;
    g_variant_get(response.get(), "(^ao&o)", &locked, &prompt_path);
    bool found = false;
    if (locked != nullptr) {
      for (std::size_t index = 0; locked[index] != nullptr; ++index) {
        found = found || collection_path_ == locked[index];
      }
      g_strfreev(locked);
    }
    return found && prompt_path != nullptr && std::strcmp(prompt_path, "/") == 0;
  }

 private:
  LinuxSecretServiceInspector(SecretServicePtr service, std::string owner)
      : service_(std::move(service)),
        owner_(std::move(owner)),
        connection_(g_dbus_proxy_get_connection(G_DBUS_PROXY(service_.get()))) {}

  [[nodiscard]] GVariantPtr Call(const std::string_view object_path,
                                 const char* interface_name, const char* method_name,
                                 GVariant* parameters, const GVariantType* reply_type,
                                 const std::string_view destination = {}) const {
    const std::string owned_path(object_path);
    const std::string owned_destination =
        destination.empty() ? owner_ : std::string(destination);
    GError* error = nullptr;
    GVariant* response = g_dbus_connection_call_sync(
        connection_, owned_destination.c_str(), owned_path.c_str(), interface_name,
        method_name, parameters, reply_type, G_DBUS_CALL_FLAGS_NO_AUTO_START, 5'000,
        nullptr, &error);
    if (response == nullptr) {
      if (error != nullptr) {
        g_error_free(error);
      }
      return nullptr;
    }
    return GVariantPtr(response);
  }

  SecretServicePtr service_;
  std::string owner_;
  std::string collection_path_;
  GDBusConnection* connection_;
};

[[nodiscard]] std::string ReadProcessExecutable(const std::uint32_t process_id) {
  std::array<char, 4'097> path{};
  const std::string link = "/proc/" + std::to_string(process_id) + "/exe";
  const ssize_t length = readlink(link.c_str(), path.data(), path.size() - 1);
  if (length <= 0 || static_cast<std::size_t>(length) >= path.size()) {
    return {};
  }
  return std::string(path.data(), static_cast<std::size_t>(length));
}

constexpr std::string_view kTestKeyringPassword = "xnn-transfer-ci-keyring";

[[nodiscard]] bool WriteTestKeyringPassword(const int descriptor) {
  std::size_t written = 0;
  while (written < kTestKeyringPassword.size()) {
    const ssize_t count = write(descriptor, kTestKeyringPassword.data() + written,
                                kTestKeyringPassword.size() - written);
    if (count <= 0) {
      return false;
    }
    written += static_cast<std::size_t>(count);
  }
  return true;
}

[[nodiscard]] pid_t LaunchGnomeKeyringDaemon(const std::string& daemon_path,
                                             const std::string& control_directory) {
  int input_pipe[2]{};
  if (pipe(input_pipe) != 0) {
    return -1;
  }
  const pid_t child = fork();
  if (child < 0) {
    close(input_pipe[0]);
    close(input_pipe[1]);
    return -1;
  }
  if (child == 0) {
    close(input_pipe[1]);
    if (dup2(input_pipe[0], STDIN_FILENO) < 0) {
      _exit(126);
    }
    close(input_pipe[0]);
    const std::string control_option = "--control-directory=" + control_directory;
    execl(daemon_path.c_str(), daemon_path.c_str(), "--foreground", "--unlock",
          "--components=secrets", control_option.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }

  close(input_pipe[0]);
  const bool password_written = WriteTestKeyringPassword(input_pipe[1]);
  close(input_pipe[1]);
  if (!password_written) {
    static_cast<void>(kill(child, SIGKILL));
    static_cast<void>(waitpid(child, nullptr, 0));
    return -1;
  }
  return child;
}

[[nodiscard]] std::uint32_t SecretServiceOwnerProcessIdWithoutActivation() {
  GError* error = nullptr;
  GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (connection == nullptr) {
    if (error != nullptr) {
      g_error_free(error);
    }
    return 0;
  }
  GVariant* owner_response = g_dbus_connection_call_sync(
      connection, kBusServiceName, kBusServicePath, kBusServiceInterface,
      "GetNameOwner", g_variant_new("(s)", kSecretServiceName), G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START, 1'000, nullptr, &error);
  if (owner_response == nullptr) {
    if (error != nullptr) {
      g_error_free(error);
    }
    g_object_unref(connection);
    return 0;
  }
  GVariantPtr owned_owner_response(owner_response);
  const gchar* owner = nullptr;
  g_variant_get(owner_response, "(&s)", &owner);
  if (owner == nullptr || owner[0] == '\0') {
    g_object_unref(connection);
    return 0;
  }
  GVariant* process_response = g_dbus_connection_call_sync(
      connection, kBusServiceName, kBusServicePath, kBusServiceInterface,
      "GetConnectionUnixProcessID", g_variant_new("(s)", owner), G_VARIANT_TYPE("(u)"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START, 1'000, nullptr, &error);
  g_object_unref(connection);
  if (process_response == nullptr) {
    if (error != nullptr) {
      g_error_free(error);
    }
    return 0;
  }
  GVariantPtr owned_process_response(process_response);
  guint32 process_id = 0;
  g_variant_get(process_response, "(u)", &process_id);
  return process_id;
}

class LinuxSecretServiceEnvironment final {
 public:
  explicit LinuxSecretServiceEnvironment(std::string daemon_path)
      : daemon_path_(std::move(daemon_path)) {}

  ~LinuxSecretServiceEnvironment() {
    Stop();
    if (!root_directory_.empty()) {
      std::error_code error;
      std::filesystem::remove_all(root_directory_, error);
    }
  }

  [[nodiscard]] bool Prepare() {
    std::array<char, 48> path{};
    constexpr std::string_view kTemplate = "/tmp/xnn-transfer-secret-service-XXXXXX";
    std::copy(kTemplate.begin(), kTemplate.end(), path.begin());
    char* root = mkdtemp(path.data());
    if (root == nullptr) {
      return false;
    }
    root_directory_ = root;
    home_directory_ = root_directory_ + "/home";
    data_directory_ = root_directory_ + "/data";
    runtime_directory_ = root_directory_ + "/runtime";
    control_directory_ = runtime_directory_ + "/keyring";
    std::error_code error;
    if (!std::filesystem::create_directory(home_directory_, error) ||
        !std::filesystem::create_directory(data_directory_, error) ||
        !std::filesystem::create_directory(runtime_directory_, error) ||
        chmod(home_directory_.c_str(), 0700) != 0 ||
        chmod(data_directory_.c_str(), 0700) != 0 ||
        chmod(runtime_directory_.c_str(), 0700) != 0) {
      return false;
    }
    setenv("HOME", home_directory_.c_str(), 1);
    setenv("XDG_DATA_HOME", data_directory_.c_str(), 1);
    setenv("XDG_RUNTIME_DIR", runtime_directory_.c_str(), 1);
    unsetenv("GNOME_KEYRING_CONTROL");
    return Start();
  }

  [[nodiscard]] bool Start() {
    unsetenv("GNOME_KEYRING_CONTROL");
    std::error_code error;
    std::filesystem::remove_all(control_directory_, error);
    const pid_t child =
        error ? -1 : LaunchGnomeKeyringDaemon(daemon_path_, control_directory_);
    if (child <= 0) {
      return false;
    }
    daemon_process_id_ = static_cast<std::uint32_t>(child);
    setenv("GNOME_KEYRING_CONTROL", control_directory_.c_str(), 1);
    for (std::size_t attempt = 0; attempt < 50; ++attempt) {
      int status = 0;
      if (waitpid(child, &status, WNOHANG) == child) {
        daemon_process_id_ = 0;
        unsetenv("GNOME_KEYRING_CONTROL");
        return false;
      }
      if (SecretServiceOwnerProcessIdWithoutActivation() == daemon_process_id_) {
        auto inspector = LinuxSecretServiceInspector::Open();
        if (inspector != nullptr &&
            inspector->collection_path() == kQualifiedCollectionPath &&
            inspector->OwnerProcessId() == daemon_process_id_) {
          return true;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    Stop();
    return false;
  }

  void Stop() {
    if (daemon_process_id_ == 0) {
      return;
    }
    const pid_t child = static_cast<pid_t>(daemon_process_id_);
    static_cast<void>(kill(child, SIGTERM));
    bool stopped = false;
    for (std::size_t attempt = 0; attempt < 50; ++attempt) {
      int status = 0;
      const pid_t result = waitpid(child, &status, WNOHANG);
      if (result == child || (result < 0 && errno == ECHILD)) {
        stopped = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!stopped) {
      static_cast<void>(kill(child, SIGKILL));
      static_cast<void>(waitpid(child, nullptr, 0));
    }
    daemon_process_id_ = 0;
    unsetenv("GNOME_KEYRING_CONTROL");
  }

  [[nodiscard]] const std::string& data_directory() const noexcept {
    return data_directory_;
  }

  [[nodiscard]] const std::string& runtime_directory() const noexcept {
    return runtime_directory_;
  }

 private:
  std::string daemon_path_;
  std::string root_directory_;
  std::string home_directory_;
  std::string data_directory_;
  std::string runtime_directory_;
  std::string control_directory_;
  std::uint32_t daemon_process_id_{};
};

[[nodiscard]] std::unique_ptr<ProtectedStore> OpenLinuxProtectedStore() {
  for (std::size_t attempt = 0; attempt < 30; ++attempt) {
    auto result = CreateLinuxSecretServiceProtectedStore();
    if (result.ok()) {
      return std::move(result).value();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return nullptr;
}

[[nodiscard]] SecretBuffer FilledBuffer(const std::size_t size,
                                        const std::uint8_t value) {
  SecretBuffer result(size);
  std::fill(result.mutable_bytes().begin(), result.mutable_bytes().end(), value);
  return result;
}

[[nodiscard]] int RunLinuxSecretServiceWorker(const int argc, char** argv) {
  if (argc != 6) {
    return kWorkerFailureExit;
  }
  const std::string_view operation(argv[2]);
  const std::string item_id(argv[3]);
  const int barrier_descriptor = std::atoi(argv[4]);
  const int worker_index = std::atoi(argv[5]);
  auto store = OpenLinuxProtectedStore();
  if (store == nullptr || barrier_descriptor < 0) {
    return kWorkerFailureExit;
  }
  char release = 0;
  if (read(barrier_descriptor, &release, 1) != 1) {
    return kWorkerFailureExit;
  }
  close(barrier_descriptor);

  Result<void> result = Result<void>::Failure(ErrorCode::kInvalidArgument);
  if (operation == "create") {
    result = store->CompareExchangePut(
        item_id, std::nullopt,
        ProtectedItem(1, ByteBuffer(static_cast<std::uint8_t>(0x50 + worker_index))));
  } else if (operation == "replace") {
    result = store->CompareExchangePut(
        item_id, 1,
        ProtectedItem(2, ByteBuffer(static_cast<std::uint8_t>(0x60 + worker_index))));
  }
  if (result.ok()) {
    return 0;
  }
  return result.error() == ErrorCode::kRevisionConflict ? kWorkerConflictExit
                                                        : kWorkerFailureExit;
}

struct WorkerProcess {
  pid_t process_id{-1};
  int release_descriptor{-1};
};

[[nodiscard]] WorkerProcess SpawnWorker(const std::string_view operation,
                                        const std::string_view item_id,
                                        const int worker_index) {
  int barrier[2]{};
  if (pipe(barrier) != 0) {
    return {};
  }
  const pid_t child = fork();
  if (child < 0) {
    close(barrier[0]);
    close(barrier[1]);
    return {};
  }
  if (child == 0) {
    close(barrier[1]);
    const std::string descriptor = std::to_string(barrier[0]);
    const std::string index = std::to_string(worker_index);
    execl(linux_test_program.c_str(), linux_test_program.c_str(),
          "--linux-secret-service-worker", std::string(operation).c_str(),
          std::string(item_id).c_str(), descriptor.c_str(), index.c_str(),
          static_cast<char*>(nullptr));
    _exit(127);
  }
  close(barrier[0]);
  return WorkerProcess{child, barrier[1]};
}

[[nodiscard]] std::array<int, 2> RunContendingWorkers(const std::string_view operation,
                                                      const std::string_view item_id) {
  std::array<WorkerProcess, 2> workers{
      SpawnWorker(operation, item_id, 0),
      SpawnWorker(operation, item_id, 1),
  };
  for (WorkerProcess& worker : workers) {
    if (worker.release_descriptor >= 0) {
      const char release = 1;
      static_cast<void>(write(worker.release_descriptor, &release, 1));
      close(worker.release_descriptor);
      worker.release_descriptor = -1;
    }
  }

  std::array<int, 2> exits{kWorkerFailureExit, kWorkerFailureExit};
  for (std::size_t index = 0; index < workers.size(); ++index) {
    int status = 0;
    if (workers[index].process_id > 0 &&
        waitpid(workers[index].process_id, &status, 0) == workers[index].process_id &&
        WIFEXITED(status)) {
      exits[index] = WEXITSTATUS(status);
    }
  }
  return exits;
}

[[nodiscard]] bool HasOneSuccessAndOneConflict(std::array<int, 2> exits) {
  std::sort(exits.begin(), exits.end());
  return exits[0] == 0 && exits[1] == kWorkerConflictExit;
}

[[nodiscard]] bool FileContains(const std::filesystem::path& path,
                                const std::span<const std::uint8_t> needle) {
  if (needle.empty()) {
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  const std::vector<std::uint8_t> content{std::istreambuf_iterator<char>(input),
                                          std::istreambuf_iterator<char>()};
  return std::search(content.begin(), content.end(), needle.begin(), needle.end()) !=
         content.end();
}

[[nodiscard]] bool DirectoryContains(const std::filesystem::path& directory,
                                     const std::span<const std::uint8_t> needle) {
  std::error_code error;
  if (!std::filesystem::exists(directory, error)) {
    return false;
  }
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(directory, error)) {
    if (error) {
      return false;
    }
    if (entry.is_regular_file(error) && FileContains(entry.path(), needle)) {
      return true;
    }
  }
  return false;
}

class AcceptingPeerPublicKeyValidator final : public PeerPublicKeyValidator {
 public:
  Result<void> Validate(const PublicKey&) override { return Result<void>::Success(); }
};

[[nodiscard]] PublicKey PublicFromIndex(OpenSslIdentityCrypto& crypto,
                                        const std::uint16_t value) {
  std::array<std::uint8_t, 32> seed{};
  seed[0] = static_cast<std::uint8_t>(value);
  seed[1] = static_cast<std::uint8_t>(value >> 8U);
  seed.back() = 0xa5U;
  auto result = crypto.DerivePublicKey(seed);
  return result.ok() ? result.value() : PublicKey{};
}

[[nodiscard]] std::uint32_t ReadU32(const std::span<const std::uint8_t> bytes,
                                    const std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value = static_cast<std::uint32_t>(
        (value << 8U) | static_cast<std::uint32_t>(bytes[offset + index]));
  }
  return value;
}

void WriteU16(const std::span<std::uint8_t> bytes, const std::size_t offset,
              const std::uint16_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value >> 8U);
  bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

void WriteU32(const std::span<std::uint8_t> bytes, const std::size_t offset,
              const std::uint32_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value >> 24U);
  bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16U);
  bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8U);
  bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

[[nodiscard]] SecretBuffer RemoveRootSeed(const SecretBuffer& root_item) {
  std::vector<std::uint8_t> bytes(root_item.bytes().begin(), root_item.bytes().end());
  constexpr std::size_t kEnvelopeSize = 12;
  constexpr std::size_t kFieldHeaderSize = 6;
  constexpr std::size_t kSeedSize = 32;
  constexpr std::size_t kSeedFieldSize = kFieldHeaderSize + kSeedSize;
  const std::uint32_t body_size = ReadU32(bytes, 8);
  bytes.erase(
      bytes.begin() + static_cast<std::ptrdiff_t>(kEnvelopeSize),
      bytes.begin() + static_cast<std::ptrdiff_t>(kEnvelopeSize + kSeedFieldSize));
  WriteU16(bytes, 6, 4);
  WriteU32(bytes, 8, body_size - static_cast<std::uint32_t>(kSeedFieldSize));
  return SecretBuffer(bytes);
}

[[nodiscard]] SecretBuffer CloneSecret(const SecretBuffer& source) {
  return SecretBuffer(source.bytes());
}

[[nodiscard]] bool ClearStore(ProtectedStore& store) {
  auto metadata = store.Enumerate();
  if (!metadata.ok()) {
    return false;
  }
  for (const ProtectedItemMetadata& item : metadata.value()) {
    if (!store.CompareExchangeDelete(item.item_id, item.revision).ok()) {
      return false;
    }
  }
  auto empty = store.Enumerate();
  return empty.ok() && empty.value().empty();
}

void TestGnomeKeyringQualificationRejectsUnknownIdentity() {
  LinuxSecretServiceBackendIdentity wrong_path{
      static_cast<std::uint32_t>(geteuid()),
      static_cast<std::uint32_t>(getpid()),
      "/tmp/gnome-keyring-daemon",
  };
  Expect(!IsQualifiedGnomeKeyringBackendIdentity(wrong_path),
         "GNOME Keyring policy rejects an unreviewed executable path");
  LinuxSecretServiceBackendIdentity wrong_user{
      static_cast<std::uint32_t>(geteuid()) + 1U,
      static_cast<std::uint32_t>(getpid()),
      std::string(kQualifiedGnomeKeyringExecutablePath),
  };
  Expect(!IsQualifiedGnomeKeyringBackendIdentity(wrong_user),
         "GNOME Keyring policy rejects another user");
  LinuxSecretServiceBackendIdentity missing_process{
      static_cast<std::uint32_t>(geteuid()),
      0,
      std::string(kQualifiedGnomeKeyringExecutablePath),
  };
  Expect(!IsQualifiedGnomeKeyringBackendIdentity(missing_process),
         "GNOME Keyring policy rejects an absent service process");
}

void TestRealGnomeKeyringLifecycle(const std::string& daemon_path) {
  std::error_code path_error;
  const std::filesystem::path canonical_daemon =
      std::filesystem::canonical(daemon_path, path_error);
  Expect(!path_error && canonical_daemon ==
                            std::filesystem::path(kQualifiedGnomeKeyringExecutablePath),
         "integration test uses the exact qualified GNOME Keyring executable");
  if (path_error ||
      canonical_daemon != std::filesystem::path(kQualifiedGnomeKeyringExecutablePath)) {
    return;
  }

  LinuxSecretServiceEnvironment environment(canonical_daemon.string());
  Expect(environment.Prepare(),
         "isolated GNOME Keyring starts with a persistent login collection");
  if (failures != 0) {
    return;
  }

  auto inspector = LinuxSecretServiceInspector::Open();
  Expect(
      inspector != nullptr && inspector->collection_path() == kQualifiedCollectionPath,
      "default collection is the qualified persistent login collection");
  if (inspector == nullptr) {
    return;
  }
  const std::uint32_t first_owner_process = inspector->OwnerProcessId();
  LinuxSecretServiceBackendIdentity live_identity{
      static_cast<std::uint32_t>(geteuid()),
      first_owner_process,
      ReadProcessExecutable(first_owner_process),
  };
  Expect(IsQualifiedGnomeKeyringBackendIdentity(live_identity),
         "live service owner matches the reviewed GNOME Keyring profile");

  auto store = OpenLinuxProtectedStore();
  Expect(store != nullptr, "qualified GNOME Keyring creates the production store");
  if (store == nullptr) {
    return;
  }

  constexpr std::string_view kLifecycleItem = "integration/lifecycle";
  SecretBuffer first_payload = FilledBuffer(64, 0xa1U);
  const std::vector<std::uint8_t> first_payload_copy(first_payload.bytes().begin(),
                                                     first_payload.bytes().end());
  const auto create_result =
      store->CompareExchangePut(std::string(kLifecycleItem), std::nullopt,
                                ProtectedItem(1, std::move(first_payload)));
  static_cast<void>(ExpectSuccess(create_result,
                                  "real Secret Service creates a bounded binary item"));
  auto created = store->Get(std::string(kLifecycleItem));
  if (ExpectSuccess(created, "real Secret Service reads the created item")) {
    Expect(created.value().has_value() && created.value()->revision == 1 &&
               created.value()->payload.bytes().size() == first_payload_copy.size() &&
               std::equal(created.value()->payload.bytes().begin(),
                          created.value()->payload.bytes().end(),
                          first_payload_copy.begin(), first_payload_copy.end()),
           "real Secret Service preserves the created item");
  }
  ExpectError(store->CompareExchangePut(std::string(kLifecycleItem), std::nullopt,
                                        ProtectedItem(1, ByteBuffer(0x01))),
              ErrorCode::kRevisionConflict,
              "real Secret Service rejects a duplicate first write");
  Expect(store
             ->CompareExchangePut(std::string(kLifecycleItem), 1,
                                  ProtectedItem(2, FilledBuffer(64, 0xa2U)))
             .ok(),
         "real Secret Service replaces the matching revision");
  ExpectError(store->CompareExchangePut(std::string(kLifecycleItem), 1,
                                        ProtectedItem(2, FilledBuffer(64, 0xa3U))),
              ErrorCode::kRevisionConflict,
              "real Secret Service rejects a stale replacement");

  const std::vector<std::string> lifecycle_paths = inspector->Search(kLifecycleItem);
  Expect(lifecycle_paths.size() == 1,
         "qualified collection contains exactly one lifecycle item");
  if (lifecycle_paths.size() == 1) {
    const auto attributes = inspector->Attributes(lifecycle_paths.front());
    const std::map<std::string, std::string> expected_attributes{
        {kApplicationAttribute, kApplicationValue},
        {kItemIdAttribute, std::string(kLifecycleItem)},
        {kSchemaAttribute, kSchemaName},
    };
    Expect(attributes.has_value() && *attributes == expected_attributes,
           "Secret Service item exposes only the three reviewed lookup attributes");
    const std::vector<std::uint8_t> raw_secret =
        inspector->RawSecret(lifecycle_paths.front());
    Expect(raw_secret.size() == 64 + 17 &&
               std::equal(raw_secret.begin(), raw_secret.begin() + 4, "XNSP"),
           "libsecret boundary stores one bounded binary XNSP envelope");
  }

  const std::filesystem::path lock_path =
      std::filesystem::path(environment.runtime_directory()) /
      ".xnn-transfer-identity.lock";
  struct stat lock_status{};
  Expect(stat(lock_path.c_str(), &lock_status) == 0 && S_ISREG(lock_status.st_mode) &&
             lock_status.st_size == 0 && (lock_status.st_mode & 0777) == 0600,
         "cross-process lock is a private payload-free regular file");
  const std::filesystem::path keyring_directory =
      std::filesystem::path(environment.data_directory()) / "keyrings";
  Expect(std::filesystem::exists(keyring_directory) &&
             !DirectoryContains(keyring_directory, first_payload_copy),
         "persistent keyring files do not contain the application plaintext");

  auto metadata = store->Enumerate();
  Expect(metadata.ok() && metadata.value().size() == 1 &&
             metadata.value().front().item_id == kLifecycleItem &&
             metadata.value().front().revision == 2,
         "real Secret Service enumeration is bounded and revision preserving");
  Expect(store->CompareExchangeDelete(std::string(kLifecycleItem), 2).ok(),
         "real Secret Service deletes the matching revision");
  auto deleted = store->Get(std::string(kLifecycleItem));
  Expect(deleted.ok() && !deleted.value().has_value(),
         "real Secret Service deletion is observable immediately");

  constexpr std::string_view kFirstWriteItem = "integration/first-write";
  Expect(HasOneSuccessAndOneConflict(RunContendingWorkers("create", kFirstWriteItem)),
         "two processes racing a first write produce one winner");
  auto first_write = store->Get(std::string(kFirstWriteItem));
  Expect(first_write.ok() && first_write.value().has_value() &&
             first_write.value()->revision == 1,
         "cross-process first-write race leaves one revision-one item");
  if (first_write.ok() && first_write.value().has_value()) {
    Expect(store
               ->CompareExchangeDelete(std::string(kFirstWriteItem),
                                       first_write.value()->revision)
               .ok(),
           "first-write race item is deleted");
  }

  constexpr std::string_view kStaleWriteItem = "integration/stale-write";
  Expect(store
             ->CompareExchangePut(std::string(kStaleWriteItem), std::nullopt,
                                  ProtectedItem(1, ByteBuffer(0x70)))
             .ok(),
         "stale-writer fixture creates revision one");
  Expect(HasOneSuccessAndOneConflict(RunContendingWorkers("replace", kStaleWriteItem)),
         "two processes racing a stale revision produce one winner");
  auto stale_write = store->Get(std::string(kStaleWriteItem));
  Expect(stale_write.ok() && stale_write.value().has_value() &&
             stale_write.value()->revision == 2,
         "cross-process stale-revision race leaves one revision-two item");
  if (stale_write.ok() && stale_write.value().has_value()) {
    Expect(store
               ->CompareExchangeDelete(std::string(kStaleWriteItem),
                                       stale_write.value()->revision)
               .ok(),
           "stale-write race item is deleted");
  }

  constexpr std::string_view kRestartItem = "integration/restart";
  Expect(store
             ->CompareExchangePut(std::string(kRestartItem), std::nullopt,
                                  ProtectedItem(1, ByteBuffer(0x7f)))
             .ok(),
         "restart fixture persists an item");
  environment.Stop();
  ExpectError(store->Enumerate(), ErrorCode::kStorageUnavailable,
              "captured service owner becomes unusable after daemon exit");
  Expect(environment.Start(), "GNOME Keyring restarts over the same persistent data");
  inspector = LinuxSecretServiceInspector::Open();
  Expect(inspector != nullptr && inspector->OwnerProcessId() != 0 &&
             inspector->OwnerProcessId() != first_owner_process,
         "restart replaces the unique D-Bus owner process");
  store = OpenLinuxProtectedStore();
  Expect(store != nullptr, "production policy accepts the restarted qualified daemon");
  if (store == nullptr || inspector == nullptr) {
    return;
  }
  auto restarted_item = store->Get(std::string(kRestartItem));
  Expect(restarted_item.ok() && restarted_item.value().has_value() &&
             restarted_item.value()->revision == 1 &&
             restarted_item.value()->payload.bytes()[0] == 0x7f,
         "item persists across daemon and client-process restart");
  if (restarted_item.ok() && restarted_item.value().has_value()) {
    Expect(store
               ->CompareExchangeDelete(std::string(kRestartItem),
                                       restarted_item.value()->revision)
               .ok(),
           "restart fixture item is deleted");
  }

  Expect(inspector->LockCollection(),
         "integration fixture locks the persistent collection without a prompt");
  ExpectError(store->Enumerate(), ErrorCode::kStorageLocked,
              "locked collection disables store access");
  auto locked_factory = CreateLinuxSecretServiceProtectedStore();
  ExpectError(locked_factory, ErrorCode::kStorageLocked,
              "locked collection disables new production stores");
  environment.Stop();
  Expect(environment.Start(),
         "isolated daemon restart unlocks the collection with the test password");
  inspector = LinuxSecretServiceInspector::Open();
  store = OpenLinuxProtectedStore();
  Expect(inspector != nullptr && store != nullptr,
         "unlocked collection restores qualified access");
  if (inspector == nullptr || store == nullptr) {
    return;
  }

  Expect(chmod(environment.runtime_directory().c_str(), 0755) == 0,
         "runtime directory can be made unsafe for the denial test");
  auto denied_factory = CreateLinuxSecretServiceProtectedStore();
  ExpectError(denied_factory, ErrorCode::kPermissionDenied,
              "unsafe runtime policy denies access before touching libsecret");
  Expect(chmod(environment.runtime_directory().c_str(), 0700) == 0,
         "runtime directory privacy is restored");

  Expect(ClearStore(*store), "repository fixture starts from an empty real store");
  OpenSslIdentityCrypto crypto;
  AcceptingPeerPublicKeyValidator validator;
  IdentityRepository repository(*store, crypto, validator);
  Expect(repository.Open().ok(), "real store initializes the identity repository");
  const auto committed = repository.CommitPeer(
      PeerCommit{PublicFromIndex(crypto, 1), 1, "Linux integration peer"});
  Expect(committed.ok() && repository.peers().size() == 1,
         "real store persists an authenticated peer record");

  SecretBuffer observed_seed;
  const auto seed_result = repository.UseIdentitySeed(
      [&observed_seed](const std::span<const std::uint8_t> seed) {
        observed_seed = SecretBuffer(seed);
        return Result<void>::Success();
      });
  Expect(seed_result.ok() && observed_seed.size() == 32 &&
             !DirectoryContains(keyring_directory, observed_seed.bytes()) &&
             !FileContains(lock_path, observed_seed.bytes()),
         "identity seed is absent from keyring plaintext and runtime lock files");
  observed_seed.clear();

  auto root_result = store->Get("root");
  auto repository_metadata = store->Enumerate();
  ProtectedItemId peer_item_id;
  if (repository_metadata.ok()) {
    for (const ProtectedItemMetadata& item : repository_metadata.value()) {
      if (item.item_id.starts_with("peer/")) {
        peer_item_id = item.item_id;
      }
    }
  }
  auto peer_result =
      peer_item_id.empty()
          ? Result<std::optional<ProtectedItem>>::Failure(ErrorCode::kNotFound)
          : store->Get(peer_item_id);
  Expect(root_result.ok() && root_result.value().has_value() &&
             root_result.value()->revision == 1 && peer_result.ok() &&
             peer_result.value().has_value(),
         "real repository exposes one root and one separately protected peer");
  if (!root_result.ok() || !root_result.value().has_value() || !peer_result.ok() ||
      !peer_result.value().has_value()) {
    return;
  }
  ProtectedItem root_backup(root_result.value()->revision,
                            CloneSecret(root_result.value()->payload));
  ProtectedItem peer_backup(peer_result.value()->revision,
                            CloneSecret(peer_result.value()->payload));

  Expect(store->CompareExchangeDelete("root", root_backup.revision).ok(),
         "partial-deletion fixture removes the protected root");
  IdentityRepository missing_root(*store, crypto, validator);
  ExpectError(missing_root.Open(), ErrorCode::kIdentityLoss,
              "surviving peer items cannot regenerate a missing root");
  Expect(store
             ->CompareExchangePut(
                 "root", std::nullopt,
                 ProtectedItem(root_backup.revision, CloneSecret(root_backup.payload)))
             .ok(),
         "partial-deletion fixture restores its original root");

  auto missing_seed_payload = RemoveRootSeed(root_backup.payload);
  Expect(store
             ->CompareExchangePut("root", root_backup.revision,
                                  ProtectedItem(root_backup.revision + 1,
                                                std::move(missing_seed_payload)))
             .ok(),
         "missing-seed fixture replaces the root through real CAS");
  IdentityRepository missing_seed(*store, crypto, validator);
  ExpectError(missing_seed.Open(), ErrorCode::kIdentityLoss,
              "root metadata without a seed remains identity loss");

  auto restored_root_envelope =
      EncodePlatformProtectedItemEnvelope(PlatformProtectedItem(
          "root", root_backup.revision, CloneSecret(root_backup.payload)));
  Expect(
      restored_root_envelope.ok() &&
          inspector->ReplaceRawSecret("root", restored_root_envelope.value().bytes()),
      "integration inspector restores the complete prior root envelope");
  IdentityRepository restored(*store, crypto, validator);
  Expect(restored.Open().ok() && restored.peers().size() == 1,
         "complete valid platform restore remains the documented residual limit");

  const auto reset = restored.Reset();
  Expect(reset.ok() && reset.value().cleanup_complete && restored.peers().empty(),
         "real store reset commits a fresh root and cleans the retired generation");
  Expect(store
             ->CompareExchangePut(
                 peer_item_id, std::nullopt,
                 ProtectedItem(peer_backup.revision, CloneSecret(peer_backup.payload)))
             .ok(),
         "partial-restore fixture reintroduces one retired peer item");
  IdentityRepository after_partial_restore(*store, crypto, validator);
  Expect(after_partial_restore.Open().ok() && after_partial_restore.peers().empty(),
         "restored peer from a retired generation does not restore trust");
  Expect(after_partial_restore.CleanupStaleItems().ok(),
         "stale-generation cleanup deletes the partially restored peer");
  auto stale_peer = store->Get(peer_item_id);
  Expect(stale_peer.ok() && !stale_peer.value().has_value(),
         "retired peer is absent after resumable cleanup");

  auto current_root_result = store->Get("root");
  Expect(current_root_result.ok() && current_root_result.value().has_value() &&
             current_root_result.value()->revision == 2,
         "reset leaves root revision two in the real store");
  if (!current_root_result.ok() || !current_root_result.value().has_value()) {
    return;
  }
  ProtectedItem current_root(current_root_result.value()->revision,
                             CloneSecret(current_root_result.value()->payload));
  Expect(store
             ->CompareExchangePut("root", current_root.revision,
                                  ProtectedItem(current_root.revision + 1,
                                                CloneSecret(current_root.payload)))
             .ok(),
         "rollback fixture creates an external/internal revision mismatch");
  IdentityRepository rolled_back(*store, crypto, validator);
  ExpectError(rolled_back.Open(), ErrorCode::kRollbackDetected,
              "real store detects an internally inconsistent rollback");

  auto current_root_envelope =
      EncodePlatformProtectedItemEnvelope(PlatformProtectedItem(
          "root", current_root.revision, CloneSecret(current_root.payload)));
  Expect(current_root_envelope.ok() &&
             inspector->ReplaceRawSecret("root", current_root_envelope.value().bytes()),
         "rollback fixture restores the valid current root");
  IdentityRepository after_valid_restore(*store, crypto, validator);
  Expect(after_valid_restore.Open().ok() && after_valid_restore.peers().empty(),
         "valid current root remains usable after complete platform restore");

  const std::array<std::uint8_t, 4> corrupt_envelope{0x00, 0x01, 0x02, 0x03};
  Expect(inspector->ReplaceRawSecret("root", corrupt_envelope),
         "corruption fixture replaces the raw Secret Service value");
  auto corrupt_root = store->Get("root");
  ExpectError(corrupt_root, ErrorCode::kCorruptRecord,
              "corrupt real-service envelope fails closed");
  Expect(inspector->DeleteAllApplicationItems(),
         "integration fixture removes every application item");
}
#endif

void TestRuntimeDirectoryLockSecurity() {
  std::array<char, 40> template_path{};
  constexpr std::string_view kTemplate = "/tmp/xnn-transfer-lock-test-XXXXXX";
  std::copy(kTemplate.begin(), kTemplate.end(), template_path.begin());
  char* directory = mkdtemp(template_path.data());
  Expect(directory != nullptr, "runtime-lock test directory is created");
  if (directory == nullptr) {
    return;
  }

  Expect(chmod(directory, 0700) == 0, "runtime-lock test directory is private");
  auto operation_lock = MakePosixDirectoryLock(directory);
  auto guard_result = operation_lock->Acquire();
  Expect(guard_result.ok(), "private runtime directory accepts a lock");
  if (guard_result.ok()) {
    auto guard = std::move(guard_result).value();
    const std::string lock_path =
        std::string(directory) + "/.xnn-transfer-identity.lock";
    struct stat status{};
    Expect(stat(lock_path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0600 &&
               status.st_uid == geteuid(),
           "runtime lock file is owned by the user with mode 0600");
    guard.reset();

    Expect(chmod(directory, 0755) == 0,
           "runtime-lock test directory can be made insecure");
    auto insecure_directory = operation_lock->Acquire();
    ExpectError(insecure_directory, ErrorCode::kPermissionDenied,
                "group-readable runtime directory is rejected");
    Expect(chmod(directory, 0700) == 0,
           "runtime-lock test directory privacy is restored");

    Expect(chmod(lock_path.c_str(), 0644) == 0,
           "runtime lock file can be made insecure");
    auto insecure_file = operation_lock->Acquire();
    ExpectError(insecure_file, ErrorCode::kPermissionDenied,
                "group-readable runtime lock file is rejected");
    Expect(unlink(lock_path.c_str()) == 0, "runtime lock test file is removed");
  }
  Expect(rmdir(directory) == 0, "runtime-lock test directory is removed");
}
#endif

#if defined(__APPLE__)
void TestMacosKeychainLifecycle() {
  auto store_result =
      xnn_transfer::core::security::identity::CreateMacosKeychainProtectedStore();
  Expect(store_result.ok(), "macOS Keychain protected store is available");
  if (!store_result.ok()) {
    return;
  }
  std::unique_ptr<ProtectedStore> store = std::move(store_result).value();
  const std::string item_id =
      "adapter-test-" + std::to_string(static_cast<long long>(getpid()));

  auto stale = store->Get(item_id);
  if (stale.ok() && stale.value().has_value()) {
    [[maybe_unused]] auto stale_cleanup =
        store->CompareExchangeDelete(item_id, stale.value()->revision);
  }

  Expect(store
             ->CompareExchangePut(item_id, std::nullopt,
                                  ProtectedItem(1, ByteBuffer(0x31)))
             .ok(),
         "macOS Keychain creates a device-only item");
  auto created = store->Get(item_id);
  Expect(created.ok() && created.value().has_value() &&
             created.value()->revision == 1 &&
             created.value()->payload.bytes()[0] == 0x31,
         "macOS Keychain reads the created item");
  Expect(store->CompareExchangePut(item_id, 1, ProtectedItem(2, ByteBuffer(0x32))).ok(),
         "macOS Keychain replaces an item under CAS");
  Expect(store->CompareExchangeDelete(item_id, 2).ok(),
         "macOS Keychain deletes the matching revision");
  auto deleted = store->Get(item_id);
  Expect(deleted.ok() && !deleted.value().has_value(),
         "macOS Keychain deletion is durable");

  if (!deleted.ok() || deleted.value().has_value()) {
    auto cleanup = store->Get(item_id);
    if (cleanup.ok() && cleanup.value().has_value()) {
      [[maybe_unused]] auto cleanup_result =
          store->CompareExchangeDelete(item_id, cleanup.value()->revision);
    }
  }
}
#endif

#if defined(_WIN32)
void TestWindowsCredentialLifecycle() {
  auto store_result =
      xnn_transfer::core::security::identity::CreateWindowsCredentialProtectedStore();
  Expect(store_result.ok(), "Windows Credential Manager protected store is available");
  if (!store_result.ok()) {
    return;
  }
  std::unique_ptr<ProtectedStore> store = std::move(store_result).value();
  const std::string item_id =
      "adapter-test-" + std::to_string(static_cast<long long>(_getpid()));

  auto stale = store->Get(item_id);
  if (stale.ok() && stale.value().has_value()) {
    [[maybe_unused]] auto stale_cleanup =
        store->CompareExchangeDelete(item_id, stale.value()->revision);
  }

  Expect(store
             ->CompareExchangePut(item_id, std::nullopt,
                                  ProtectedItem(1, ByteBuffer(0x41)))
             .ok(),
         "Windows Credential Manager creates a local item");
  auto created = store->Get(item_id);
  Expect(created.ok() && created.value().has_value() &&
             created.value()->revision == 1 &&
             created.value()->payload.bytes()[0] == 0x41,
         "Windows Credential Manager reads the created item");
  Expect(store->CompareExchangePut(item_id, 1, ProtectedItem(2, ByteBuffer(0x42))).ok(),
         "Windows Credential Manager replaces an item under CAS");
  Expect(store->CompareExchangeDelete(item_id, 2).ok(),
         "Windows Credential Manager deletes the matching revision");
  auto deleted = store->Get(item_id);
  Expect(deleted.ok() && !deleted.value().has_value(),
         "Windows Credential Manager deletion is durable");

  if (!deleted.ok() || deleted.value().has_value()) {
    auto cleanup = store->Get(item_id);
    if (cleanup.ok() && cleanup.value().has_value()) {
      [[maybe_unused]] auto cleanup_result =
          store->CompareExchangeDelete(item_id, cleanup.value()->revision);
    }
  }
}
#endif

}  // namespace

int main(const int argc, char** argv) {
#if defined(__linux__)
  linux_test_program = argv[0];
  if (argc >= 2 && std::string_view(argv[1]) == "--linux-secret-service-worker") {
    return RunLinuxSecretServiceWorker(argc, argv);
  }
  if (argc == 3 && std::string_view(argv[1]) == "--linux-secret-service-integration") {
    TestRealGnomeKeyringLifecycle(argv[2]);
    if (failures != 0) {
      std::cerr << failures << " real GNOME Keyring integration assertion(s) failed\n";
      return 1;
    }
    std::cout << "Real GNOME Keyring integration tests passed.\n";
    return 0;
  }
#else
  static_cast<void>(argc);
  static_cast<void>(argv);
#endif

  TestPlatformItemEnvelope();
  TestCasLifecycleAndOrdering();
  TestInputAndBackendFailures();
  TestCorruptAndDuplicateItemsFailClosed();
  TestConcurrentFirstWriteIsAtomic();
#if defined(__linux__)
  TestGnomeKeyringQualificationRejectsUnknownIdentity();
#endif
#if defined(__linux__) || defined(__APPLE__)
  TestRuntimeDirectoryLockSecurity();
#endif
#if defined(__APPLE__)
  TestMacosKeychainLifecycle();
#endif
#if defined(_WIN32)
  TestWindowsCredentialLifecycle();
#endif

  if (failures != 0) {
    std::cerr << failures << " platform protected-store assertion(s) failed\n";
    return 1;
  }
  std::cout << "Platform protected-store tests passed.\n";
  return 0;
}
