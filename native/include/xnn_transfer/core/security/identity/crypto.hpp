#ifndef XNN_TRANSFER_CORE_SECURITY_IDENTITY_CRYPTO_HPP_
#define XNN_TRANSFER_CORE_SECURITY_IDENTITY_CRYPTO_HPP_

#include <cstdint>
#include <span>

#include "xnn_transfer/core/security/identity/secret_buffer.hpp"
#include "xnn_transfer/core/security/identity/types.hpp"

namespace xnn_transfer::core::security::identity {

class IdentityCrypto {
 public:
  virtual ~IdentityCrypto() = default;

  [[nodiscard]] virtual Result<SecretBuffer> GenerateSeed() = 0;
  [[nodiscard]] virtual Result<StoreId> GenerateStoreId() = 0;
  [[nodiscard]] virtual Result<PublicKey> DerivePublicKey(
      std::span<const std::uint8_t> seed) = 0;
  [[nodiscard]] virtual Result<DeviceId> DeriveDeviceId(
      const PublicKey& public_key) = 0;
  [[nodiscard]] virtual Result<SecretBuffer> DerivePeerRecordMacKey(
      std::span<const std::uint8_t> seed, const StoreId& store_id) = 0;
  [[nodiscard]] virtual Result<Mac> HmacSha256(
      std::span<const std::uint8_t> key, std::span<const std::uint8_t> message) = 0;
};

class OpenSslIdentityCrypto final : public IdentityCrypto {
 public:
  [[nodiscard]] Result<SecretBuffer> GenerateSeed() override;
  [[nodiscard]] Result<StoreId> GenerateStoreId() override;
  [[nodiscard]] Result<PublicKey> DerivePublicKey(
      std::span<const std::uint8_t> seed) override;
  [[nodiscard]] Result<DeviceId> DeriveDeviceId(const PublicKey& public_key) override;
  [[nodiscard]] Result<SecretBuffer> DerivePeerRecordMacKey(
      std::span<const std::uint8_t> seed, const StoreId& store_id) override;
  [[nodiscard]] Result<Mac> HmacSha256(std::span<const std::uint8_t> key,
                                       std::span<const std::uint8_t> message) override;
};

}  // namespace xnn_transfer::core::security::identity

#endif  // XNN_TRANSFER_CORE_SECURITY_IDENTITY_CRYPTO_HPP_
