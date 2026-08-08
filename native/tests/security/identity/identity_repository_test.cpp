#include "xnn_transfer/core/security/identity/identity_repository.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xnn_transfer/core/security/identity/crypto.hpp"
#include "xnn_transfer/core/security/identity/protected_store.hpp"
#include "xnn_transfer/core/security/identity/secret_buffer.hpp"
#include "xnn_transfer/core/security/identity/types.hpp"

namespace {

using xnn_transfer::core::security::identity::DeviceId;
using xnn_transfer::core::security::identity::ErrorCode;
using xnn_transfer::core::security::identity::IdentityCrypto;
using xnn_transfer::core::security::identity::IdentityRepository;
using xnn_transfer::core::security::identity::kMaxPeerRecords;
using xnn_transfer::core::security::identity::kMaxProtectedItemPayloadSize;
using xnn_transfer::core::security::identity::Mac;
using xnn_transfer::core::security::identity::OpenSslIdentityCrypto;
using xnn_transfer::core::security::identity::PeerCommit;
using xnn_transfer::core::security::identity::PeerPublicKeyValidator;
using xnn_transfer::core::security::identity::ProtectedItem;
using xnn_transfer::core::security::identity::ProtectedItemId;
using xnn_transfer::core::security::identity::ProtectedItemMetadata;
using xnn_transfer::core::security::identity::ProtectedStore;
using xnn_transfer::core::security::identity::PublicKey;
using xnn_transfer::core::security::identity::ResetOutcome;
using xnn_transfer::core::security::identity::Result;
using xnn_transfer::core::security::identity::SecretBuffer;
using xnn_transfer::core::security::identity::StoreId;
using xnn_transfer::core::security::identity::TrustState;

int failures = 0;

void Expect(const bool condition, const std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

template <typename T>
void ExpectError(const Result<T>& result, const ErrorCode expected,
                 const std::string_view message) {
  Expect(!result.ok() && result.error() == expected, message);
}

std::uint32_t ReadU32(const std::span<const std::uint8_t> bytes,
                      const std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value = static_cast<std::uint32_t>(
        (value << 8U) | static_cast<std::uint32_t>(bytes[offset + index]));
  }
  return value;
}

void WriteU16(const std::span<std::uint8_t> output, const std::size_t offset,
              const std::uint16_t value) {
  output[offset] = static_cast<std::uint8_t>(value >> 8U);
  output[offset + 1] = static_cast<std::uint8_t>(value);
}

void WriteU32(const std::span<std::uint8_t> output, const std::size_t offset,
              const std::uint32_t value) {
  output[offset] = static_cast<std::uint8_t>(value >> 24U);
  output[offset + 1] = static_cast<std::uint8_t>(value >> 16U);
  output[offset + 2] = static_cast<std::uint8_t>(value >> 8U);
  output[offset + 3] = static_cast<std::uint8_t>(value);
}

std::uint8_t Nibble(const char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<std::uint8_t>(value - '0');
  }
  return static_cast<std::uint8_t>(value - 'a' + 10);
}

template <std::size_t Size>
std::array<std::uint8_t, Size> Hex(const std::string_view encoded) {
  std::array<std::uint8_t, Size> output{};
  Expect(encoded.size() == Size * 2, "hex fixture has the expected size");
  for (std::size_t index = 0; index < Size; ++index) {
    output[index] = static_cast<std::uint8_t>((Nibble(encoded[index * 2]) << 4U) |
                                              Nibble(encoded[(index * 2) + 1]));
  }
  return output;
}

class FakeProtectedStore final : public ProtectedStore {
 public:
  Result<std::vector<ProtectedItemMetadata>> Enumerate() override {
    if (next_enumerate_error_ != ErrorCode::kNone) {
      const ErrorCode error = std::exchange(next_enumerate_error_, ErrorCode::kNone);
      return Result<std::vector<ProtectedItemMetadata>>::Failure(error);
    }
    std::function<void()> callback = std::move(next_enumerate_callback_);
    if (callback) {
      callback();
    }
    std::vector<ProtectedItemMetadata> metadata;
    metadata.reserve(items_.size());
    for (const auto& [id, item] : items_) {
      metadata.push_back(ProtectedItemMetadata{id, item.revision});
    }
    return Result<std::vector<ProtectedItemMetadata>>::Success(std::move(metadata));
  }

  Result<std::optional<ProtectedItem>> Get(const ProtectedItemId& item_id) override {
    ++get_attempts_[item_id];
    if (next_get_error_ != ErrorCode::kNone) {
      const ErrorCode error = std::exchange(next_get_error_, ErrorCode::kNone);
      return Result<std::optional<ProtectedItem>>::Failure(error);
    }
    const auto iterator = items_.find(item_id);
    if (iterator == items_.end()) {
      return Result<std::optional<ProtectedItem>>::Success(std::nullopt);
    }
    return Result<std::optional<ProtectedItem>>::Success(ProtectedItem(
        iterator->second.revision, SecretBuffer(iterator->second.payload.bytes())));
  }

