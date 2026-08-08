#ifndef XNN_TRANSFER_CORE_SECURITY_IDENTITY_IDENTITY_REPOSITORY_HPP_
#define XNN_TRANSFER_CORE_SECURITY_IDENTITY_IDENTITY_REPOSITORY_HPP_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "xnn_transfer/core/security/identity/crypto.hpp"
#include "xnn_transfer/core/security/identity/protected_store.hpp"
#include "xnn_transfer/core/security/identity/types.hpp"

namespace xnn_transfer::core::security::identity {

class PeerPublicKeyValidator {
 public:
  virtual ~PeerPublicKeyValidator() = default;

  // Success means the exact bytes canonically encode a non-identity point in
  // the Ed25519 prime-order subgroup required by ADR 0002.
  [[nodiscard]] virtual Result<void> Validate(const PublicKey& public_key) = 0;
};

struct PeerCommit {
  PublicKey public_key{};
  std::uint16_t security_profile{};
  std::string display_label{};
};

struct ResetOutcome {
  bool cleanup_complete{};
};

using IdentitySeedConsumer = std::function<Result<void>(std::span<const std::uint8_t>)>;

class IdentityRepository final {
 public:
  IdentityRepository(ProtectedStore& store, IdentityCrypto& crypto,
                     PeerPublicKeyValidator& peer_key_validator);
  ~IdentityRepository();

  IdentityRepository(const IdentityRepository&) = delete;
  IdentityRepository& operator=(const IdentityRepository&) = delete;
  IdentityRepository(IdentityRepository&&) = delete;
  IdentityRepository& operator=(IdentityRepository&&) = delete;

  [[nodiscard]] Result<void> Open();
  [[nodiscard]] Result<void> Refresh();
  [[nodiscard]] Result<ResetOutcome> Reset();
  [[nodiscard]] Result<void> CleanupStaleItems();

  [[nodiscard]] Result<DeviceId> CommitPeer(PeerCommit peer);
  [[nodiscard]] Result<DeviceId> RotatePeer(const DeviceId& current_device_id,
                                            const PublicKey& new_public_key,
                                            std::uint64_t rotation_counter);
  [[nodiscard]] Result<void> RevokePeer(const DeviceId& device_id);
  [[nodiscard]] Result<void> ForgetPeer(const DeviceId& device_id);

  [[nodiscard]] Result<void> UseIdentitySeed(
      const IdentitySeedConsumer& consumer) const;
  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] std::uint64_t revision() const noexcept;
  [[nodiscard]] const PublicKey* root_public_key() const noexcept;
  [[nodiscard]] const DeviceId* root_device_id() const noexcept;
  [[nodiscard]] std::span<const PeerRecord> peers() const noexcept;
  [[nodiscard]] const PeerRecord* FindPeer(const DeviceId& device_id) const noexcept;

 private:
  struct State;
  struct StoredPeer;

  [[nodiscard]] Result<void> InitializeEmpty();
  [[nodiscard]] Result<void> LoadFromRoot(ProtectedItem root_item);
  [[nodiscard]] Result<void> EnsureRootCurrent() const;
  [[nodiscard]] Result<SecretBuffer> LoadIdentitySeed() const;
  [[nodiscard]] Result<void> ValidatePeer(const StoredPeer& peer) const;
  [[nodiscard]] Result<Mac> ComputePeerMac(const StoredPeer& peer) const;
  [[nodiscard]] Result<void> PutPeer(StoredPeer peer,
                                     std::optional<std::uint64_t> expected_revision);
  [[nodiscard]] StoredPeer* FindStoredPeer(const DeviceId& device_id) noexcept;
  [[nodiscard]] const StoredPeer* FindStoredPeer(
      const DeviceId& device_id) const noexcept;
  [[nodiscard]] bool KeyIsKnown(
      const PublicKey& public_key,
      const DeviceId* except_device_id = nullptr) const noexcept;

  ProtectedStore* store_;
  IdentityCrypto* crypto_;
  PeerPublicKeyValidator* peer_key_validator_;
  std::unique_ptr<State> state_;
  bool ready_{};
};

}  // namespace xnn_transfer::core::security::identity

#endif  // XNN_TRANSFER_CORE_SECURITY_IDENTITY_IDENTITY_REPOSITORY_HPP_
