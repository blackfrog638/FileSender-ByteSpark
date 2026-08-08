#ifndef XNN_TRANSFER_NATIVE_SRC_SECURITY_IDENTITY_PLATFORM_PROTECTED_STORE_INTERNAL_HPP_
#define XNN_TRANSFER_NATIVE_SRC_SECURITY_IDENTITY_PLATFORM_PROTECTED_STORE_INTERNAL_HPP_

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xnn_transfer/core/security/identity/protected_store.hpp"

namespace xnn_transfer::core::security::identity::internal {

struct PlatformProtectedItem {
  ProtectedItemId item_id{};
  std::uint64_t revision{};
  SecretBuffer payload{};

  PlatformProtectedItem() = default;
  PlatformProtectedItem(ProtectedItemId id, const std::uint64_t item_revision,
                        SecretBuffer item_payload)
      : item_id(std::move(id)),
        revision(item_revision),
        payload(std::move(item_payload)) {}
  PlatformProtectedItem(PlatformProtectedItem&&) noexcept = default;
  PlatformProtectedItem& operator=(PlatformProtectedItem&&) noexcept = default;
  PlatformProtectedItem(const PlatformProtectedItem&) = delete;
  PlatformProtectedItem& operator=(const PlatformProtectedItem&) = delete;
};

class PlatformProtectedStoreBackend {
 public:
  virtual ~PlatformProtectedStoreBackend() = default;

  [[nodiscard]] virtual Result<std::vector<PlatformProtectedItem>> Load(
      std::optional<std::string_view> item_id) = 0;
  [[nodiscard]] virtual Result<void> Put(const PlatformProtectedItem& item) = 0;
  [[nodiscard]] virtual Result<void> Delete(std::string_view item_id) = 0;
};

class PlatformStoreLockGuard {
 public:
  virtual ~PlatformStoreLockGuard() = default;
};

class PlatformStoreOperationLock {
 public:
  virtual ~PlatformStoreOperationLock() = default;

  [[nodiscard]] virtual Result<std::unique_ptr<PlatformStoreLockGuard>> Acquire() = 0;
};

[[nodiscard]] std::unique_ptr<ProtectedStore> MakePlatformProtectedStore(
    std::unique_ptr<PlatformProtectedStoreBackend> backend,
    std::unique_ptr<PlatformStoreOperationLock> operation_lock);

[[nodiscard]] Result<SecretBuffer> EncodePlatformProtectedItemEnvelope(
    const PlatformProtectedItem& item);

[[nodiscard]] Result<PlatformProtectedItem> DecodePlatformProtectedItemEnvelope(
    ProtectedItemId item_id, std::span<const std::uint8_t> envelope);

[[nodiscard]] std::unique_ptr<PlatformStoreOperationLock> MakePosixDirectoryLock(
    std::string runtime_directory);

}  // namespace xnn_transfer::core::security::identity::internal

#endif  // XNN_TRANSFER_NATIVE_SRC_SECURITY_IDENTITY_PLATFORM_PROTECTED_STORE_INTERNAL_HPP_