  Result<void> CompareExchangePut(const ProtectedItemId& item_id,
                                  const std::optional<std::uint64_t> expected_revision,
                                  ProtectedItem replacement) override {
    ++put_attempts_;
    if (next_put_error_ != ErrorCode::kNone) {
      const ErrorCode error = std::exchange(next_put_error_, ErrorCode::kNone);
      return Result<void>::Failure(error);
    }
    if (item_id.empty() || item_id.size() > 128 ||
        replacement.payload.size() > kMaxProtectedItemPayloadSize) {
      return Result<void>::Failure(ErrorCode::kCapacityExceeded);
    }

    auto iterator = items_.find(item_id);
    if (conflict_on_create_ && !expected_revision.has_value() &&
        iterator == items_.end()) {
      conflict_on_create_ = false;
      max_payload_size_ = std::max(max_payload_size_, replacement.payload.size());
      items_.emplace(item_id, ProtectedItem(replacement.revision,
                                            SecretBuffer(replacement.payload.bytes())));
      return Result<void>::Failure(ErrorCode::kRevisionConflict);
    }

    const bool expected_absent = !expected_revision.has_value() &&
                                 iterator == items_.end() && replacement.revision == 1;
    const bool expected_present =
        expected_revision.has_value() && iterator != items_.end() &&
        iterator->second.revision == *expected_revision &&
        *expected_revision != std::numeric_limits<std::uint64_t>::max() &&
        replacement.revision == *expected_revision + 1;
    if (!expected_absent && !expected_present) {
      return Result<void>::Failure(ErrorCode::kRevisionConflict);
    }

    max_payload_size_ = std::max(max_payload_size_, replacement.payload.size());
    if (iterator == items_.end()) {
      items_.emplace(item_id, std::move(replacement));
    } else {
      iterator->second = std::move(replacement);
    }
    return Result<void>::Success();
  }

  Result<void> CompareExchangeDelete(const ProtectedItemId& item_id,
                                     const std::uint64_t expected_revision) override {
    ++delete_attempts_;
    if (next_delete_error_ != ErrorCode::kNone) {
      const ErrorCode error = std::exchange(next_delete_error_, ErrorCode::kNone);
      return Result<void>::Failure(error);
    }
    const auto iterator = items_.find(item_id);
    if (iterator == items_.end() || iterator->second.revision != expected_revision) {
      return Result<void>::Failure(ErrorCode::kRevisionConflict);
    }
    items_.erase(iterator);
    return Result<void>::Success();
  }

  void FailNextEnumerate(const ErrorCode error) { next_enumerate_error_ = error; }
  void FailNextGet(const ErrorCode error) { next_get_error_ = error; }
  void FailNextPut(const ErrorCode error) { next_put_error_ = error; }
  void FailNextDelete(const ErrorCode error) { next_delete_error_ = error; }
  void ConflictOnCreate() { conflict_on_create_ = true; }
  void OnNextEnumerate(std::function<void()> callback) {
    next_enumerate_callback_ = std::move(callback);
  }

  [[nodiscard]] std::size_t item_count() const noexcept { return items_.size(); }
  [[nodiscard]] std::size_t put_attempts() const noexcept { return put_attempts_; }
  [[nodiscard]] std::size_t delete_attempts() const noexcept {
    return delete_attempts_;
  }
  [[nodiscard]] std::size_t get_attempts(
      const ProtectedItemId& item_id) const noexcept {
    const auto iterator = get_attempts_.find(item_id);
    return iterator == get_attempts_.end() ? 0 : iterator->second;
  }
  [[nodiscard]] std::size_t max_payload_size() const noexcept {
    return max_payload_size_;
  }

  [[nodiscard]] std::vector<ProtectedItemId> PeerItemIds() const {
    std::vector<ProtectedItemId> ids;
    for (const auto& [id, item] : items_) {
      static_cast<void>(item);
      if (id.starts_with("peer/")) {
        ids.push_back(id);
      }
    }
    return ids;
  }

  [[nodiscard]] std::optional<ProtectedItem> CloneItem(
      const ProtectedItemId& item_id) const {
    const auto iterator = items_.find(item_id);
    if (iterator == items_.end()) {
      return std::nullopt;
    }
    return ProtectedItem(iterator->second.revision,
                         SecretBuffer(iterator->second.payload.bytes()));
  }

  void RestoreItem(const ProtectedItemId& item_id, ProtectedItem item) {
    items_.insert_or_assign(item_id, std::move(item));
  }

  void RemoveItem(const ProtectedItemId& item_id) { items_.erase(item_id); }

  void CopyItem(const ProtectedItemId& source, const ProtectedItemId& destination) {
    const auto item = CloneItem(source);
    if (item.has_value()) {
      RestoreItem(destination,
                  ProtectedItem(item->revision, SecretBuffer(item->payload.bytes())));
    }
  }

  void CorruptLastByte(const ProtectedItemId& item_id) {
    const auto iterator = items_.find(item_id);
    if (iterator != items_.end() && !iterator->second.payload.empty()) {
      iterator->second.payload.mutable_bytes().back() ^= 0x80U;
    }
  }

  void SetPayloadByte(const ProtectedItemId& item_id, const std::size_t offset,
                      const std::uint8_t value) {
    const auto iterator = items_.find(item_id);
    if (iterator != items_.end() && offset < iterator->second.payload.size()) {
      iterator->second.payload.mutable_bytes()[offset] = value;
    }
  }

  void SetExternalRevision(const ProtectedItemId& item_id,
                           const std::uint64_t revision) {
    const auto iterator = items_.find(item_id);
    if (iterator != items_.end()) {
      iterator->second.revision = revision;
    }
  }

 private:
  std::map<ProtectedItemId, ProtectedItem> items_{};
  std::map<ProtectedItemId, std::size_t> get_attempts_{};
  ErrorCode next_enumerate_error_{ErrorCode::kNone};
  ErrorCode next_get_error_{ErrorCode::kNone};
  ErrorCode next_put_error_{ErrorCode::kNone};
  ErrorCode next_delete_error_{ErrorCode::kNone};
  std::function<void()> next_enumerate_callback_{};
  std::size_t put_attempts_{};
  std::size_t delete_attempts_{};
  std::size_t max_payload_size_{};
  bool conflict_on_create_{};
};

