#include "xnn_transfer/core/storage/storage.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <sys/acl.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <AccCtrl.h>
#include <Aclapi.h>
#include <Windows.h>
#include <winioctl.h>
#endif

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

[[nodiscard]] std::string PathUtf8(const std::filesystem::path& path) {
  const std::u8string encoded = path.u8string();
  return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

void WriteFile(const std::filesystem::path& path, const std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  Expect(output.good(), "test fixture file is written");
}

#if defined(_WIN32)
struct MountPointReparseData {
  DWORD tag;
  WORD data_length;
  WORD reserved;
  WORD substitute_name_offset;
  WORD substitute_name_length;
  WORD print_name_offset;
  WORD print_name_length;
  wchar_t path_buffer[1];
};

[[nodiscard]] bool CreateJunction(const std::filesystem::path& junction,
                                  const std::filesystem::path& target) {
  std::error_code error;
  const std::filesystem::path absolute_target =
      std::filesystem::absolute(target, error);
  if (error || !std::filesystem::create_directory(junction, error) || error) {
    return false;
  }

  const std::wstring target_name = absolute_target.native();
  const std::wstring substitute_name = L"\\??\\" + target_name;
  const std::size_t substitute_bytes = substitute_name.size() * sizeof(wchar_t);
  const std::size_t print_bytes = target_name.size() * sizeof(wchar_t);
  const std::size_t path_bytes =
      substitute_bytes + sizeof(wchar_t) + print_bytes + sizeof(wchar_t);
  const std::size_t buffer_size =
      offsetof(MountPointReparseData, path_buffer) + path_bytes;
  if (buffer_size > MAXIMUM_REPARSE_DATA_BUFFER_SIZE ||
      buffer_size - 8U > static_cast<std::size_t>(std::numeric_limits<WORD>::max())) {
    std::filesystem::remove(junction, error);
    return false;
  }

  std::vector<std::byte> buffer(buffer_size);
  auto* const reparse = reinterpret_cast<MountPointReparseData*>(buffer.data());
  reparse->tag = IO_REPARSE_TAG_MOUNT_POINT;
  reparse->data_length = static_cast<WORD>(buffer_size - 8U);
  reparse->reserved = 0;
  reparse->substitute_name_offset = 0;
  reparse->substitute_name_length = static_cast<WORD>(substitute_bytes);
  reparse->print_name_offset = static_cast<WORD>(substitute_bytes + sizeof(wchar_t));
  reparse->print_name_length = static_cast<WORD>(print_bytes);
  std::memcpy(reparse->path_buffer, substitute_name.data(), substitute_bytes);
  std::memcpy(reinterpret_cast<std::byte*>(reparse->path_buffer) + substitute_bytes +
                  sizeof(wchar_t),
              target_name.data(), print_bytes);

  const HANDLE handle =
      ::CreateFileW(junction.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    std::filesystem::remove(junction, error);
    return false;
  }
  DWORD bytes_returned = 0;
  const BOOL set = ::DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, reparse,
                                     static_cast<DWORD>(buffer_size), nullptr, 0,
                                     &bytes_returned, nullptr);
  const BOOL closed = ::CloseHandle(handle);
  if (set == FALSE || closed == FALSE) {
    std::filesystem::remove(junction, error);
    return false;
  }
  return true;
}

[[nodiscard]] bool SetPermissiveDacl(const std::filesystem::path& path) {
  std::array<std::byte, SECURITY_MAX_SID_SIZE> sid_storage{};
  DWORD sid_size = static_cast<DWORD>(sid_storage.size());
  auto* const everyone_sid = reinterpret_cast<PSID>(sid_storage.data());
  if (::CreateWellKnownSid(WinWorldSid, nullptr, everyone_sid, &sid_size) == FALSE) {
    return false;
  }

  EXPLICIT_ACCESSW access{};
  access.grfAccessPermissions = GENERIC_ALL;
  access.grfAccessMode = SET_ACCESS;
  access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
  access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
  access.Trustee.ptstrName = static_cast<LPWSTR>(everyone_sid);
  PACL acl = nullptr;
  if (::SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) {
    return false;
  }
  const DWORD result = ::SetNamedSecurityInfoW(
      const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
      acl, nullptr);
  ::LocalFree(acl);
  return result == ERROR_SUCCESS;
}
#endif

