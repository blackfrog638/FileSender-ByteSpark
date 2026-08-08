#ifndef XNN_TRANSFER_CORE_SECURITY_IDENTITY_WINDOWS_CREDENTIAL_STORE_HPP_
#define XNN_TRANSFER_CORE_SECURITY_IDENTITY_WINDOWS_CREDENTIAL_STORE_HPP_

#include <memory>

#include "xnn_transfer/core/security/identity/protected_store.hpp"

namespace xnn_transfer::core::security::identity {

[[nodiscard]] Result<std::unique_ptr<ProtectedStore>>
CreateWindowsCredentialProtectedStore();

}  // namespace xnn_transfer::core::security::identity

#endif  // XNN_TRANSFER_CORE_SECURITY_IDENTITY_WINDOWS_CREDENTIAL_STORE_HPP_