class TestPeerPublicKeyValidator final : public PeerPublicKeyValidator {
 public:
  Result<void> Validate(const PublicKey& public_key) override {
    ++validation_calls_;
    if (std::find(rejected_.begin(), rejected_.end(), public_key) != rejected_.end()) {
      return Result<void>::Failure(ErrorCode::kInvalidArgument);
    }
    return Result<void>::Success();
  }

  void Reject(PublicKey public_key) { rejected_.push_back(std::move(public_key)); }
  void AllowAll() { rejected_.clear(); }

  [[nodiscard]] std::size_t validation_calls() const noexcept {
    return validation_calls_;
  }

 private:
  std::vector<PublicKey> rejected_{};
  std::size_t validation_calls_{};
};

class FaultCrypto final : public IdentityCrypto {
 public:
  Result<SecretBuffer> GenerateSeed() override {
    if (fail_seed_) {
      fail_seed_ = false;
      return Result<SecretBuffer>::Failure(ErrorCode::kEntropyFailure);
    }
    auto result = delegate_.GenerateSeed();
    if (result.ok()) {
      std::copy(result.value().bytes().begin(), result.value().bytes().end(),
                generated_seed_.begin());
      has_generated_seed_ = true;
    }
    return result;
  }

  Result<StoreId> GenerateStoreId() override { return delegate_.GenerateStoreId(); }

  Result<PublicKey> DerivePublicKey(const std::span<const std::uint8_t> seed) override {
    return delegate_.DerivePublicKey(seed);
  }

  Result<DeviceId> DeriveDeviceId(const PublicKey& public_key) override {
    ++device_id_calls_;
    return delegate_.DeriveDeviceId(public_key);
  }

  Result<SecretBuffer> DerivePeerRecordMacKey(const std::span<const std::uint8_t> seed,
                                              const StoreId& store_id) override {
    ++mac_key_derivations_;
    last_store_id_ = store_id;
    if (fail_mac_key_derivation_) {
      fail_mac_key_derivation_ = false;
      return Result<SecretBuffer>::Failure(ErrorCode::kCryptoFailure);
    }
    return delegate_.DerivePeerRecordMacKey(seed, store_id);
  }

  Result<Mac> HmacSha256(const std::span<const std::uint8_t> key,
                         const std::span<const std::uint8_t> message) override {
    if (has_generated_seed_ &&
        std::equal(key.begin(), key.end(), generated_seed_.begin(),
                   generated_seed_.end())) {
      hmac_used_identity_seed_ = true;
    }
    if (fail_hmac_) {
      fail_hmac_ = false;
      return Result<Mac>::Failure(ErrorCode::kCryptoFailure);
    }
    return delegate_.HmacSha256(key, message);
  }

  void FailSeed() { fail_seed_ = true; }
  void FailMacKeyDerivation() { fail_mac_key_derivation_ = true; }
  void FailHmac() { fail_hmac_ = true; }

  [[nodiscard]] bool hmac_used_identity_seed() const noexcept {
    return hmac_used_identity_seed_;
  }
  [[nodiscard]] std::size_t mac_key_derivations() const noexcept {
    return mac_key_derivations_;
  }
  [[nodiscard]] std::size_t device_id_calls() const noexcept {
    return device_id_calls_;
  }

 private:
  OpenSslIdentityCrypto delegate_{};
  std::array<std::uint8_t, 32> generated_seed_{};
  StoreId last_store_id_{};
  std::size_t mac_key_derivations_{};
  std::size_t device_id_calls_{};
  bool has_generated_seed_{};
  bool hmac_used_identity_seed_{};
  bool fail_seed_{};
  bool fail_mac_key_derivation_{};
  bool fail_hmac_{};
};

PublicKey PublicFromIndex(OpenSslIdentityCrypto& crypto, const std::uint16_t value) {
  std::array<std::uint8_t, 32> seed{};
  seed[0] = static_cast<std::uint8_t>(value);
  seed[1] = static_cast<std::uint8_t>(value >> 8U);
  seed.back() = 0xa5U;
  const auto result = crypto.DerivePublicKey(seed);
  Expect(result.ok(), "deterministic test seed derives a public key");
  return result.ok() ? result.value() : PublicKey{};
}

SecretBuffer RemoveRootSeed(const SecretBuffer& root_item) {
  std::vector<std::uint8_t> bytes(root_item.bytes().begin(), root_item.bytes().end());
  constexpr std::size_t kEnvelopeSize = 12;
  constexpr std::size_t kFieldHeaderSize = 6;
  constexpr std::size_t kSeedSize = 32;
  constexpr std::size_t kSeedFieldSize = kFieldHeaderSize + kSeedSize;
  const std::uint32_t body_size = ReadU32(bytes, 8);
  bytes.erase(
      bytes.begin() + static_cast<std::ptrdiff_t>(kEnvelopeSize),
      bytes.begin() + static_cast<std::ptrdiff_t>(kEnvelopeSize + kSeedFieldSize));
  WriteU16(bytes, 6, 4);
  WriteU32(bytes, 8, body_size - static_cast<std::uint32_t>(kSeedFieldSize));
  return SecretBuffer(bytes);
}

