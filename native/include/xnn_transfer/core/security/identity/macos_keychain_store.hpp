#ifndef XNN_TRANSFER_CORE_SECURITY_IDENTITY_MACOS_KEYCHAIN_STORE_HPP_
#define XNN_TRANSFER_CORE_SECURITY_IDENTITY_MACOS_KEYCHAIN_STORE_HPP_

#include <memory>

#include "xnn_transfer/core/security/identity/protected_store.hpp"

namespace xnn_transfer::core::security::identity {

[[nodiscard]] Result<std::unique_ptr<ProtectedStore>>
CreateMacosKeychainProtectedStore();

}  // namespace xnn_transfer::core::security::identity

#endif  // XNN_TRANSFER_CORE_SECURITY_IDENTITY_MACOS_KEYCHAIN_STORE_HPP_
