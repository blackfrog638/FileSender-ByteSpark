#ifndef XNN_TRANSFER_CORE_SECURITY_IDENTITY_PROTECTED_STORE_HPP_
#define XNN_TRANSFER_CORE_SECURITY_IDENTITY_PROTECTED_STORE_HPP_

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "xnn_transfer/core/security/identity/secret_buffer.hpp"
#include "xnn_transfer/core/security/identity/types.hpp"

namespace xnn_transfer::core::security::identity {

using ProtectedItemId = std::string;

struct ProtectedItemMetadata {
  ProtectedItemId item_id{};
  std::uint64_t revision{};
};

struct ProtectedItem {
  std::uint64_t revision{};
  SecretBuffer payload{};

  ProtectedItem() = default;
  ProtectedItem(std::uint64_t item_revision, SecretBuffer item_payload)
      : revision(item_revision), payload(std::move(item_payload)) {}
  ProtectedItem(ProtectedItem&&) noexcept = default;
  ProtectedItem& operator=(ProtectedItem&&) noexcept = default;
  ProtectedItem(const ProtectedItem&) = delete;
  ProtectedItem& operator=(const ProtectedItem&) = delete;
};

class ProtectedStore {
 public:
  virtual ~ProtectedStore() = default;

  [[nodiscard]] virtual Result<std::vector<ProtectedItemMetadata>> Enumerate() = 0;
  [[nodiscard]] virtual Result<std::optional<ProtectedItem>> Get(
      const ProtectedItemId& item_id) = 0;
  [[nodiscard]] virtual Result<void> CompareExchangePut(
      const ProtectedItemId& item_id, std::optional<std::uint64_t> expected_revision,
      ProtectedItem replacement) = 0;
  [[nodiscard]] virtual Result<void> CompareExchangeDelete(
      const ProtectedItemId& item_id, std::uint64_t expected_revision) = 0;
};

}  // namespace xnn_transfer::core::security::identity

#endif  // XNN_TRANSFER_CORE_SECURITY_IDENTITY_PROTECTED_STORE_HPP_