void TestOpenSslCryptoAndSecretBuffer() {
  OpenSslIdentityCrypto crypto;
  const auto seed = Hex<32>(
      "9d61b19deffd5a60ba844af492ec2cc4"
      "4449c5697b326919703bac031cae7f60");
  const auto expected_public = Hex<32>(
      "d75a980182b10ab7d54bfed3c964073a"
      "0ee172f3daa62325af021a68f707511a");
  const auto public_result = crypto.DerivePublicKey(seed);
  Expect(public_result.ok() && public_result.value() == expected_public,
         "OpenSSL derives the RFC 8032 Ed25519 public key");

  const auto expected_device = Hex<32>(
      "c503a982b3cc915bd6366c4f6e9e37b0"
      "8df30a150bf061f4a91693c6f9c44c89");
  const auto device_result = crypto.DeriveDeviceId(expected_public);
  Expect(device_result.ok() && device_result.value() == expected_device,
         "device identifier uses the ADR 0002 canonical object");

  std::array<std::uint8_t, 20> hmac_key{};
  hmac_key.fill(0x0bU);
  constexpr std::string_view kMessage = "Hi There";
  const std::span<const std::uint8_t> message(
      reinterpret_cast<const std::uint8_t*>(kMessage.data()), kMessage.size());
  const auto expected_hmac = Hex<32>(
      "b0344c61d8db38535ca8afceaf0bf12b"
      "881dc200c9833da726e9376c2e32cff7");
  const auto hmac_result = crypto.HmacSha256(hmac_key, message);
  Expect(hmac_result.ok() && hmac_result.value() == expected_hmac,
         "OpenSSL HMAC-SHA256 matches RFC 4231");

  StoreId first_store{};
  StoreId second_store{};
  first_store.fill(0x11U);
  second_store.fill(0x22U);
  auto first_mac_key = crypto.DerivePeerRecordMacKey(seed, first_store);
  auto second_mac_key = crypto.DerivePeerRecordMacKey(seed, second_store);
  Expect(
      first_mac_key.ok() && second_mac_key.ok() &&
          first_mac_key.value().bytes().size() == 32 &&
          !std::equal(first_mac_key.value().bytes().begin(),
                      first_mac_key.value().bytes().end(), seed.begin(), seed.end()) &&
          !std::equal(first_mac_key.value().bytes().begin(),
                      first_mac_key.value().bytes().end(),
                      second_mac_key.value().bytes().begin(),
                      second_mac_key.value().bytes().end()),
      "record MAC keys are domain-separated from the seed and store id");

  SecretBuffer original(seed);
  SecretBuffer moved(std::move(original));
  Expect(original.empty() && moved.size() == seed.size(),
         "secret move transfers ownership and clears the source");
  moved.clear();
  Expect(moved.empty(), "explicit secret clear removes active bytes");
}

void TestSeparateItemsAndScopedSeed() {
  OpenSslIdentityCrypto crypto;
  TestPeerPublicKeyValidator validator;
  FakeProtectedStore store;
  IdentityRepository repository(store, crypto, validator);
  Expect(repository.Open().ok() && repository.ready() && store.item_count() == 1,
         "empty store creates one root item");

  bool consumer_called = false;
  const auto use_result =
      repository.UseIdentitySeed([&consumer_called, &crypto, &repository](
                                     const std::span<const std::uint8_t> seed) {
        consumer_called = true;
        const auto public_result = crypto.DerivePublicKey(seed);
        if (!public_result.ok() || repository.root_public_key() == nullptr ||
            public_result.value() != *repository.root_public_key()) {
          return Result<void>::Failure(ErrorCode::kCryptoFailure);
        }
        return Result<void>::Success();
      });
  Expect(use_result.ok() && consumer_called,
         "identity seed is available only through the scoped consumer");
  const std::size_t root_gets_after_first_use = store.get_attempts("root");
  consumer_called = false;
  const auto second_use_result = repository.UseIdentitySeed(
      [&consumer_called](const std::span<const std::uint8_t> seed) {
        consumer_called = true;
        return seed.size() == 32 ? Result<void>::Success()
                                 : Result<void>::Failure(ErrorCode::kCryptoFailure);
      });
  Expect(second_use_result.ok() && consumer_called &&
             store.get_attempts("root") == root_gets_after_first_use + 1,
         "each scoped seed use reloads the protected root item");

  store.FailNextGet(ErrorCode::kStorageLocked);
  consumer_called = false;
  ExpectError(repository.UseIdentitySeed(
                  [&consumer_called](const std::span<const std::uint8_t>) {
                    consumer_called = true;
                    return Result<void>::Success();
                  }),
              ErrorCode::kStorageLocked,
              "scoped seed use observes a new protected-store read failure");
  Expect(!consumer_called,
         "protected-store read failure prevents the scoped seed callback");

  const auto root_backup = store.CloneItem("root");
  Expect(root_backup.has_value(), "scoped seed fixture exposes the root item");
  if (!root_backup.has_value()) {
    return;
  }
  store.SetPayloadByte("root", 4, 2);
  consumer_called = false;
  ExpectError(repository.UseIdentitySeed(
                  [&consumer_called](const std::span<const std::uint8_t>) {
                    consumer_called = true;
                    return Result<void>::Success();
                  }),
              ErrorCode::kUnsupportedSchema,
              "scoped seed use observes a newly corrupt protected root");
  Expect(!consumer_called, "corrupt root never reaches the scoped seed callback");
  const std::size_t put_attempts_before_corrupt_commit = store.put_attempts();
  ExpectError(
      repository.CommitPeer(PeerCommit{PublicFromIndex(crypto, 3), 1, "Corrupt root"}),
      ErrorCode::kUnsupportedSchema,
      "MAC-bearing commit reloads and validates the protected root");
  Expect(store.put_attempts() == put_attempts_before_corrupt_commit,
         "corrupt root prevents peer persistence");
  const DeviceId root_before_corruption = *repository.root_device_id();
  const std::size_t delete_attempts_before_corrupt_cleanup = store.delete_attempts();
  ExpectError(repository.Reset(), ErrorCode::kUnsupportedSchema,
              "explicit reset does not overwrite a newly corrupt root");
  ExpectError(repository.CleanupStaleItems(), ErrorCode::kUnsupportedSchema,
              "stale-item cleanup validates the live root first");
  Expect(*repository.root_device_id() == root_before_corruption &&
             store.put_attempts() == put_attempts_before_corrupt_commit &&
             store.delete_attempts() == delete_attempts_before_corrupt_cleanup,
         "corrupt-root failures leave identity and durable items unchanged");
  store.RestoreItem("root", ProtectedItem(root_backup->revision,
                                          SecretBuffer(root_backup->payload.bytes())));

  const auto first =
      repository.CommitPeer(PeerCommit{PublicFromIndex(crypto, 1), 1, "First"});
  const auto second =
      repository.CommitPeer(PeerCommit{PublicFromIndex(crypto, 2), 1, "Second"});
  Expect(first.ok() && second.ok() && repository.peers().size() == 2 &&
             store.item_count() == 3,
         "root and each committed peer use independent protected items");
  Expect(store.max_payload_size() <= kMaxProtectedItemPayloadSize,
         "every opaque item remains within the protected-store cap");

  IdentityRepository restarted(store, crypto, validator);
  Expect(restarted.Open().ok() && restarted.peers().size() == 2,
         "separate peer items survive repository restart");
}

