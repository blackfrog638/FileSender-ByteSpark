#ifndef XNN_TRANSFER_NATIVE_SRC_SECURITY_IDENTITY_LINUX_SECRET_SERVICE_STORE_INTERNAL_HPP_
#define XNN_TRANSFER_NATIVE_SRC_SECURITY_IDENTITY_LINUX_SECRET_SERVICE_STORE_INTERNAL_HPP_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xnn_transfer/core/security/identity/protected_store.hpp"

namespace xnn_transfer::core::security::identity::internal {

struct LinuxSecretServiceItem {
  ProtectedItemId item_id{};
  std::uint64_t revision{};
  SecretBuffer payload{};

  LinuxSecretServiceItem() = default;
  LinuxSecretServiceItem(ProtectedItemId id, const std::uint64_t item_revision,
                         SecretBuffer item_payload)
      : item_id(std::move(id)),
        revision(item_revision),
        payload(std::move(item_payload)) {}
  LinuxSecretServiceItem(LinuxSecretServiceItem&&) noexcept = default;
  LinuxSecretServiceItem& operator=(LinuxSecretServiceItem&&) noexcept = default;
  LinuxSecretServiceItem(const LinuxSecretServiceItem&) = delete;
  LinuxSecretServiceItem& operator=(const LinuxSecretServiceItem&) = delete;
};

class LinuxSecretServiceBackend {
 public:
  virtual ~LinuxSecretServiceBackend() = default;

  [[nodiscard]] virtual Result<std::vector<LinuxSecretServiceItem>> Load(
      std::optional<std::string_view> item_id) = 0;
  [[nodiscard]] virtual Result<void> Put(const LinuxSecretServiceItem& item) = 0;
  [[nodiscard]] virtual Result<void> Delete(std::string_view item_id) = 0;
};

class LinuxStoreLockGuard {
 public:
  virtual ~LinuxStoreLockGuard() = default;
};

class LinuxStoreOperationLock {
 public:
  virtual ~LinuxStoreOperationLock() = default;

  [[nodiscard]] virtual Result<std::unique_ptr<LinuxStoreLockGuard>> Acquire() = 0;
};

[[nodiscard]] std::unique_ptr<ProtectedStore> MakeLinuxSecretServiceStore(
    std::unique_ptr<LinuxSecretServiceBackend> backend,
    std::unique_ptr<LinuxStoreOperationLock> operation_lock);

[[nodiscard]] std::unique_ptr<LinuxStoreOperationLock> MakeLinuxRuntimeDirectoryLock(
    std::string runtime_directory);

}  // namespace xnn_transfer::core::security::identity::internal

#endif  // XNN_TRANSFER_NATIVE_SRC_SECURITY_IDENTITY_LINUX_SECRET_SERVICE_STORE_INTERNAL_HPP_
