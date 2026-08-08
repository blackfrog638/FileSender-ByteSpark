#ifndef XNN_TRANSFER_CORE_SECURITY_IDENTITY_LINUX_SECRET_SERVICE_STORE_HPP_
#define XNN_TRANSFER_CORE_SECURITY_IDENTITY_LINUX_SECRET_SERVICE_STORE_HPP_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "xnn_transfer/core/security/identity/protected_store.hpp"

namespace xnn_transfer::core::security::identity {

struct LinuxSecretServiceBackendIdentity {
  std::uint32_t user_id{};
  std::uint32_t process_id{};
  std::string executable_path{};
};

using LinuxSecretServiceBackendQualifier =
    std::function<bool(const LinuxSecretServiceBackendIdentity&)>;

// The qualifier must establish that the concrete service is device-local and
// non-synchronizing. An absent or rejecting qualifier fails closed.
[[nodiscard]] Result<std::unique_ptr<ProtectedStore>>
CreateLinuxSecretServiceProtectedStore(LinuxSecretServiceBackendQualifier qualifier);

}  // namespace xnn_transfer::core::security::identity

#endif  // XNN_TRANSFER_CORE_SECURITY_IDENTITY_LINUX_SECRET_SERVICE_STORE_HPP_