#if defined(__APPLE__)
[[nodiscard]] bool SetPermissiveAcl(const std::filesystem::path& path) {
  constexpr const char* kAclText =
      "!#acl 1\n"
      "group:ABCDEFAB-CDEF-ABCD-EFAB-CDEF0000000C:everyone:12:"
      "allow,file_inherit,directory_inherit:"
      "write,delete,append,delete_child\n";
  acl_t const acl = ::acl_from_text(kAclText);
  if (acl == nullptr) {
    return false;
  }
  const int result = ::acl_set_file(path.c_str(), ACL_TYPE_EXTENDED, acl);
  static_cast<void>(::acl_free(acl));
  return result == 0;
}
#endif

void TestManifestFixtureOwnershipAndPathValidation() {
  static_assert(kManifestFixtureCases.size() == 70);
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
  Expect(ValidateReceivePath(Bytes(".xnn-transfer-tmp/part-user")).error() ==
             ValidationError::kPathReservedComponent,
         "backend temporary namespace is not a destination");
  Expect(ValidateReceivePath(Bytes(".XNN-Transfer-Tmp/part-user")).error() ==
             ValidationError::kPathReservedComponent,
         "reserved namespace rejection covers Windows case aliases");
  Expect(ValidateReceivePath(Bytes("safe/.xnn-transfer-tmp")).ok(),
         "reserved name is limited to the root component");
}