void TestPeerCasAndFaultBoundaries() {
  OpenSslIdentityCrypto crypto;
  TestPeerPublicKeyValidator validator;
  FakeProtectedStore store;
  IdentityRepository writer(store, crypto, validator);
  IdentityRepository stale(store, crypto, validator);
  Expect(writer.Open().ok() && stale.Open().ok(),
         "CAS fixture opens two repository views");

  const PublicKey first_key = PublicFromIndex(crypto, 10);
  const auto first = writer.CommitPeer(PeerCommit{first_key, 1, "Office"});
  Expect(first.ok() && writer.peers().size() == 1, "peer commit creates revision one");
  const auto conflict = stale.CommitPeer(PeerCommit{first_key, 1, "Stale"});
  ExpectError(conflict, ErrorCode::kRevisionConflict,
              "concurrent create of one peer item reports CAS conflict");
  Expect(stale.peers().empty(),
         "peer create conflict leaves stale in-memory state unchanged");
  Expect(stale.Refresh().ok() && stale.peers().size() == 1,
         "refresh adopts the committed peer item");

  const PublicKey rotated_key = PublicFromIndex(crypto, 11);
  const auto rotated = writer.RotatePeer(first.value(), rotated_key, 1);
  Expect(rotated.ok() && writer.peers()[0].record_revision == 2 &&
             writer.peers()[0].tombstones.size() == 1,
         "rotation CAS replaces the pin and retains the old key");

  store.FailNextPut(ErrorCode::kStorageUnavailable);
  const auto failed_rotation =
      writer.RotatePeer(rotated.value(), PublicFromIndex(crypto, 12), 2);
  ExpectError(failed_rotation, ErrorCode::kStorageUnavailable,
              "peer put failure is preserved");
  Expect(writer.peers()[0].record_revision == 2 &&
             writer.peers()[0].public_key == rotated_key,
         "failed peer put leaves in-memory state unchanged");

  Expect(writer.RevokePeer(rotated.value()).ok() &&
             writer.peers()[0].trust_state == TrustState::kRevoked,
         "revocation CAS persists the revoked state");
  store.FailNextDelete(ErrorCode::kStorageLocked);
  ExpectError(writer.ForgetPeer(rotated.value()), ErrorCode::kStorageLocked,
              "peer delete failure is preserved");
  Expect(writer.peers().size() == 1,
         "failed peer delete leaves in-memory state unchanged");
  Expect(writer.ForgetPeer(rotated.value()).ok() && writer.peers().empty(),
         "forget deletes only a revoked peer item");
}

