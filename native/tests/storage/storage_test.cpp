#include "xnn_transfer/core/storage/storage.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using xnn_transfer::core::storage::FilesystemBackendOpenResult;
using xnn_transfer::core::storage::kMaxFileBytes;
using xnn_transfer::core::storage::OpenFilesystemBackend;
using xnn_transfer::core::storage::PathValidationResult;
using xnn_transfer::core::storage::PlatformBackend;
using xnn_transfer::core::storage::PlatformCommitDisposition;
using xnn_transfer::core::storage::PlatformCommitResult;
using xnn_transfer::core::storage::PlatformError;
using xnn_transfer::core::storage::PlatformResult;
using xnn_transfer::core::storage::PlatformWriteResult;
using xnn_transfer::core::storage::ReceiveTransaction;
using xnn_transfer::core::storage::RequestValidationResult;
using xnn_transfer::core::storage::StreamingIntegrityVerifier;
using xnn_transfer::core::storage::TemporaryBudget;
using xnn_transfer::core::storage::TemporaryFileHandle;
using xnn_transfer::core::storage::TransactionError;
using xnn_transfer::core::storage::TransactionResult;
using xnn_transfer::core::storage::TransactionState;
using xnn_transfer::core::storage::ValidatedReceivePath;
using xnn_transfer::core::storage::ValidatedReceiveRequest;
using xnn_transfer::core::storage::ValidateReceivePath;
using xnn_transfer::core::storage::ValidateReceiveRequest;
using xnn_transfer::core::storage::ValidationError;

static_assert(!std::is_aggregate_v<ValidatedReceivePath>);
static_assert(!std::is_default_constructible_v<ValidatedReceivePath>);
static_assert(!std::is_constructible_v<ValidatedReceivePath, std::string,
                                       std::vector<std::string>>);
static_assert(!std::is_copy_assignable_v<ValidatedReceivePath>);
static_assert(!std::is_move_assignable_v<ValidatedReceivePath>);
static_assert(
    std::is_same_v<decltype(std::declval<const ValidatedReceivePath&>().utf8()),
                   std::string_view>);
static_assert(
    std::is_same_v<decltype(std::declval<const ValidatedReceivePath&>().components()),
                   std::span<const std::string>>);

static_assert(!std::is_aggregate_v<ValidatedReceiveRequest>);
static_assert(!std::is_default_constructible_v<ValidatedReceiveRequest>);
static_assert(!std::is_constructible_v<ValidatedReceiveRequest, ValidatedReceivePath,
                                       std::uint64_t>);
static_assert(!std::is_copy_assignable_v<ValidatedReceiveRequest>);
static_assert(!std::is_move_assignable_v<ValidatedReceiveRequest>);
static_assert(
    std::is_same_v<decltype(std::declval<const ValidatedReceiveRequest&>().path()),
                   const ValidatedReceivePath&>);

using CreateTemporaryMethod = PlatformResult (PlatformBackend::*)(
    const ValidatedReceivePath&, std::uint64_t, TemporaryFileHandle&);
using CommitTemporaryMethod = PlatformCommitResult (PlatformBackend::*)(
    TemporaryFileHandle, const ValidatedReceivePath&);
static_assert(
    std::is_same_v<decltype(&PlatformBackend::CreateTemporary), CreateTemporaryMethod>);
static_assert(
    std::is_same_v<decltype(&PlatformBackend::CommitTemporary), CommitTemporaryMethod>);
static_assert(
    std::is_constructible_v<
        ReceiveTransaction, ValidatedReceiveRequest, std::shared_ptr<TemporaryBudget>,
        std::unique_ptr<StreamingIntegrityVerifier>, PlatformBackend&>);
static_assert(!std::is_constructible_v<
              ReceiveTransaction, std::string, std::shared_ptr<TemporaryBudget>,
              std::unique_ptr<StreamingIntegrityVerifier>, PlatformBackend&>);

enum class FixtureOwner : std::uint8_t {
  kStorage,
  kXt028,
  kXt033,
};

struct ManifestFixtureCase {
  std::string_view name;
  FixtureOwner owner;
  std::string_view reason;
  std::string_view path_hex;
  std::uint64_t declared_size;
  ValidationError expected_path_error;
  ValidationError expected_request_error;
};

#include "manifest_fixture_cases.inc"

int failures = 0;