void TestPortableWindowsPathAliases() {
  Expect(ValidateReceivePath(Bytes("CON")).error() ==
             ValidationError::kPathReservedComponent,
         "Windows reserved path aliases must fail before filesystem access");

  constexpr std::array<std::string_view, 22> aliases{
      "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
      "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
      "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
  };
  for (const std::string_view alias : aliases) {
    Expect(ValidateReceivePath(Bytes(alias)).error() ==
               ValidationError::kPathReservedComponent,
           "every Windows reserved device name is rejected");

    std::string with_extension(alias);
    with_extension.append(".txt");
    Expect(ValidateReceivePath(Bytes(with_extension)).error() ==
               ValidationError::kPathReservedComponent,
           "reserved device names remain reserved with extensions");

    std::string lowercase(alias);
    for (char& character : lowercase) {
      if (character >= 'A' && character <= 'Z') {
        character = static_cast<char>(character - 'A' + 'a');
      }
    }
    Expect(ValidateReceivePath(Bytes(lowercase)).error() ==
               ValidationError::kPathReservedComponent,
           "reserved device names use ASCII-insensitive comparison");
  }

  for (const std::string_view path :
       {"name.", "name ", "docs/name.", "docs/name ", "docs./name"}) {
    Expect(ValidateReceivePath(Bytes(path)).error() ==
               ValidationError::kPathReservedComponent,
           "trailing dots and spaces are rejected in every component");
  }
  for (const std::string_view path :
       {"docs/a.b", "docs/a b", "console.txt", "com0", "com10", "lpt0", "lpt10"}) {
    Expect(ValidateReceivePath(Bytes(path)).ok(),
           "ordinary interior dots, spaces, and alias prefixes remain valid");
  }
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
  FilesystemBackendOpenResult opened = OpenFilesystemBackend(PathUtf8(root.path()));
  Expect(opened.ok(), "filesystem backend opens a real root");
  if (!opened.ok()) {
    return;
  }
  FilesystemBackendOpenResult competing = OpenFilesystemBackend(PathUtf8(root.path()));
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

void TestRealFilesystemRejectsPermissiveDirectories() {
#if !defined(_WIN32)
  {
    ScopedDirectory root("root-mode");
    std::error_code error;
    std::filesystem::permissions(
        root.path(),
        std::filesystem::perms::owner_all | std::filesystem::perms::group_write,
        std::filesystem::perm_options::replace, error);
    Expect(!error, "permissive root fixture is created");
    const FilesystemBackendOpenResult opened =
        OpenFilesystemBackend(PathUtf8(root.path()));
    Expect(!opened.ok() && opened.error == PlatformError::kInvalidRoot,
           "group-writable destination root is rejected");
  }

  {
    ScopedDirectory root("parent-mode");
    FilesystemBackendOpenResult opened = OpenFilesystemBackend(PathUtf8(root.path()));
    Expect(opened.ok(), "intermediate-mode backend opens");
    if (!opened.ok()) {
      return;
    }

    const std::filesystem::path intermediate = root.path() / "permissive";
    std::error_code error;
    std::filesystem::create_directory(intermediate, error);
    std::filesystem::permissions(
        intermediate,
        std::filesystem::perms::owner_all | std::filesystem::perms::group_write,
        std::filesystem::perm_options::replace, error);
    Expect(!error, "permissive intermediate fixture is created");

    auto budget = std::make_shared<TemporaryBudget>(8);
    ReceiveTransaction transaction(Request("permissive/file.bin", 1), budget,
                                   std::make_unique<FakeVerifier>(), *opened.backend);
    Expect(transaction.Begin().ok() && transaction.Write(Bytes("x")).ok(),
           "permissive-intermediate transaction stages safely");
    const TransactionResult result = transaction.SealAndCommit();
    Expect(result.error == TransactionError::kCommitFailed &&
               result.platform_error == PlatformError::kInvalidRoot,
           "group-writable intermediate is rejected");
    Expect(!std::filesystem::exists(intermediate / "file.bin"),
           "permissive intermediate receives no destination file");
  }
#endif
}

void TestRealFilesystemRejectsMacosExtendedAcls() {
#if defined(__APPLE__)
  {
    ScopedDirectory root("root-acl");
    Expect(SetPermissiveAcl(root.path()), "permissive root ACL fixture is created");
    const FilesystemBackendOpenResult opened =
        OpenFilesystemBackend(PathUtf8(root.path()));
    Expect(!opened.ok() && opened.error == PlatformError::kInvalidRoot,
           "destination root with an extended ACL is rejected");
  }

  {
    ScopedDirectory root("temporary-acl");
    const std::filesystem::path temporary_directory = root.path() / ".xnn-transfer-tmp";
    std::error_code error;
    std::filesystem::create_directory(temporary_directory, error);
    std::filesystem::permissions(temporary_directory, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    Expect(!error && SetPermissiveAcl(temporary_directory),
           "permissive temporary-directory ACL fixture is created");
    const FilesystemBackendOpenResult opened =
        OpenFilesystemBackend(PathUtf8(root.path()));
    Expect(!opened.ok() && opened.error == PlatformError::kInvalidRoot,
           "temporary directory with an extended ACL is rejected");
  }

  {
    ScopedDirectory root("lock-acl");
    {
      FilesystemBackendOpenResult opened = OpenFilesystemBackend(PathUtf8(root.path()));
      Expect(opened.ok(), "backend creates POSIX lock metadata");
    }
    const std::filesystem::path lock = root.path() / ".xnn-transfer-tmp" / ".lock";
    Expect(SetPermissiveAcl(lock), "permissive lock ACL fixture is created");
    const FilesystemBackendOpenResult opened =
        OpenFilesystemBackend(PathUtf8(root.path()));
    Expect(!opened.ok() && opened.error == PlatformError::kInvalidRoot,
           "lock with an extended ACL is rejected");
  }

  {
    ScopedDirectory root("parent-acl");
    FilesystemBackendOpenResult opened = OpenFilesystemBackend(PathUtf8(root.path()));
    Expect(opened.ok(), "parent-ACL backend opens");
    if (!opened.ok()) {
      return;
    }
    const std::filesystem::path parent = root.path() / "shared";
    std::error_code error;
    std::filesystem::create_directory(parent, error);
    Expect(!error && SetPermissiveAcl(parent),
           "permissive destination-parent ACL fixture is created");

    auto budget = std::make_shared<TemporaryBudget>(8);
    ReceiveTransaction transaction(Request("shared/file.bin", 1), budget,
                                   std::make_unique<FakeVerifier>(), *opened.backend);
    Expect(transaction.Begin().ok() && transaction.Write(Bytes("x")).ok(),
           "parent-ACL transaction stages safely");
    const TransactionResult result = transaction.SealAndCommit();
    Expect(result.error == TransactionError::kCommitFailed &&
               result.platform_error == PlatformError::kInvalidRoot,
           "destination parent with an extended ACL is rejected");
  }
#endif
}

void TestRealFilesystemLinkContainment() {
  ScopedDirectory root("links");
  ScopedDirectory outside("outside");
  FilesystemBackendOpenResult opened = OpenFilesystemBackend(PathUtf8(root.path()));
  Expect(opened.ok(), "link test backend opens");
  if (!opened.ok()) {
    return;
  }
  auto budget = std::make_shared<TemporaryBudget>(64);

  std::error_code error;
#if defined(_WIN32)
  const bool parent_link_created =
      CreateJunction(root.path() / "linked-parent", outside.path());
  Expect(parent_link_created, "parent junction fixture is created");
#else
  std::filesystem::create_directory_symlink(outside.path(),
                                            root.path() / "linked-parent", error);
  Expect(!error, "parent symlink fixture is created");
#endif
  if (
#if defined(_WIN32)
      parent_link_created
#else
      !error
#endif
  ) {
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

#if defined(_WIN32)
  WriteFile(outside.path() / "marker.bin", "outside");
  const bool destination_link_created =
      CreateJunction(root.path() / "destination-link", outside.path());
  Expect(destination_link_created, "destination junction fixture is created");
#else
  WriteFile(outside.path() / "target.bin", "outside");
  error.clear();
  std::filesystem::create_symlink(outside.path() / "target.bin",
                                  root.path() / "destination-link", error);
  Expect(!error, "destination symlink fixture is created");
#endif
  if (
#if defined(_WIN32)
      destination_link_created
#else
      !error
#endif
  ) {
    ReceiveTransaction destination_link(Request("destination-link", 1), budget,
                                        std::make_unique<FakeVerifier>(),
                                        *opened.backend);
    Expect(destination_link.Begin().ok() && destination_link.Write(Bytes("x")).ok(),
           "destination-link transaction stages safely");
    const TransactionResult result = destination_link.SealAndCommit();
    Expect(result.error == TransactionError::kCommitFailed &&
               result.platform_error == PlatformError::kDestinationExists,
           "destination symlink is an existing collision");
#if defined(_WIN32)
    Expect(ReadFile(outside.path() / "marker.bin") == "outside",
           "destination junction target is unchanged");
#else
    Expect(ReadFile(outside.path() / "target.bin") == "outside",
           "destination symlink target is unchanged");
#endif
  }

#if defined(_WIN32)
  const bool root_link_created =
      CreateJunction(outside.path() / "root-link", root.path());
  Expect(root_link_created, "root junction fixture is created");
#else
  error.clear();
  std::filesystem::create_directory_symlink(root.path(), outside.path() / "root-link",
                                            error);
  Expect(!error, "root symlink fixture is created");
#endif
  if (
#if defined(_WIN32)
      root_link_created
#else
      !error
#endif
  ) {
    const FilesystemBackendOpenResult symlink_root =
        OpenFilesystemBackend(PathUtf8(outside.path() / "root-link"));
    Expect(!symlink_root.ok() && symlink_root.error == PlatformError::kInvalidRoot,
           "symlink destination root is rejected");
  }
}

void TestRealFilesystemSymlinkRace() {
  ScopedDirectory root("race");
  ScopedDirectory outside("race-outside");
  FilesystemBackendOpenResult opened = OpenFilesystemBackend(PathUtf8(root.path()));
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
#if defined(_WIN32)
      static_cast<void>(CreateJunction(root.path() / "race", outside.path()));
#else
      std::filesystem::create_directory_symlink(outside.path(), root.path() / "race",
                                                ignored);
#endif
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
}

void TestRestartCleanupAndDestructorAbort() {
  ScopedDirectory root("restart");
  {
    FilesystemBackendOpenResult opened = OpenFilesystemBackend(PathUtf8(root.path()));
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
#if defined(_WIN32)
  ScopedDirectory outside("restart-outside");
  WriteFile(outside.path() / "marker.bin", "outside");
  const std::filesystem::path stale_junction =
      root.path() / ".xnn-transfer-tmp" / "part-stale-junction";
  const bool stale_junction_created = CreateJunction(stale_junction, outside.path());
  Expect(stale_junction_created, "stale junction fixture exists");
#endif
  FilesystemBackendOpenResult reopened = OpenFilesystemBackend(PathUtf8(root.path()));
  Expect(reopened.ok(), "backend reopens after prior owner exits");
  Expect(!std::filesystem::exists(stale), "startup removes stale temporary files");
#if defined(_WIN32)
  Expect(!std::filesystem::exists(stale_junction),
         "startup removes a stale junction without following it");
  Expect(ReadFile(outside.path() / "marker.bin") == "outside",
         "startup stale cleanup preserves the junction target");
#endif
}

void TestMissingRootClassification() {
  ScopedDirectory parent("missing-root");
  const FilesystemBackendOpenResult opened =
      OpenFilesystemBackend(PathUtf8(parent.path() / "missing"));
  Expect(!opened.ok() && opened.error == PlatformError::kInvalidRoot,
         "missing destination root is classified as invalid");
}

void TestWindowsBoundedStaleCleanupProgress() {
#if defined(_WIN32)
  ScopedDirectory root("windows-stale-batch");
  {
    FilesystemBackendOpenResult opened = OpenFilesystemBackend(PathUtf8(root.path()));
    Expect(opened.ok(), "stale-batch backend creates private metadata");
  }
  const std::filesystem::path temporary_directory = root.path() / ".xnn-transfer-tmp";
  constexpr std::size_t kStaleFiles = 4'097;
  for (std::size_t index = 0; index < kStaleFiles; ++index) {
    WriteFile(temporary_directory / ("part-batch-" + std::to_string(index)), "");
  }

  FilesystemBackendOpenResult first = OpenFilesystemBackend(PathUtf8(root.path()));
  Expect(!first.ok() && first.error == PlatformError::kBusy,
         "bounded cleanup reports additional stale work");

  std::size_t remaining = 0;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(temporary_directory)) {
    if (entry.path().filename().wstring().starts_with(L"part-")) {
      ++remaining;
    }
  }
  Expect(remaining == 1, "bounded cleanup makes progress before returning busy");

  FilesystemBackendOpenResult second = OpenFilesystemBackend(PathUtf8(root.path()));
  Expect(second.ok(), "next open completes the remaining stale cleanup");
#endif
}

void TestWindowsRootEncodingAndProtectedDacl() {
#if defined(_WIN32)
  const FilesystemBackendOpenResult invalid_utf8 =
      OpenFilesystemBackend(std::string("\xc3\x28", 2));
  Expect(!invalid_utf8.ok() && invalid_utf8.error == PlatformError::kInvalidRoot,
         "Windows root conversion rejects malformed UTF-8");

  ScopedDirectory unicode_parent("windows-unicode");
  const std::filesystem::path unicode_root =
      unicode_parent.path() / L"\u63a5\u6536\u76ee\u5f55";
  std::error_code error;
  std::filesystem::create_directory(unicode_root, error);
  Expect(!error, "non-ASCII Windows root fixture is created");
  FilesystemBackendOpenResult unicode_opened =
      OpenFilesystemBackend(PathUtf8(unicode_root));
  Expect(unicode_opened.ok(), "strict UTF-8 conversion accepts a valid Windows root");

  ScopedDirectory root("windows-dacl");
  const std::filesystem::path temporary_directory = root.path() / ".xnn-transfer-tmp";
  error.clear();
  std::filesystem::create_directory(temporary_directory, error);
  Expect(!error && SetPermissiveDacl(temporary_directory),
         "permissive temporary-directory DACL fixture is created");
  const FilesystemBackendOpenResult permissive_directory =
      OpenFilesystemBackend(PathUtf8(root.path()));
  Expect(!permissive_directory.ok() &&
             permissive_directory.error == PlatformError::kInvalidRoot,
         "existing temporary directory with a permissive DACL is rejected");

  ScopedDirectory lock_root("windows-lock-dacl");
  {
    FilesystemBackendOpenResult opened =
        OpenFilesystemBackend(PathUtf8(lock_root.path()));
    Expect(opened.ok(), "backend creates protected Windows lock metadata");
  }
  const std::filesystem::path lock = lock_root.path() / ".xnn-transfer-tmp" / ".lock";
  Expect(SetPermissiveDacl(lock), "permissive lock DACL fixture is created");
  const FilesystemBackendOpenResult permissive_lock =
      OpenFilesystemBackend(PathUtf8(lock_root.path()));
  Expect(!permissive_lock.ok() && permissive_lock.error == PlatformError::kInvalidRoot,
         "existing lock with a permissive DACL is rejected");
#endif
}

}  // namespace

int main() {
  TestManifestFixtureOwnershipAndPathValidation();
  TestAdditionalRequestBounds();
  TestPortableWindowsPathAliases();
  TestSuccessfulAndEmptyCommit();
  TestShortExtraAndPartialWrites();
  TestBudgetSpacePermissionAndOpenCleanup();
  TestIntegrityFlushCommitAndCleanupFailures();
  TestOutcomeUncertainRetainsAccounting();
  TestConcurrentReservationAndIdempotentAbort();
  TestRealFilesystemCommitAndCollision();
  TestRealFilesystemRejectsPermissiveLockMode();
  TestRealFilesystemRejectsPermissiveDirectories();
  TestRealFilesystemRejectsMacosExtendedAcls();
  TestRealFilesystemLinkContainment();
  TestRealFilesystemSymlinkRace();
  TestRestartCleanupAndDestructorAbort();
  TestMissingRootClassification();
  TestWindowsBoundedStaleCleanupProgress();
  TestWindowsRootEncodingAndProtectedDacl();

  if (failures != 0) {
    std::cerr << failures << " storage assertion(s) failed\n";
    return 1;
  }
  std::cout << "Storage tests passed with all 70 manifest cases classified.\n";
  return 0;
}