void TestResetCommitPointAndCleanup() {
  OpenSslIdentityCrypto crypto;
  TestPeerPublicKeyValidator validator;
  FakeProtectedStore store;
  IdentityRepository repository(store, crypto, validator);
  IdentityRepository stale(store, crypto, validator);
  Expect(repository.Open().ok() && stale.Open().ok(),
         "reset fixture opens current and stale views");
  const DeviceId old_root = *repository.root_device_id();
  const auto peer =
      repository.CommitPeer(PeerCommit{PublicFromIndex(crypto, 20), 1, "Reset peer"});
  Expect(peer.ok(), "reset fixture commits a peer");
  const auto old_peer_items = store.PeerItemIds();
  Expect(old_peer_items.size() == 1, "reset fixture has one old-generation peer");
  if (old_peer_items.size() != 1) {
    return;
  }

  store.FailNextDelete(ErrorCode::kStorageUnavailable);
  const auto reset = repository.Reset();
  Expect(reset.ok() && !reset.value().cleanup_complete && repository.ready() &&
             repository.peers().empty() && *repository.root_device_id() != old_root,
         "new root commits reset even when stale-item cleanup must be retried");
  const DeviceId reset_root = *repository.root_device_id();
  Expect(store.CloneItem(old_peer_items[0]).has_value(),
         "cleanup fault leaves the retired-generation peer item");

  const std::size_t put_attempts_before_blocked_reset = store.put_attempts();
  ExpectError(repository.Reset(), ErrorCode::kInvalidState,
              "pending retired-generation cleanup blocks another reset");
  Expect(*repository.root_device_id() == reset_root &&
             store.put_attempts() == put_attempts_before_blocked_reset,
         "blocked reset neither replaces the identity nor writes the root");

  IdentityRepository restarted(store, crypto, validator);
  Expect(restarted.Open().ok() && restarted.peers().empty() &&
             *restarted.root_device_id() == *repository.root_device_id(),
         "reopen restores the new generation and pending cleanup target");
  const auto new_peer = restarted.CommitPeer(
      PeerCommit{PublicFromIndex(crypto, 21), 1, "New generation"});
  Expect(new_peer.ok(), "new generation can persist a peer while cleanup is pending");
  const auto all_peer_items = store.PeerItemIds();
  ProtectedItemId new_peer_item;
  for (const ProtectedItemId& item_id : all_peer_items) {
    if (item_id != old_peer_items[0]) {
      new_peer_item = item_id;
    }
  }
  Expect(all_peer_items.size() == 2 && !new_peer_item.empty(),
         "store contains one retired and one current-generation peer");

  const std::size_t deletes_before_stale_cleanup = store.delete_attempts();
  ExpectError(stale.CleanupStaleItems(), ErrorCode::kRevisionConflict,
              "old repository cleanup cannot act after another process resets");
  Expect(store.delete_attempts() == deletes_before_stale_cleanup &&
             store.CloneItem(new_peer_item).has_value(),
         "stale cleanup never deletes a new-generation peer");

  Expect(restarted.CleanupStaleItems().ok() &&
             !store.CloneItem(old_peer_items[0]).has_value() &&
             store.CloneItem(new_peer_item).has_value() &&
             store.PeerItemIds().size() == 1,
         "reopened cleanup deletes only the persisted retired generation");

  IdentityRepository after_cleanup(store, crypto, validator);
  Expect(after_cleanup.Open().ok() && after_cleanup.peers().size() == 1,
         "current-generation peer survives cleanup and restart");

  const DeviceId current_root = *after_cleanup.root_device_id();
  store.FailNextPut(ErrorCode::kPermissionDenied);
  const auto failed_reset = after_cleanup.Reset();
  ExpectError(failed_reset, ErrorCode::kPermissionDenied,
              "root CAS failure prevents reset");
  Expect(after_cleanup.ready() && *after_cleanup.root_device_id() == current_root &&
             store.CloneItem(new_peer_item).has_value(),
         "failed root CAS keeps the current identity and peer active");
}

void TestCleanupRaceKeepsNewGenerationPeers() {
  OpenSslIdentityCrypto crypto;
  TestPeerPublicKeyValidator validator;
  FakeProtectedStore store;
  IdentityRepository current(store, crypto, validator);
  Expect(current.Open().ok(), "cleanup race fixture initializes");
  const auto first_reset = current.Reset();
  Expect(first_reset.ok() && first_reset.value().cleanup_complete,
         "cleanup race fixture establishes an empty retired generation");

  IdentityRepository stale(store, crypto, validator);
  Expect(stale.Open().ok(), "cleanup race fixture opens a second repository");

  Result<ResetOutcome> raced_reset =
      Result<ResetOutcome>::Failure(ErrorCode::kInvalidState);
  Result<DeviceId> new_peer = Result<DeviceId>::Failure(ErrorCode::kInvalidState);
  store.OnNextEnumerate([&current, &crypto, &raced_reset, &new_peer]() {
    raced_reset = current.Reset();
    if (raced_reset.ok()) {
      new_peer = current.CommitPeer(
          PeerCommit{PublicFromIndex(crypto, 22), 1, "Raced generation"});
    }
  });

  const auto stale_cleanup = stale.CleanupStaleItems();
  const auto peer_items = store.PeerItemIds();
  Expect(stale_cleanup.ok() && raced_reset.ok() && new_peer.ok() &&
             peer_items.size() == 1 && store.CloneItem(peer_items[0]).has_value(),
         "stale cleanup retains a peer created after its root validation");

  IdentityRepository restarted(store, crypto, validator);
  Expect(restarted.Open().ok() && restarted.peers().size() == 1,
         "raced new-generation peer remains trusted after restart");
}

