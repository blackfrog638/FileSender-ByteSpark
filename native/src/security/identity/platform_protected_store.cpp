#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "platform_protected_store_internal.hpp"

namespace xnn_transfer::core::security::identity::internal {
namespace {

constexpr std::size_t kMaxEnumeratedItems = (kMaxPeerRecords + 1) * 2;
constexpr std::array<std::uint8_t, 4> kEnvelopeMagic{'X', 'N', 'S', 'P'};
constexpr std::uint8_t kEnvelopeVersion = 1;
constexpr std::size_t kEnvelopeHeaderSize = 17;

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

[[nodiscard]] Result<void> ValidateItemId(const std::string_view item_id) {
  if (item_id.empty() || item_id.find('\0') != std::string_view::npos) {
    return Result<void>::Failure(ErrorCode::kInvalidArgument);
  }
  if (item_id.size() > kMaxProtectedItemIdBytes) {
    return Result<void>::Failure(ErrorCode::kCapacityExceeded);
  }
  return Result<void>::Success();
}

[[nodiscard]] Result<void> ValidateLoadedItem(const PlatformProtectedItem& item) {
  Result<void> id_result = ValidateItemId(item.item_id);
  if (!id_result.ok()) {
    return Result<void>::Failure(ErrorCode::kCorruptRecord);
  }
  if (item.revision == 0 || item.payload.size() > kMaxProtectedItemPayloadSize) {
    return Result<void>::Failure(ErrorCode::kCorruptRecord);
  }
  return Result<void>::Success();
}

class PlatformProtectedStore final : public ProtectedStore {
 public:
  PlatformProtectedStore(std::unique_ptr<PlatformProtectedStoreBackend> backend,
                         std::unique_ptr<PlatformStoreOperationLock> operation_lock)
      : backend_(std::move(backend)), operation_lock_(std::move(operation_lock)) {}

  Result<std::vector<ProtectedItemMetadata>> Enumerate() override {
    try {
      auto guard_result = operation_lock_->Acquire();
      if (!guard_result.ok()) {
        return Result<std::vector<ProtectedItemMetadata>>::Failure(
            guard_result.error());
      }
      [[maybe_unused]] auto guard = std::move(guard_result).value();

      auto load_result = backend_->Load(std::nullopt);
      if (!load_result.ok()) {
        return Result<std::vector<ProtectedItemMetadata>>::Failure(load_result.error());
      }
      std::vector<PlatformProtectedItem> items = std::move(load_result).value();
      if (items.size() > kMaxEnumeratedItems) {
        return Result<std::vector<ProtectedItemMetadata>>::Failure(
            ErrorCode::kCapacityExceeded);
      }

      std::vector<ProtectedItemMetadata> metadata;
      metadata.reserve(items.size());
      for (const PlatformProtectedItem& item : items) {
        Result<void> validation = ValidateLoadedItem(item);
        if (!validation.ok()) {
          return Result<std::vector<ProtectedItemMetadata>>::Failure(
              validation.error());
        }
        metadata.push_back(ProtectedItemMetadata{item.item_id, item.revision});
      }
      std::sort(
          metadata.begin(), metadata.end(),
          [](const ProtectedItemMetadata& left, const ProtectedItemMetadata& right) {
            return left.item_id < right.item_id;
          });
      for (std::size_t index = 1; index < metadata.size(); ++index) {
        if (metadata[index - 1].item_id == metadata[index].item_id) {
          return Result<std::vector<ProtectedItemMetadata>>::Failure(
              ErrorCode::kCorruptRecord);
        }
      }
      return Result<std::vector<ProtectedItemMetadata>>::Success(std::move(metadata));
    } catch (const std::bad_alloc&) {
      return Result<std::vector<ProtectedItemMetadata>>::Failure(
          ErrorCode::kCapacityExceeded);
    }
  }

