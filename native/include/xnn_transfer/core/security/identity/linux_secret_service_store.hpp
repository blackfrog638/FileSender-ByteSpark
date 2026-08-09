#ifndef XNN_TRANSFER_CORE_SECURITY_IDENTITY_LINUX_SECRET_SERVICE_STORE_HPP_
#define XNN_TRANSFER_CORE_SECURITY_IDENTITY_LINUX_SECRET_SERVICE_STORE_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "xnn_transfer/core/security/identity/protected_store.hpp"

namespace xnn_transfer::core::security::identity {

inline constexpr std::string_view kQualifiedGnomeKeyringExecutablePath =
    "/usr/bin/gnome-keyring-daemon";

struct LinuxSecretServiceBackendIdentity {
  std::uint32_t user_id{};
  std::uint32_t process_id{};
  std::string executable_path{};
};

// The supported Linux profile is the current-user GNOME Keyring daemon from
// the root-owned system executable. Other Secret Service implementations stay
// unavailable even when they expose the same D-Bus API.
[[nodiscard]] bool IsQualifiedGnomeKeyringBackendIdentity(
    const LinuxSecretServiceBackendIdentity& identity) noexcept;

[[nodiscard]] Result<std::unique_ptr<ProtectedStore>>
CreateLinuxSecretServiceProtectedStore();

}  // namespace xnn_transfer::core::security::identity

#endif  // XNN_TRANSFER_CORE_SECURITY_IDENTITY_LINUX_SECRET_SERVICE_STORE_HPP_