void TestIdentityLossCorruptionAndStoreBinding() {
  OpenSslIdentityCrypto crypto;
  TestPeerPublicKeyValidator validator;
  FakeProtectedStore store;
  IdentityRepository writer(store, crypto, validator);
  Expect(writer.Open().ok(), "corruption fixture initializes");
  const auto peer =
      writer.CommitPeer(PeerCommit{PublicFromIndex(crypto, 30), 1, "Corrupt peer"});
  Expect(peer.ok(), "corruption fixture commits a peer");
  const auto peer_ids = store.PeerItemIds();
  const auto root_backup = store.CloneItem("root");
  Expect(peer_ids.size() == 1 && root_backup.has_value(),
         "fake store exposes root and peer items");
  if (peer_ids.size() != 1 || !root_backup.has_value()) {
    return;
  }
  const auto peer_backup = store.CloneItem(peer_ids[0]);
  if (!peer_backup.has_value()) {
    Expect(false, "fake store clones the peer item");
    return;
  }

  store.CorruptLastByte(peer_ids[0]);
  IdentityRepository corrupt_peer(store, crypto, validator);
  ExpectError(corrupt_peer.Open(), ErrorCode::kCorruptRecord,
              "peer authenticator corruption fails closed");
  store.RestoreItem(
      peer_ids[0],
      ProtectedItem(peer_backup->revision, SecretBuffer(peer_backup->payload.bytes())));

  ProtectedItemId wrong_id = peer_ids[0];
  wrong_id.back() = wrong_id.back() == '0' ? '1' : '0';
  store.CopyItem(peer_ids[0], wrong_id);
  IdentityRepository wrong_binding(store, crypto, validator);
  ExpectError(wrong_binding.Open(), ErrorCode::kCorruptRecord,
              "peer payload copied to another store item fails binding");
  store.RemoveItem(wrong_id);

  store.SetExternalRevision("root", root_backup->revision + 1);
  IdentityRepository rollback(store, crypto, validator);
  ExpectError(rollback.Open(), ErrorCode::kRollbackDetected,
              "root payload and protected revision mismatch detects rollback");
  store.RestoreItem("root", ProtectedItem(root_backup->revision,
                                          SecretBuffer(root_backup->payload.bytes())));

  store.SetPayloadByte("root", 4, 2);
  IdentityRepository unsupported(store, crypto, validator);
  ExpectError(unsupported.Open(), ErrorCode::kUnsupportedSchema,
              "unknown root schema fails closed without migration");
  store.RestoreItem("root", ProtectedItem(root_backup->revision,
                                          SecretBuffer(root_backup->payload.bytes())));

  store.RestoreItem("root", ProtectedItem(root_backup->revision,
                                          RemoveRootSeed(root_backup->payload)));
  IdentityRepository missing_seed(store, crypto, validator);
  ExpectError(missing_seed.Open(), ErrorCode::kIdentityLoss,
              "root metadata without the seed is identity loss");
  store.RestoreItem("root", ProtectedItem(root_backup->revision,
                                          SecretBuffer(root_backup->payload.bytes())));

  store.RemoveItem("root");
  IdentityRepository missing_root(store, crypto, validator);
  ExpectError(missing_root.Open(), ErrorCode::kIdentityLoss,
              "peer items without the root never generate a replacement identity");
}

void TestPeerPublicKeyValidation() {
  FaultCrypto crypto;
  OpenSslIdentityCrypto key_crypto;
  TestPeerPublicKeyValidator validator;
  FakeProtectedStore store;
  IdentityRepository repository(store, crypto, validator);
  Expect(repository.Open().ok(), "invalid-key fixture initializes");

  const std::array invalid_keys{
      Hex<32>("edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"),
      Hex<32>("0100000000000000000000000000000000000000000000000000000000000000"),
      Hex<32>("ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"),
      Hex<32>("16a567fe7d4ef5482ab4012c369bf8c5f11e8d0c2559dcda50fde59708f8aee5"),
  };
  const std::size_t initial_put_attempts = store.put_attempts();
  const std::size_t initial_device_id_calls = crypto.device_id_calls();
  for (const PublicKey& invalid_key : invalid_keys) {
    validator.Reject(invalid_key);
    ExpectError(repository.CommitPeer(PeerCommit{invalid_key, 1, "Invalid"}),
                ErrorCode::kInvalidArgument,
                "invalid peer key is rejected before durable trust");
  }
  Expect(store.put_attempts() == initial_put_attempts &&
             crypto.device_id_calls() == initial_device_id_calls &&
             repository.peers().empty(),
         "invalid commit keys are rejected before derivation or store access");

  validator.AllowAll();
  const auto committed =
      repository.CommitPeer(PeerCommit{PublicFromIndex(key_crypto, 40), 1, "Valid"});
  Expect(committed.ok(), "valid peer key reaches durable trust");
  validator.Reject(invalid_keys.back());
  const std::size_t before_rotation = store.put_attempts();
  ExpectError(repository.RotatePeer(committed.value(), invalid_keys.back(), 1),
              ErrorCode::kInvalidArgument,
              "invalid rotation key is rejected before persistence");
  Expect(store.put_attempts() == before_rotation &&
             repository.peers()[0].record_revision == 1,
         "invalid rotation leaves durable and in-memory pins unchanged");

  TestPeerPublicKeyValidator accepting_validator;
  FakeProtectedStore persisted_store;
  IdentityRepository unsafe_writer(persisted_store, key_crypto, accepting_validator);
  Expect(
      unsafe_writer.Open().ok() &&
          unsafe_writer.CommitPeer(PeerCommit{invalid_keys[1], 1, "Persisted invalid"})
              .ok(),
      "fixture persists a key through an accepting test validator");
  TestPeerPublicKeyValidator rejecting_validator;
  rejecting_validator.Reject(invalid_keys[1]);
  IdentityRepository strict_reader(persisted_store, key_crypto, rejecting_validator);
  ExpectError(strict_reader.Open(), ErrorCode::kCorruptRecord,
              "load revalidates persisted peer keys and fails closed");
}

