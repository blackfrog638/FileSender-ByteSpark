#ifndef XNN_TRANSFER_SRC_SECURITY_IDENTITY_CODEC_HPP_
#define XNN_TRANSFER_SRC_SECURITY_IDENTITY_CODEC_HPP_

#include <cstdint>
#include <optional>
#include <span>

#include "xnn_transfer/core/security/identity/secret_buffer.hpp"
#include "xnn_transfer/core/security/identity/types.hpp"

namespace xnn_transfer::core::security::identity::internal {

struct RootRecord {
  std::uint64_t revision{};
  SecretBuffer seed{};
  PublicKey public_key{};
  DeviceId device_id{};
  StoreId store_id{};
  std::optional<StoreId> retired_store_id{};

  RootRecord() = default;
  RootRecord(RootRecord&&) noexcept = default;
  RootRecord& operator=(RootRecord&&) noexcept = default;
  RootRecord(const RootRecord&) = delete;
  RootRecord& operator=(const RootRecord&) = delete;
};

struct StoredPeerRecord {
  StoreId store_id{};
  DeviceId record_id{};
  DeviceId root_device_id{};
  PeerRecord peer{};
  Mac authenticator{};
};

[[nodiscard]] Result<SecretBuffer> EncodeRootRecord(const RootRecord& record);
[[nodiscard]] Result<RootRecord> DecodeRootRecord(
    std::span<const std::uint8_t> encoded);

[[nodiscard]] Result<SecretBuffer> EncodePeerRecord(const StoredPeerRecord& record);
[[nodiscard]] Result<SecretBuffer> EncodePeerMacInput(const StoredPeerRecord& record);
[[nodiscard]] Result<StoredPeerRecord> DecodePeerRecord(
    std::span<const std::uint8_t> encoded);

}  // namespace xnn_transfer::core::security::identity::internal

#endif  // XNN_TRANSFER_SRC_SECURITY_IDENTITY_CODEC_HPP_
