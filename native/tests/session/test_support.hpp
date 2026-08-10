#ifndef XNN_TRANSFER_TESTS_SESSION_TEST_SUPPORT_HPP_
#define XNN_TRANSFER_TESTS_SESSION_TEST_SUPPORT_HPP_

#ifndef NOMINMAX
#define NOMINMAX
#endif

// OpenSSL may expose Windows SDK macros before this shared test helper.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xnn_transfer/core/security/identity/crypto.hpp"
#include "xnn_transfer/core/security/identity/identity_repository.hpp"
#include "xnn_transfer/core/security/identity/protected_store.hpp"
#include "xnn_transfer/core/security/tls/security_profile.hpp"
#include "xnn_transfer/core/session/session.hpp"

namespace session_test {

namespace identity = xnn_transfer::core::security::identity;
namespace session = xnn_transfer::core::session;
namespace tls = xnn_transfer::core::security::tls;

inline session::Bytes DecodeHex(const std::string_view encoded) {
  const auto nibble = [](const char value) -> std::uint8_t {
    if (value >= '0' && value <= '9') {
      return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
      return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    return static_cast<std::uint8_t>(value - 'A' + 10);
  };
  session::Bytes output;
  output.reserve(encoded.size() / 2);
  for (std::size_t offset = 0; offset + 1 < encoded.size(); offset += 2) {
    output.push_back(static_cast<std::uint8_t>((nibble(encoded[offset]) << 4U) |
                                               nibble(encoded[offset + 1])));
  }
  return output;
}

template <std::size_t Size>
inline std::array<std::uint8_t, Size> DecodeArray(const std::string_view encoded) {
  const session::Bytes bytes = DecodeHex(encoded);
  std::array<std::uint8_t, Size> output{};
  std::copy_n(bytes.begin(), std::min(bytes.size(), output.size()), output.begin());
  return output;
}

class FakeProtectedStore final : public identity::ProtectedStore {
 public:
  identity::Result<std::vector<identity::ProtectedItemMetadata>> Enumerate() override {
    std::vector<identity::ProtectedItemMetadata> metadata;
    metadata.reserve(items_.size());
    for (const auto& [id, item] : items_) {
      metadata.push_back({id, item.revision});
    }
    return identity::Result<std::vector<identity::ProtectedItemMetadata>>::Success(
        std::move(metadata));
  }

  identity::Result<std::optional<identity::ProtectedItem>> Get(
      const identity::ProtectedItemId& item_id) override {
    const auto iterator = items_.find(item_id);
    if (iterator == items_.end()) {
      return identity::Result<std::optional<identity::ProtectedItem>>::Success(
          std::nullopt);
    }
    return identity::Result<std::optional<identity::ProtectedItem>>::Success(
        identity::ProtectedItem(
            iterator->second.revision,
            identity::SecretBuffer(iterator->second.payload.bytes())));
  }

  identity::Result<void> CompareExchangePut(
      const identity::ProtectedItemId& item_id,
      const std::optional<std::uint64_t> expected_revision,
      identity::ProtectedItem replacement) override {
    {
      std::unique_lock lock(block_mutex_);
      if (block_next_put_) {
        put_blocked_ = true;
        block_condition_.notify_all();
        block_condition_.wait(lock, [this] { return release_blocked_put_; });
        block_next_put_ = false;
        put_blocked_ = false;
        release_blocked_put_ = false;
      }
    }
    if (fail_next_put_) {
      fail_next_put_ = false;
      return identity::Result<void>::Failure(identity::ErrorCode::kStorageUnavailable);
    }
    auto iterator = items_.find(item_id);
    const bool creates = !expected_revision.has_value() && iterator == items_.end() &&
                         replacement.revision == 1;
    const bool updates =
        expected_revision.has_value() && iterator != items_.end() &&
        iterator->second.revision == *expected_revision &&
        *expected_revision != std::numeric_limits<std::uint64_t>::max() &&
        replacement.revision == *expected_revision + 1;
    if (!creates && !updates) {
      return identity::Result<void>::Failure(identity::ErrorCode::kRevisionConflict);
    }
    if (iterator == items_.end()) {
      items_.emplace(item_id, std::move(replacement));
    } else {
      iterator->second = std::move(replacement);
    }
    return identity::Result<void>::Success();
  }

  identity::Result<void> CompareExchangeDelete(
      const identity::ProtectedItemId& item_id,
      const std::uint64_t expected_revision) override {
    const auto iterator = items_.find(item_id);
    if (iterator == items_.end() || iterator->second.revision != expected_revision) {
      return identity::Result<void>::Failure(identity::ErrorCode::kRevisionConflict);
    }
    items_.erase(iterator);
    return identity::Result<void>::Success();
  }

  void FailNextPut() noexcept { fail_next_put_ = true; }

  void BlockNextPut() {
    const std::lock_guard lock(block_mutex_);
    block_next_put_ = true;
    put_blocked_ = false;
    release_blocked_put_ = false;
  }

  [[nodiscard]] bool WaitForBlockedPut() {
    std::unique_lock lock(block_mutex_);
    return block_condition_.wait_for(lock, std::chrono::seconds(5),
                                     [this] { return put_blocked_; });
  }

  void ReleaseBlockedPut() {
    const std::lock_guard lock(block_mutex_);
    release_blocked_put_ = true;
    block_condition_.notify_all();
  }

 private:
  std::map<identity::ProtectedItemId, identity::ProtectedItem> items_{};
  bool fail_next_put_{};
  std::mutex block_mutex_{};
  std::condition_variable block_condition_{};
  bool block_next_put_{};
  bool put_blocked_{};
  bool release_blocked_put_{};
};

class FixedIdentityCrypto final : public identity::IdentityCrypto {
 public:
  explicit FixedIdentityCrypto(const std::array<std::uint8_t, 32> seed) : seed_(seed) {}

  identity::Result<identity::SecretBuffer> GenerateSeed() override {
    std::array<std::uint8_t, 32> generated = seed_;
    generated.back() ^= generation_;
    ++generation_;
    return identity::Result<identity::SecretBuffer>::Success(
        identity::SecretBuffer(generated));
  }

  identity::Result<identity::StoreId> GenerateStoreId() override {
    identity::StoreId store_id{};
    std::copy_n(seed_.begin(), store_id.size(), store_id.begin());
    store_id[0] ^= 0xa5U;
    store_id.back() ^= generation_;
    return identity::Result<identity::StoreId>::Success(store_id);
  }

  identity::Result<identity::PublicKey> DerivePublicKey(
      const std::span<const std::uint8_t> seed) override {
    return delegate_.DerivePublicKey(seed);
  }

  identity::Result<identity::DeviceId> DeriveDeviceId(
      const identity::PublicKey& public_key) override {
    return delegate_.DeriveDeviceId(public_key);
  }

  identity::Result<identity::SecretBuffer> DerivePeerRecordMacKey(
      const std::span<const std::uint8_t> seed,
      const identity::StoreId& store_id) override {
    return delegate_.DerivePeerRecordMacKey(seed, store_id);
  }

  identity::Result<identity::Mac> HmacSha256(
      const std::span<const std::uint8_t> key,
      const std::span<const std::uint8_t> message) override {
    return delegate_.HmacSha256(key, message);
  }

 private:
  std::array<std::uint8_t, 32> seed_{};
  std::uint8_t generation_{};
  identity::OpenSslIdentityCrypto delegate_{};
};

class IdentityFixture final {
 public:
  explicit IdentityFixture(const std::array<std::uint8_t, 32> seed)
      : crypto(seed), repository(store, crypto, validator) {}

  FakeProtectedStore store{};
  FixedIdentityCrypto crypto;
  tls::OpenSslPeerPublicKeyValidator validator{};
  identity::IdentityRepository repository;
};

class FixedAttemptEntropy final : public session::SessionEntropy {
 public:
  FixedAttemptEntropy(const std::uint8_t handle_start, const std::uint8_t nonce_start)
      : handle_start_(handle_start), nonce_start_(nonce_start) {}

  bool Fill(const std::span<std::uint8_t> output) override {
    const std::uint8_t start = calls_++ == 0 ? handle_start_ : nonce_start_;
    for (std::size_t index = 0; index < output.size(); ++index) {
      output[index] =
          static_cast<std::uint8_t>(start + static_cast<std::uint8_t>(index));
    }
    return true;
  }

 private:
  std::uint8_t handle_start_{};
  std::uint8_t nonce_start_{};
  std::size_t calls_{};
};

class CounterEntropy final : public session::SessionEntropy {
 public:
  bool Fill(const std::span<std::uint8_t> output) override {
    ++counter_;
    for (std::size_t index = 0; index < output.size(); ++index) {
      output[index] = static_cast<std::uint8_t>(counter_ + index);
    }
    return true;
  }

 private:
  std::uint8_t counter_{};
};

class FakePairingChannel final : public session::PairingChannel {
 public:
  explicit FakePairingChannel(tls::ValidatedEd25519PublicKey peer_public_key)
      : peer_public_key_(std::move(peer_public_key)) {}

  const tls::ValidatedEd25519PublicKey& peer_public_key() const noexcept override {
    return peer_public_key_;
  }

  identity::Result<identity::SecretBuffer> ExportPairing(
      const tls::PairingContext&) override {
    std::array<std::uint8_t, tls::kSha256Size> exporter{};
    for (std::size_t index = 0; index < exporter.size(); ++index) {
      exporter[index] = static_cast<std::uint8_t>(0x80U + index);
    }
    return identity::Result<identity::SecretBuffer>::Success(
        identity::SecretBuffer(exporter));
  }

  identity::Result<identity::SecretBuffer> ExportConfirmation(
      const tls::PairingContext&) override {
    std::array<std::uint8_t, tls::kSha256Size> exporter{};
    for (std::size_t index = 0; index < exporter.size(); ++index) {
      exporter[index] = static_cast<std::uint8_t>(0xc0U + index);
    }
    return identity::Result<identity::SecretBuffer>::Success(
        identity::SecretBuffer(exporter));
  }

 private:
  tls::ValidatedEd25519PublicKey peer_public_key_;
};

inline std::unique_ptr<session::PairingAdmissionLease> AdmitForTest(
    const identity::PublicKey& local_key, const identity::PublicKey& peer_key,
    const tls::Role local_role, const std::uint8_t connection_value,
    const std::uint64_t now_ms = 1'000,
    const std::uint64_t duration_ms = session::kMaximumPairingWindowMs) {
  session::PairingAdmissionController controller;
  if (!controller.OpenWindow(now_ms, duration_ms)) {
    return nullptr;
  }
  session::AttemptHandle connection_id{};
  connection_id.fill(connection_value);
  session::SourceToken source{};
  source.fill(connection_value);
  session::PairingAdmissionResult admitted =
      controller.Admit(session::PairingAdmissionRequest{
          .connection_id = connection_id,
          .source = source,
          .local_key = local_key,
          .peer_key = peer_key,
          .local_role = local_role,
          .user_initiated = true,
          .now_ms = now_ms,
      });
  return std::move(admitted.lease);
}

inline session::NegotiationOffer InitiatorOffer() {
  return {
      .versions = {{1, 0}, {1, 0}},
      .offered_capabilities = {0x0001'0001U, 0x0002'0001U},
      .required_capabilities = {0x0001'0001U},
      .receive_limits = {1'048'576, 67'108'864, 32},
  };
}

inline session::NegotiationOffer ResponderOffer() {
  return {
      .versions = {{1, 0}, {1, 0}},
      .offered_capabilities = {0x0001'0001U, 0x0002'0001U},
      .required_capabilities = {0x0001'0001U},
      .receive_limits = {524'288, 33'554'432, 16},
  };
}

}  // namespace session_test

#endif  // XNN_TRANSFER_TESTS_SESSION_TEST_SUPPORT_HPP_