void TestMacDerivationAndSecretLifetime() {
  FaultCrypto crypto;
  OpenSslIdentityCrypto key_crypto;
  TestPeerPublicKeyValidator validator;
  FakeProtectedStore store;
  IdentityRepository repository(store, crypto, validator);
  Expect(repository.Open().ok(), "MAC derivation fixture initializes");
  const auto peer = repository.CommitPeer(
      PeerCommit{PublicFromIndex(key_crypto, 50), 1, "Derived MAC"});
  Expect(peer.ok() && crypto.mac_key_derivations() != 0 &&
             !crypto.hmac_used_identity_seed(),
         "peer MAC uses a derived key instead of the Ed25519 seed");

  const auto consumer_failure =
      repository.UseIdentitySeed([](const std::span<const std::uint8_t> seed) {
        Expect(seed.size() == 32, "scoped consumer receives one Ed25519 seed");
        return Result<void>::Failure(ErrorCode::kCryptoFailure);
      });
  ExpectError(consumer_failure, ErrorCode::kCryptoFailure,
              "scoped seed consumer failure is propagated");

  FaultCrypto derivation_failure;
  FakeProtectedStore derivation_store;
  IdentityRepository no_mac_key(derivation_store, derivation_failure, validator);
  Expect(no_mac_key.Open().ok(), "MAC-key fault fixture initializes");
  derivation_failure.FailMacKeyDerivation();
  const std::size_t before_put = derivation_store.put_attempts();
  ExpectError(no_mac_key.CommitPeer(
                  PeerCommit{PublicFromIndex(key_crypto, 51), 1, "No MAC key"}),
              ErrorCode::kCryptoFailure,
              "record-MAC key derivation failure prevents peer persistence");
  Expect(derivation_store.put_attempts() == before_put && no_mac_key.peers().empty(),
         "MAC-key derivation fault leaves trust state unchanged");

  FaultCrypto hmac_failure;
  FakeProtectedStore hmac_store;
  IdentityRepository no_mac(hmac_store, hmac_failure, validator);
  Expect(no_mac.Open().ok(), "HMAC fault fixture initializes");
  hmac_failure.FailHmac();
  const std::size_t before_hmac_put = hmac_store.put_attempts();
  ExpectError(
      no_mac.CommitPeer(PeerCommit{PublicFromIndex(key_crypto, 52), 1, "No HMAC"}),
      ErrorCode::kCryptoFailure, "HMAC failure prevents peer persistence");
  Expect(hmac_store.put_attempts() == before_hmac_put && no_mac.peers().empty(),
         "HMAC fault leaves trust state unchanged");
}

void TestItemCapAtMaximumPeerCount() {
  OpenSslIdentityCrypto crypto;
  TestPeerPublicKeyValidator validator;
  FakeProtectedStore store;
  IdentityRepository repository(store, crypto, validator);
  Expect(repository.Open().ok(), "item-cap fixture initializes");

  std::optional<DeviceId> rotating_peer;
  for (std::size_t index = 0; index < kMaxPeerRecords; ++index) {
    const auto result = repository.CommitPeer(
        PeerCommit{PublicFromIndex(crypto, static_cast<std::uint16_t>(index)), 1,
                   std::string(96, 'x')});
    if (!result.ok()) {
      Expect(false, "every peer up to the configured maximum persists");
      return;
    }
    if (index == 0) {
      rotating_peer = result.value();
    }
  }
  if (!rotating_peer.has_value()) {
    Expect(false, "item-cap fixture retains the rotating peer id");
    return;
  }
  for (std::uint64_t rotation = 1; rotation <= 8; ++rotation) {
    const auto result = repository.RotatePeer(
        *rotating_peer,
        PublicFromIndex(crypto, static_cast<std::uint16_t>(400U + rotation)), rotation);
    if (!result.ok()) {
      Expect(false, "maximum tombstone history remains within one item");
      return;
    }
    rotating_peer = result.value();
  }
  Expect(repository.peers().size() == kMaxPeerRecords &&
             store.item_count() == kMaxPeerRecords + 1 &&
             store.max_payload_size() <= kMaxProtectedItemPayloadSize,
         "maximum peer set maps to bounded root plus per-peer items");
  ExpectError(
      repository.CommitPeer(PeerCommit{PublicFromIndex(crypto, 300), 1, "Overflow"}),
      ErrorCode::kCapacityExceeded,
      "peer count above the configured maximum fails before store access");
}

void TestStoreAndEntropyFaults() {
  OpenSslIdentityCrypto crypto;
  TestPeerPublicKeyValidator validator;

  FakeProtectedStore denied_store;
  denied_store.FailNextGet(ErrorCode::kPermissionDenied);
  IdentityRepository denied(denied_store, crypto, validator);
  ExpectError(denied.Open(), ErrorCode::kPermissionDenied,
              "protected-store get error is preserved");

  FakeProtectedStore enumerate_store;
  enumerate_store.FailNextEnumerate(ErrorCode::kStorageUnavailable);
  IdentityRepository unavailable(enumerate_store, crypto, validator);
  ExpectError(unavailable.Open(), ErrorCode::kStorageUnavailable,
              "protected-store enumerate error is preserved");

  FaultCrypto entropy_failure;
  entropy_failure.FailSeed();
  FakeProtectedStore empty_store;
  IdentityRepository no_entropy(empty_store, entropy_failure, validator);
  ExpectError(no_entropy.Open(), ErrorCode::kEntropyFailure,
              "entropy failure prevents root creation");

  FakeProtectedStore raced_store;
  raced_store.ConflictOnCreate();
  IdentityRepository raced(raced_store, crypto, validator);
  Expect(raced.Open().ok() && raced.ready() && raced_store.item_count() == 1,
         "root create conflict loads the unique winning identity");
}

}  // namespace

int main() {
  TestOpenSslCryptoAndSecretBuffer();
  TestSeparateItemsAndScopedSeed();
  TestPeerCasAndFaultBoundaries();
  TestResetCommitPointAndCleanup();
  TestCleanupRaceKeepsNewGenerationPeers();
  TestIdentityLossCorruptionAndStoreBinding();
  TestPeerPublicKeyValidation();
  TestMacDerivationAndSecretLifetime();
  TestItemCapAtMaximumPeerCount();
  TestStoreAndEntropyFaults();

  if (failures != 0) {
    std::cerr << failures << " identity storage assertion(s) failed\n";
    return 1;
  }
  std::cout << "Native identity storage tests passed.\n";
  return 0;
}