  Result<std::optional<ProtectedItem>> Get(const ProtectedItemId& item_id) override {
    Result<void> id_result = ValidateItemId(item_id);
    if (!id_result.ok()) {
      return Result<std::optional<ProtectedItem>>::Failure(id_result.error());
    }

    try {
      auto guard_result = operation_lock_->Acquire();
      if (!guard_result.ok()) {
        return Result<std::optional<ProtectedItem>>::Failure(guard_result.error());
      }
      [[maybe_unused]] auto guard = std::move(guard_result).value();

      auto load_result = backend_->Load(item_id);
      if (!load_result.ok()) {
        return Result<std::optional<ProtectedItem>>::Failure(load_result.error());
      }
      std::vector<PlatformProtectedItem> items = std::move(load_result).value();
      if (items.empty()) {
        return Result<std::optional<ProtectedItem>>::Success(std::nullopt);
      }
      if (items.size() != 1 || items.front().item_id != item_id) {
        return Result<std::optional<ProtectedItem>>::Failure(ErrorCode::kCorruptRecord);
      }
      Result<void> validation = ValidateLoadedItem(items.front());
      if (!validation.ok()) {
        return Result<std::optional<ProtectedItem>>::Failure(validation.error());
      }
      PlatformProtectedItem item = std::move(items.front());
      return Result<std::optional<ProtectedItem>>::Success(
          ProtectedItem(item.revision, std::move(item.payload)));
    } catch (const std::bad_alloc&) {
      return Result<std::optional<ProtectedItem>>::Failure(
          ErrorCode::kCapacityExceeded);
    }
  }

  Result<void> CompareExchangePut(const ProtectedItemId& item_id,
                                  const std::optional<std::uint64_t> expected_revision,
                                  ProtectedItem replacement) override {
    Result<void> argument_result =
        ValidatePutArguments(item_id, expected_revision, replacement);
    if (!argument_result.ok()) {
      return argument_result;
    }

    try {
      auto guard_result = operation_lock_->Acquire();
      if (!guard_result.ok()) {
        return Result<void>::Failure(guard_result.error());
      }
      [[maybe_unused]] auto guard = std::move(guard_result).value();

      auto load_result = backend_->Load(item_id);
      if (!load_result.ok()) {
        return Result<void>::Failure(load_result.error());
      }
      std::vector<PlatformProtectedItem> items = std::move(load_result).value();
      Result<void> comparison = CompareRevision(items, item_id, expected_revision);
      if (!comparison.ok()) {
        return comparison;
      }

      PlatformProtectedItem item(item_id, replacement.revision,
                                 std::move(replacement.payload));
      return backend_->Put(item);
    } catch (const std::bad_alloc&) {
      return Result<void>::Failure(ErrorCode::kCapacityExceeded);
    }
  }

  Result<void> CompareExchangeDelete(const ProtectedItemId& item_id,
                                     const std::uint64_t expected_revision) override {
    Result<void> id_result = ValidateItemId(item_id);
    if (!id_result.ok()) {
      return id_result;
    }
    if (expected_revision == 0) {
      return Result<void>::Failure(ErrorCode::kInvalidArgument);
    }

    try {
      auto guard_result = operation_lock_->Acquire();
      if (!guard_result.ok()) {
        return Result<void>::Failure(guard_result.error());
      }
      [[maybe_unused]] auto guard = std::move(guard_result).value();

      auto load_result = backend_->Load(item_id);
      if (!load_result.ok()) {
        return Result<void>::Failure(load_result.error());
      }
      std::vector<PlatformProtectedItem> items = std::move(load_result).value();
      Result<void> comparison = CompareRevision(items, item_id, expected_revision);
      if (!comparison.ok()) {
        return comparison;
      }
      return backend_->Delete(item_id);
    } catch (const std::bad_alloc&) {
      return Result<void>::Failure(ErrorCode::kCapacityExceeded);
    }
  }

