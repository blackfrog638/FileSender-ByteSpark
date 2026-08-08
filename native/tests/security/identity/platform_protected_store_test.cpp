#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
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

#if defined(__linux__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__linux__)
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
using xnn_transfer::core::security::identity::kMaxProtectedItemIdBytes;
using xnn_transfer::core::security::identity::kMaxProtectedItemPayloadSize;
using xnn_transfer::core::security::identity::ProtectedItem;
using xnn_transfer::core::security::identity::ProtectedItemMetadata;
using xnn_transfer::core::security::identity::ProtectedStore;
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
  Expect(!result.ok() && result.error() == expected, message);
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
void TestUnqualifiedProductionBackendIsDisabled() {
  auto result =
      xnn_transfer::core::security::identity::CreateLinuxSecretServiceProtectedStore(
          {});
  ExpectError(result, ErrorCode::kStorageUnavailable,
              "missing Linux backend qualification fails closed");
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

int main() {
  TestPlatformItemEnvelope();
  TestCasLifecycleAndOrdering();
  TestInputAndBackendFailures();
  TestCorruptAndDuplicateItemsFailClosed();
  TestConcurrentFirstWriteIsAtomic();
#if defined(__linux__)
  TestUnqualifiedProductionBackendIsDisabled();
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