void Expect(const bool condition, const std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

[[nodiscard]] std::span<const std::uint8_t> Bytes(const std::string_view value) {
  return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

[[nodiscard]] std::vector<std::uint8_t> DecodeHex(const std::string_view encoded) {
  auto nibble = [](const char value) -> std::uint8_t {
    if (value >= '0' && value <= '9') {
      return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
      return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    return static_cast<std::uint8_t>(value - 'A' + 10);
  };

  std::vector<std::uint8_t> result;
  result.reserve(encoded.size() / 2);
  for (std::size_t offset = 0; offset < encoded.size(); offset += 2) {
    result.push_back(static_cast<std::uint8_t>((nibble(encoded[offset]) << 4U) |
                                               nibble(encoded[offset + 1])));
  }
  return result;
}

[[nodiscard]] ValidatedReceiveRequest Request(const std::string_view path,
                                              const std::uint64_t declared_size) {
  const RequestValidationResult result =
      ValidateReceiveRequest(Bytes(path), declared_size);
  Expect(result.ok(), "test request is valid");
  Expect(result.request() != nullptr, "valid result contains a request capability");
  return *result.request();
}

class FakeVerifier final : public StreamingIntegrityVerifier {
 public:
  [[nodiscard]] bool Update(const std::span<const std::uint8_t> data) override {
    ++update_calls;
    observed.insert(observed.end(), data.begin(), data.end());
    return update_succeeds;
  }

  [[nodiscard]] bool Seal() override {
    ++seal_calls;
    sealed = true;
    return seal_succeeds;
  }

  bool update_succeeds{true};
  bool seal_succeeds{true};
  bool sealed{};
  std::size_t update_calls{};
  std::size_t seal_calls{};
  std::vector<std::uint8_t> observed{};
};

class FakePlatformBackend final : public PlatformBackend {
 public:
  [[nodiscard]] PlatformResult CreateTemporary(const ValidatedReceivePath& path,
                                               const std::uint64_t declared_size,
                                               TemporaryFileHandle& output) override {
    ++create_calls;
    created_path = path.utf8();
    created_size = declared_size;
    if (create_error != PlatformError::kNone) {
      if (return_handle_on_create_error) {
        output = {.value = 1};
        temporary_open = true;
      }
      return {.error = create_error};
    }
    output = {.value = 1};
    temporary_open = true;
    return {};
  }

  [[nodiscard]] PlatformWriteResult WriteTemporary(
      const TemporaryFileHandle handle,
      const std::span<const std::uint8_t> bytes) override {
    ++write_calls;
    Expect(handle.valid() && temporary_open,
           "fake write receives an open temporary handle");
    std::size_t written = bytes.size();
    if (short_write && written != 0) {
      --written;
    }
    data.insert(data.end(), bytes.begin(),
                bytes.begin() + static_cast<std::ptrdiff_t>(written));
    return {.error = write_error, .bytes_written = written};
  }

  [[nodiscard]] PlatformResult FlushTemporary(
      const TemporaryFileHandle handle) override {
    ++flush_calls;
    Expect(handle.valid() && temporary_open,
           "fake flush receives an open temporary handle");
    return {.error = flush_error};
  }

  [[nodiscard]] PlatformCommitResult CommitTemporary(
      const TemporaryFileHandle handle,
      const ValidatedReceivePath& destination_path) override {
    ++commit_calls;
    Expect(handle.valid() && temporary_open,
           "fake commit receives an open temporary handle");
    committed_path = destination_path.utf8();
    if (commit_disposition == PlatformCommitDisposition::kCommitted) {
      destination = data;
      temporary_open = false;
    } else if (commit_disposition == PlatformCommitDisposition::kOutcomeUncertain) {
      temporary_open = false;
      owned_orphans.push_back(handle);
    }
    return {.disposition = commit_disposition, .error = commit_error};
  }

  [[nodiscard]] PlatformResult CleanupTemporary(
      const TemporaryFileHandle handle) override {
    ++cleanup_calls;
    Expect(handle.valid() && temporary_open,
           "fake cleanup consumes a valid open handle");
    temporary_open = false;
    if (cleanup_error == PlatformError::kNone) {
      data.clear();
    } else {
      owned_orphans.push_back(handle);
    }
    return {.error = cleanup_error};
  }

  PlatformError create_error{PlatformError::kNone};
  PlatformError write_error{PlatformError::kNone};
  PlatformError flush_error{PlatformError::kNone};
  PlatformError cleanup_error{PlatformError::kNone};
  PlatformCommitDisposition commit_disposition{PlatformCommitDisposition::kCommitted};
  PlatformError commit_error{PlatformError::kNone};
  bool return_handle_on_create_error{};
  bool short_write{};
  bool temporary_open{};
  std::size_t create_calls{};
  std::size_t write_calls{};
  std::size_t flush_calls{};
  std::size_t commit_calls{};
  std::size_t cleanup_calls{};
  std::uint64_t created_size{};
  std::string created_path{};
  std::string committed_path{};
  std::vector<std::uint8_t> data{};
  std::vector<std::uint8_t> destination{};
  std::vector<TemporaryFileHandle> owned_orphans{};
};

class ScopedDirectory final {
 public:
  explicit ScopedDirectory(const std::string_view label) {
    static std::atomic<std::uint64_t> next{1};
    const std::uint64_t suffix =
        static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()) ^
        next.fetch_add(1, std::memory_order_relaxed);
    path_ = std::filesystem::temp_directory_path() /
            (std::string("xnn-storage-") + std::string(label) + "-" +
             std::to_string(suffix));
    std::error_code error;
    std::filesystem::create_directories(path_, error);
    Expect(!error, "temporary test directory is created");
  }

  ~ScopedDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  ScopedDirectory(const ScopedDirectory&) = delete;
  ScopedDirectory& operator=(const ScopedDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_{};
};

[[nodiscard]] std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void WriteFile(const std::filesystem::path& path, const std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  Expect(output.good(), "test fixture file is written");
}

void TestManifestFixtureOwnershipAndPathValidation() {
  static_assert(kManifestFixtureCases.size() == 66);
  static_assert(kStorageFixtureCaseCount + kXt028FixtureCaseCount +
                    kXt033FixtureCaseCount ==
                kManifestFixtureCases.size());

  std::size_t storage_cases = 0;
  std::size_t xt028_cases = 0;
  std::size_t xt033_cases = 0;
  for (const ManifestFixtureCase& fixture : kManifestFixtureCases) {
    if (fixture.owner == FixtureOwner::kXt028) {
      ++xt028_cases;
      Expect(fixture.path_hex.empty(),
             "XT-028 fixture is classification-only in storage tests");
      continue;
    }
    if (fixture.owner == FixtureOwner::kXt033) {
      ++xt033_cases;
      Expect(fixture.path_hex.empty(),
             "XT-033 fixture is classification-only in storage tests");
      continue;
    }

    ++storage_cases;
    const std::vector<std::uint8_t> path = DecodeHex(fixture.path_hex);
    const PathValidationResult path_result = ValidateReceivePath(path);
    if (path_result.error() != fixture.expected_path_error) {
      std::cerr << "FAILED: " << fixture.name << '/' << fixture.reason
                << " path validation mismatch\n";
      ++failures;
    }
    const RequestValidationResult request_result =
        ValidateReceiveRequest(path, fixture.declared_size);
    if (request_result.error() != fixture.expected_request_error) {
      std::cerr << "FAILED: " << fixture.name << '/' << fixture.reason
                << " request validation mismatch\n";
      ++failures;
    }
    Expect((path_result.path() != nullptr) == path_result.ok(),
           "path capability exists only after validation");
    Expect((request_result.request() != nullptr) == request_result.ok(),
           "request capability exists only after validation");
    if (path_result.ok()) {
      const std::string original(reinterpret_cast<const char*>(path.data()),
                                 path.size());
      Expect(path_result.path() != nullptr && path_result.path()->utf8() == original,
             "accepted path bytes are not normalized again");
    }
  }

  Expect(storage_cases == kStorageFixtureCaseCount,
         "all storage fixture cases execute validation");
  Expect(xt028_cases == kXt028FixtureCaseCount,
         "all transfer-owned fixture cases are classified");
  Expect(xt033_cases == kXt033FixtureCaseCount,
         "all multi-file fixture cases are classified");
  Expect(kManifestFixtureSha256.size() == 64,
         "fixture table records the corpus digest");
}

void TestAdditionalRequestBounds() {
  const RequestValidationResult local_limit =
      ValidateReceiveRequest(Bytes("file.bin"), 5, 4);
  Expect(local_limit.error() == ValidationError::kDeclaredSizeLimit,
         "local file limit lowers the protocol hard limit");
  const RequestValidationResult hard_limit =
      ValidateReceiveRequest(Bytes("file.bin"), kMaxFileBytes, kMaxFileBytes + 1);
  Expect(hard_limit.ok(), "caller cannot raise the protocol hard limit");
  const std::array<std::uint8_t, 3> del_path{'a', 0x7fU, 'b'};
  Expect(ValidateReceivePath(del_path).error() == ValidationError::kPathC0Control,
         "DEL is rejected as a control");
}

void TestSuccessfulAndEmptyCommit() {
  auto budget = std::make_shared<TemporaryBudget>(32);
  FakePlatformBackend platform;
  auto verifier = std::make_unique<FakeVerifier>();
  FakeVerifier* const verifier_view = verifier.get();
  ReceiveTransaction transaction(Request("docs/file.bin", 5), budget,
                                 std::move(verifier), platform);

  Expect(transaction.Begin().ok(), "transaction begins");
  Expect(budget->reserved_bytes() == 5, "begin reserves declared bytes");
  Expect(transaction.Write(Bytes("he")).ok(), "first chunk writes");
  Expect(transaction.Write(Bytes("llo")).ok(), "second chunk writes");
  Expect(transaction.SealAndCommit().ok(), "verified file commits");
  Expect(transaction.state() == TransactionState::kCommitted,
         "successful commit is terminal");
  Expect(transaction.received_bytes() == 5, "received count reaches declaration");
  Expect(budget->reserved_bytes() == 0, "commit releases temporary budget");
  Expect(verifier_view->sealed && verifier_view->seal_calls == 1,
         "integrity verifier seals before commit");
  Expect(platform.flush_calls == 1 && platform.commit_calls == 1 &&
             platform.cleanup_calls == 0,
         "success flushes and commits without cleanup");
  Expect(
      std::string(platform.destination.begin(), platform.destination.end()) == "hello",
      "destination receives exact bytes");

  FakePlatformBackend empty_platform;
  ReceiveTransaction empty(Request("empty.bin", 0), budget,
                           std::make_unique<FakeVerifier>(), empty_platform);
  Expect(empty.Begin().ok() && empty.SealAndCommit().ok(),
         "empty file commits without a write");
  Expect(empty_platform.write_calls == 0 && budget->reserved_bytes() == 0,
         "empty file leaves no reservation");
}

void TestShortExtraAndPartialWrites() {
  {
    auto budget = std::make_shared<TemporaryBudget>(8);
    FakePlatformBackend platform;
    ReceiveTransaction transaction(Request("short.bin", 3), budget,
                                   std::make_unique<FakeVerifier>(), platform);
    Expect(transaction.Begin().ok() && transaction.Write(Bytes("ab")).ok(),
           "short-data transaction receives a prefix");
    const TransactionResult result = transaction.SealAndCommit();
    Expect(result.error == TransactionError::kDataTooShort,
           "seal rejects short content");
    Expect(platform.flush_calls == 0 && platform.cleanup_calls == 1 &&
               budget->reserved_bytes() == 0,
           "short content cleans before flush");
  }

  {
    auto budget = std::make_shared<TemporaryBudget>(8);
    FakePlatformBackend platform;
    ReceiveTransaction transaction(Request("extra.bin", 2), budget,
                                   std::make_unique<FakeVerifier>(), platform);
    Expect(transaction.Begin().ok(), "extra-data transaction begins");
    const TransactionResult result = transaction.Write(Bytes("abc"));
    Expect(result.error == TransactionError::kDataTooLarge,
           "write rejects bytes beyond declaration");
    Expect(platform.write_calls == 0 && platform.cleanup_calls == 1,
           "extra bytes never reach the backend");
  }

  {
    auto budget = std::make_shared<TemporaryBudget>(8);
    FakePlatformBackend platform;
    platform.short_write = true;
    ReceiveTransaction transaction(Request("partial.bin", 3), budget,
                                   std::make_unique<FakeVerifier>(), platform);
    Expect(transaction.Begin().ok(), "partial-write transaction begins");
    const TransactionResult result = transaction.Write(Bytes("abc"));
    Expect(result.error == TransactionError::kWriteFailed,
           "backend short write is terminal");
    Expect(platform.cleanup_calls == 1 && budget->reserved_bytes() == 0,
           "backend short write cleans and releases budget");
  }
}

void TestBudgetSpacePermissionAndOpenCleanup() {
  {
    auto budget = std::make_shared<TemporaryBudget>(3);
    FakePlatformBackend platform;
    ReceiveTransaction transaction(Request("budget.bin", 4), budget,
                                   std::make_unique<FakeVerifier>(), platform);
    const TransactionResult result = transaction.Begin();
    Expect(result.error == TransactionError::kTemporaryBudgetExceeded,
           "shared budget rejects over-reservation");
    Expect(platform.create_calls == 0 && budget->reserved_bytes() == 0,
           "budget rejection precedes platform access");
  }

  {
    auto budget = std::make_shared<TemporaryBudget>(8);
    FakePlatformBackend platform;
    platform.create_error = PlatformError::kNoSpace;
    ReceiveTransaction transaction(Request("space.bin", 4), budget,
                                   std::make_unique<FakeVerifier>(), platform);
    const TransactionResult result = transaction.Begin();
    Expect(result.error == TransactionError::kNoSpace &&
               result.platform_error == PlatformError::kNoSpace,
           "platform no-space remains machine-readable");
    Expect(budget->reserved_bytes() == 0, "failed open releases reservation");
  }

  {
    auto budget = std::make_shared<TemporaryBudget>(8);
    FakePlatformBackend platform;
    platform.create_error = PlatformError::kPermissionDenied;
    platform.return_handle_on_create_error = true;
    ReceiveTransaction transaction(Request("denied.bin", 4), budget,
                                   std::make_unique<FakeVerifier>(), platform);
    const TransactionResult result = transaction.Begin();
    Expect(result.error == TransactionError::kOpenFailed &&
               result.platform_error == PlatformError::kPermissionDenied,
           "permission failure is preserved");
    Expect(platform.cleanup_calls == 1 && !platform.temporary_open &&
               budget->reserved_bytes() == 0,
           "failed create cannot leak a returned handle");
  }
}

void TestIntegrityFlushCommitAndCleanupFailures() {
  {
    auto budget = std::make_shared<TemporaryBudget>(8);
    FakePlatformBackend platform;
    auto verifier = std::make_unique<FakeVerifier>();
    verifier->seal_succeeds = false;
    ReceiveTransaction transaction(Request("mismatch.bin", 1), budget,
                                   std::move(verifier), platform);
    Expect(transaction.Begin().ok() && transaction.Write(Bytes("x")).ok(),
           "hash-mismatch transaction receives exact bytes");
    const TransactionResult result = transaction.SealAndCommit();
    Expect(result.error == TransactionError::kIntegrityFailed &&
               platform.flush_calls == 0 && platform.cleanup_calls == 1,
           "hash mismatch cleans before flush and commit");
  }

  {
    auto budget = std::make_shared<TemporaryBudget>(8);
    FakePlatformBackend platform;
    platform.flush_error = PlatformError::kIoFailure;
    platform.cleanup_error = PlatformError::kPermissionDenied;
    ReceiveTransaction transaction(Request("flush.bin", 1), budget,
                                   std::make_unique<FakeVerifier>(), platform);
    Expect(transaction.Begin().ok() && transaction.Write(Bytes("x")).ok(),
           "flush-failure transaction receives bytes");
    const TransactionResult result = transaction.SealAndCommit();
    Expect(result.error == TransactionError::kFlushFailed &&
               result.cleanup_error == PlatformError::kPermissionDenied &&
               result.cleanup_unresolved,
           "flush failure retains cleanup failure");
    Expect(transaction.state() == TransactionState::kCleanupUnresolved &&
               budget->reserved_bytes() == 1 && budget->unresolved_bytes() == 1 &&
               budget->unresolved_files() == 1 && platform.owned_orphans.size() == 1,
           "unresolved cleanup remains owned and budgeted");
  }

  {
    auto budget = std::make_shared<TemporaryBudget>(8);
    FakePlatformBackend platform;
    platform.commit_disposition = PlatformCommitDisposition::kNotCommitted;
    platform.commit_error = PlatformError::kDestinationExists;
    ReceiveTransaction transaction(Request("existing.bin", 1), budget,
                                   std::make_unique<FakeVerifier>(), platform);
    Expect(transaction.Begin().ok() && transaction.Write(Bytes("x")).ok(),
           "collision transaction receives exact bytes");
    const TransactionResult result = transaction.SealAndCommit();
    Expect(result.error == TransactionError::kCommitFailed &&
               result.platform_error == PlatformError::kDestinationExists &&
               platform.cleanup_calls == 1,
           "destination collision cleans temporary output");
  }
}

void TestOutcomeUncertainRetainsAccounting() {
  auto budget = std::make_shared<TemporaryBudget>(8);
  FakePlatformBackend platform;
  platform.commit_disposition = PlatformCommitDisposition::kOutcomeUncertain;
  platform.commit_error = PlatformError::kIoFailure;
  ReceiveTransaction transaction(Request("uncertain.bin", 1), budget,
                                 std::make_unique<FakeVerifier>(), platform);
  Expect(transaction.Begin().ok() && transaction.Write(Bytes("x")).ok(),
         "uncertain transaction receives exact bytes");
  const TransactionResult result = transaction.SealAndCommit();
  Expect(result.error == TransactionError::kOutcomeUncertain &&
             result.platform_error == PlatformError::kIoFailure,
         "uncertain commit is an explicit terminal result");
  Expect(transaction.state() == TransactionState::kOutcomeUncertain &&
             platform.cleanup_calls == 0 && platform.owned_orphans.size() == 1 &&
             budget->reserved_bytes() == 1 && budget->unresolved_bytes() == 1,
         "backend owns uncertainty while budget remains fail-closed");
}

void TestConcurrentReservationAndIdempotentAbort() {
  constexpr std::size_t kTransactions = 8;
  auto budget = std::make_shared<TemporaryBudget>(64);
  std::array<std::unique_ptr<FakePlatformBackend>, kTransactions> platforms;
  std::array<std::unique_ptr<ReceiveTransaction>, kTransactions> transactions;
  for (std::size_t index = 0; index < kTransactions; ++index) {
    platforms[index] = std::make_unique<FakePlatformBackend>();
    transactions[index] = std::make_unique<ReceiveTransaction>(
        Request("parallel/file.bin", 16), budget, std::make_unique<FakeVerifier>(),
        *platforms[index]);
  }

  std::atomic<bool> start{false};
  std::atomic<std::size_t> begun{0};
  std::array<std::thread, kTransactions> workers;
  for (std::size_t index = 0; index < kTransactions; ++index) {
    workers[index] = std::thread([&, index] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      if (transactions[index]->Begin().ok()) {
        begun.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (std::thread& worker : workers) {
    worker.join();
  }

  Expect(begun.load(std::memory_order_relaxed) == 4,
         "concurrent reservations cannot exceed budget");
  Expect(budget->reserved_bytes() == 64, "concurrent reservations account exact bytes");
  for (std::size_t index = 0; index < kTransactions; ++index) {
    const TransactionResult first = transactions[index]->Abort();
    const TransactionResult second = transactions[index]->Abort();
    Expect(first.error == second.error, "repeated abort returns a stable result");
    Expect(platforms[index]->cleanup_calls <= 1,
           "repeated abort performs at most one cleanup");
  }
  Expect(budget->reserved_bytes() == 0, "aborting transactions releases reservations");
}

void TestRealFilesystemCommitAndCollision() {
  ScopedDirectory root("commit");
  FilesystemBackendOpenResult opened = OpenFilesystemBackend(root.path().string());
#if defined(_WIN32)
  Expect(!opened.ok() && opened.error == PlatformError::kUnsupported,
         "Windows backend fails closed until reparse-safe support exists");
  return;
#else
  Expect(opened.ok(), "filesystem backend opens a real root");
  if (!opened.ok()) {
    return;
  }
  FilesystemBackendOpenResult competing = OpenFilesystemBackend(root.path().string());
  Expect(!competing.ok() && competing.error == PlatformError::kBusy,
         "one process owns destination cleanup state");

  auto budget = std::make_shared<TemporaryBudget>(64);
  const std::filesystem::path destination = root.path() / "nested" / "file.bin";
  ReceiveTransaction transaction(Request("nested/file.bin", 5), budget,
                                 std::make_unique<FakeVerifier>(), *opened.backend);
  Expect(transaction.Begin().ok() && transaction.Write(Bytes("hello")).ok(),
         "real transaction stages exact bytes");
  Expect(!std::filesystem::exists(destination),
         "incomplete data is not destination-visible");
  Expect(transaction.SealAndCommit().ok(), "real transaction commits atomically");
  Expect(ReadFile(destination) == "hello", "committed destination has exact content");

  ReceiveTransaction collision(Request("nested/file.bin", 3), budget,
                               std::make_unique<FakeVerifier>(), *opened.backend);
  Expect(collision.Begin().ok() && collision.Write(Bytes("new")).ok(),
         "collision transaction stages separately");
  const TransactionResult collision_result = collision.SealAndCommit();
  Expect(collision_result.error == TransactionError::kCommitFailed &&
             collision_result.platform_error == PlatformError::kDestinationExists,
         "existing destination rejects no-replace commit");
  Expect(ReadFile(destination) == "hello", "collision never replaces destination");
#endif
}

void TestRealFilesystemRejectsPermissiveLockMode() {
#if !defined(_WIN32)
  ScopedDirectory root("lock-mode");
  const std::filesystem::path temporary_directory = root.path() / ".xnn-transfer-tmp";
  std::error_code error;
  std::filesystem::create_directory(temporary_directory, error);
  Expect(!error, "lock-mode temporary directory is created");
  if (error) {
    return;
  }
  std::filesystem::permissions(temporary_directory, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, error);
  Expect(!error, "lock-mode temporary directory is private");
  if (error) {
    return;
  }

  const std::filesystem::path lock = temporary_directory / ".lock";
  WriteFile(lock, "");
  std::filesystem::permissions(
      lock,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
          std::filesystem::perms::group_read | std::filesystem::perms::others_read,
      std::filesystem::perm_options::replace, error);
  Expect(!error, "lock-mode fixture is made permissive");
  if (error) {
    return;
  }

  const FilesystemBackendOpenResult opened =
      OpenFilesystemBackend(root.path().string());
  Expect(!opened.ok() && opened.error == PlatformError::kInvalidRoot,
         "preexisting lock mode must be exactly 0600");
#endif
}

void TestRealFilesystemLinkContainment() {
#if !defined(_WIN32)
  ScopedDirectory root("links");
  ScopedDirectory outside("outside");
  FilesystemBackendOpenResult opened = OpenFilesystemBackend(root.path().string());
  Expect(opened.ok(), "link test backend opens");
  if (!opened.ok()) {
    return;
  }
  auto budget = std::make_shared<TemporaryBudget>(64);

  std::error_code error;
  std::filesystem::create_directory_symlink(outside.path(),
                                            root.path() / "linked-parent", error);
  Expect(!error, "parent symlink fixture is created");
  if (!error) {
    ReceiveTransaction parent_link(Request("linked-parent/escape.bin", 1), budget,
                                   std::make_unique<FakeVerifier>(), *opened.backend);
    Expect(parent_link.Begin().ok() && parent_link.Write(Bytes("x")).ok(),
           "parent-link transaction stages safely");
    const TransactionResult result = parent_link.SealAndCommit();
    Expect(result.error == TransactionError::kCommitFailed,
           "parent symlink rejects commit");
    Expect(!std::filesystem::exists(outside.path() / "escape.bin"),
           "parent symlink cannot escape the root");
  }

  WriteFile(outside.path() / "target.bin", "outside");
  error.clear();
  std::filesystem::create_symlink(outside.path() / "target.bin",
                                  root.path() / "destination-link", error);
  Expect(!error, "destination symlink fixture is created");
  if (!error) {
    ReceiveTransaction destination_link(Request("destination-link", 1), budget,
                                        std::make_unique<FakeVerifier>(),
                                        *opened.backend);
    Expect(destination_link.Begin().ok() && destination_link.Write(Bytes("x")).ok(),
           "destination-link transaction stages safely");
    const TransactionResult result = destination_link.SealAndCommit();
    Expect(result.error == TransactionError::kCommitFailed &&
               result.platform_error == PlatformError::kDestinationExists,
           "destination symlink is an existing collision");
    Expect(ReadFile(outside.path() / "target.bin") == "outside",
           "destination symlink target is unchanged");
  }

  error.clear();
  std::filesystem::create_directory_symlink(root.path(), outside.path() / "root-link",
                                            error);
  Expect(!error, "root symlink fixture is created");
  if (!error) {
    const FilesystemBackendOpenResult symlink_root =
        OpenFilesystemBackend((outside.path() / "root-link").string());
    Expect(!symlink_root.ok() && symlink_root.error == PlatformError::kInvalidRoot,
           "symlink destination root is rejected");
  }
#endif
}

void TestRealFilesystemSymlinkRace() {
#if !defined(_WIN32)
  ScopedDirectory root("race");
  ScopedDirectory outside("race-outside");
  FilesystemBackendOpenResult opened = OpenFilesystemBackend(root.path().string());
  Expect(opened.ok(), "race test backend opens");
  if (!opened.ok()) {
    return;
  }

  std::error_code error;
  std::filesystem::create_directory(root.path() / "race", error);
  Expect(!error, "race parent is created");
  auto budget = std::make_shared<TemporaryBudget>(16);
  ReceiveTransaction transaction(Request("race/file.bin", 1), budget,
                                 std::make_unique<FakeVerifier>(), *opened.backend);
  Expect(transaction.Begin().ok() && transaction.Write(Bytes("x")).ok(),
         "race transaction stages before mutation");

  std::atomic<bool> start{false};
  std::atomic<bool> stop{false};
  std::thread racer([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    while (!stop.load(std::memory_order_acquire)) {
      std::error_code ignored;
      std::filesystem::remove(root.path() / "race", ignored);
      std::filesystem::create_directory_symlink(outside.path(), root.path() / "race",
                                                ignored);
      std::filesystem::remove(root.path() / "race", ignored);
      std::filesystem::create_directory(root.path() / "race", ignored);
    }
  });
  start.store(true, std::memory_order_release);
  const TransactionResult result = transaction.SealAndCommit();
  stop.store(true, std::memory_order_release);
  racer.join();

  Expect(result.ok() || result.error == TransactionError::kCommitFailed,
         "symlink race either commits beneath root or fails closed");
  Expect(!std::filesystem::exists(outside.path() / "file.bin"),
         "symlink race cannot create outside the root");
#endif
}

void TestRestartCleanupAndDestructorAbort() {
#if !defined(_WIN32)
  ScopedDirectory root("restart");
  {
    FilesystemBackendOpenResult opened = OpenFilesystemBackend(root.path().string());
    Expect(opened.ok(), "restart test backend opens");
    if (!opened.ok()) {
      return;
    }
    auto budget = std::make_shared<TemporaryBudget>(8);
    ReceiveTransaction transaction(Request("aborted.bin", 1), budget,
                                   std::make_unique<FakeVerifier>(), *opened.backend);
    Expect(transaction.Begin().ok() && transaction.Write(Bytes("x")).ok(),
           "destructor-abort transaction stages");
  }
  Expect(!std::filesystem::exists(root.path() / "aborted.bin"),
         "destructor abort leaves no destination");

  const std::filesystem::path stale = root.path() / ".xnn-transfer-tmp" / "part-stale";
  WriteFile(stale, "stale");
  Expect(std::filesystem::exists(stale), "stale restart fixture exists");
  FilesystemBackendOpenResult reopened = OpenFilesystemBackend(root.path().string());
  Expect(reopened.ok(), "backend reopens after prior owner exits");
  Expect(!std::filesystem::exists(stale), "startup removes stale temporary files");
#endif
}

}  // namespace

int main() {
  TestManifestFixtureOwnershipAndPathValidation();
  TestAdditionalRequestBounds();
  TestSuccessfulAndEmptyCommit();
  TestShortExtraAndPartialWrites();
  TestBudgetSpacePermissionAndOpenCleanup();
  TestIntegrityFlushCommitAndCleanupFailures();
  TestOutcomeUncertainRetainsAccounting();
  TestConcurrentReservationAndIdempotentAbort();
  TestRealFilesystemCommitAndCollision();
  TestRealFilesystemRejectsPermissiveLockMode();
  TestRealFilesystemLinkContainment();
  TestRealFilesystemSymlinkRace();
  TestRestartCleanupAndDestructorAbort();

  if (failures != 0) {
    std::cerr << failures << " storage assertion(s) failed\n";
    return 1;
  }
  std::cout << "Storage tests passed with all 66 XT-011 cases classified.\n";
  return 0;
}