 private:
  [[nodiscard]] static Result<void> ValidatePutArguments(
      const std::string_view item_id,
      const std::optional<std::uint64_t> expected_revision,
      const ProtectedItem& replacement) {
    Result<void> id_result = ValidateItemId(item_id);
    if (!id_result.ok()) {
      return id_result;
    }
    if (replacement.payload.size() > kMaxProtectedItemPayloadSize) {
      return Result<void>::Failure(ErrorCode::kCapacityExceeded);
    }
    if (!expected_revision.has_value()) {
      if (replacement.revision != 1) {
        return Result<void>::Failure(ErrorCode::kInvalidArgument);
      }
      return Result<void>::Success();
    }
    if (*expected_revision == 0 ||
        *expected_revision == std::numeric_limits<std::uint64_t>::max() ||
        replacement.revision != *expected_revision + 1) {
      return Result<void>::Failure(ErrorCode::kInvalidArgument);
    }
    return Result<void>::Success();
  }

  [[nodiscard]] static Result<void> CompareRevision(
      const std::vector<PlatformProtectedItem>& items, const std::string_view item_id,
      const std::optional<std::uint64_t> expected_revision) {
    if (items.size() > 1) {
      return Result<void>::Failure(ErrorCode::kCorruptRecord);
    }
    if (items.empty()) {
      return expected_revision.has_value()
                 ? Result<void>::Failure(ErrorCode::kRevisionConflict)
                 : Result<void>::Success();
    }

    const PlatformProtectedItem& item = items.front();
    Result<void> validation = ValidateLoadedItem(item);
    if (!validation.ok() || item.item_id != item_id) {
      return Result<void>::Failure(ErrorCode::kCorruptRecord);
    }
    if (!expected_revision.has_value() || item.revision != *expected_revision) {
      return Result<void>::Failure(ErrorCode::kRevisionConflict);
    }
    return Result<void>::Success();
  }

  std::unique_ptr<PlatformProtectedStoreBackend> backend_;
  std::unique_ptr<PlatformStoreOperationLock> operation_lock_;
};

}  // namespace

Result<SecretBuffer> EncodePlatformProtectedItemEnvelope(
    const PlatformProtectedItem& item) {
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

Result<PlatformProtectedItem> DecodePlatformProtectedItemEnvelope(
    ProtectedItemId item_id, const std::span<const std::uint8_t> envelope) {
  if (envelope.size() < kEnvelopeHeaderSize ||
      envelope.size() > kEnvelopeHeaderSize + kMaxProtectedItemPayloadSize ||
      !std::equal(kEnvelopeMagic.begin(), kEnvelopeMagic.end(), envelope.begin()) ||
      envelope[4] != kEnvelopeVersion) {
    return Result<PlatformProtectedItem>::Failure(ErrorCode::kCorruptRecord);
  }
  const std::uint64_t revision = ReadU64(envelope.subspan(5, 8));
  const std::uint32_t payload_size = ReadU32(envelope.subspan(13, 4));
  if (revision == 0 || payload_size != envelope.size() - kEnvelopeHeaderSize) {
    return Result<PlatformProtectedItem>::Failure(ErrorCode::kCorruptRecord);
  }
  try {
    SecretBuffer payload(envelope.subspan(kEnvelopeHeaderSize, payload_size));
    return Result<PlatformProtectedItem>::Success(
        PlatformProtectedItem(std::move(item_id), revision, std::move(payload)));
  } catch (const std::bad_alloc&) {
    return Result<PlatformProtectedItem>::Failure(ErrorCode::kCapacityExceeded);
  }
}

std::unique_ptr<ProtectedStore> MakePlatformProtectedStore(
    std::unique_ptr<PlatformProtectedStoreBackend> backend,
    std::unique_ptr<PlatformStoreOperationLock> operation_lock) {
  if (backend == nullptr || operation_lock == nullptr) {
    return nullptr;
  }
  return std::make_unique<PlatformProtectedStore>(std::move(backend),
                                                  std::move(operation_lock));
}

}  // namespace xnn_transfer::core::security::identity::internal
