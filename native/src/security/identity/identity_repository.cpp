#include "xnn_transfer/core/security/identity/identity_repository.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <new>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "codec.hpp"

namespace xnn_transfer::core::security::identity {
namespace {

using internal::DecodePeerRecord;
using internal::DecodeRootRecord;
using internal::EncodePeerMacInput;
using internal::EncodePeerRecord;
using internal::EncodeRootRecord;
using internal::RootRecord;
using internal::StoredPeerRecord;

constexpr std::string_view kRootItemId = "root";
constexpr std::string_view kPeerItemPrefix = "peer/";
constexpr std::string_view kPeerMacLabel = "XnnTransfer peer record MAC v1";
constexpr char kHexDigits[] = "0123456789abcdef";
constexpr std::size_t kEncodedStoreIdSize = kStoreIdSize * 2;
constexpr std::size_t kEncodedRecordIdSize = kDeviceIdSize * 2;
constexpr std::size_t kPeerItemIdSize =
    kPeerItemPrefix.size() + kEncodedStoreIdSize + 1 + kEncodedRecordIdSize;

struct ParsedPeerItemId {
  StoreId store_id{};
  DeviceId record_id{};
};

struct RootMetadata {
  std::uint64_t revision{};
  PublicKey public_key{};
  DeviceId device_id{};
  StoreId store_id{};
  std::optional<StoreId> retired_store_id{};
};

template <std::size_t Size>
[[nodiscard]] bool ConstantTimeEqual(
    const std::array<std::uint8_t, Size>& left,
    const std::array<std::uint8_t, Size>& right) noexcept {
  std::uint8_t difference = 0;
  for (std::size_t index = 0; index < Size; ++index) {
    difference = static_cast<std::uint8_t>(
        difference | static_cast<std::uint8_t>(left[index] ^ right[index]));
  }
  return difference == 0;
}

[[nodiscard]] bool IsAllZero(const std::span<const std::uint8_t> value) noexcept {
  std::uint8_t combined = 0;
  for (const std::uint8_t byte : value) {
    combined = static_cast<std::uint8_t>(combined | byte);
  }
  return combined == 0;
}

[[nodiscard]] std::optional<std::uint8_t> DecodeNibble(const char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<std::uint8_t>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<std::uint8_t>(value - 'a' + 10);
  }
  return std::nullopt;
}

template <std::size_t Size>
[[nodiscard]] bool DecodeHex(const std::string_view encoded,
                             std::array<std::uint8_t, Size>& output) {
  if (encoded.size() != Size * 2) {
    return false;
  }
  for (std::size_t index = 0; index < Size; ++index) {
    const auto high = DecodeNibble(encoded[index * 2]);
    const auto low = DecodeNibble(encoded[(index * 2) + 1]);
    if (!high.has_value() || !low.has_value()) {
      return false;
    }
    output[index] = static_cast<std::uint8_t>((static_cast<unsigned>(*high) << 4U) |
                                              static_cast<unsigned>(*low));
  }
  return true;
}

template <std::size_t Size>
void AppendHex(std::string& output, const std::array<std::uint8_t, Size>& bytes) {
  for (const std::uint8_t byte : bytes) {
    output.push_back(kHexDigits[byte >> 4U]);
    output.push_back(kHexDigits[byte & 0x0fU]);
  }
}

[[nodiscard]] Result<ProtectedItemId> BuildPeerItemId(const StoreId& store_id,
                                                      const DeviceId& record_id) {
  try {
    ProtectedItemId item_id;
    item_id.reserve(kPeerItemIdSize);
    item_id.append(kPeerItemPrefix);
    AppendHex(item_id, store_id);
    item_id.push_back('/');
    AppendHex(item_id, record_id);
    if (item_id.size() > kMaxProtectedItemIdBytes) {
      return Result<ProtectedItemId>::Failure(ErrorCode::kCapacityExceeded);
    }
    return Result<ProtectedItemId>::Success(std::move(item_id));
  } catch (const std::bad_alloc&) {
    return Result<ProtectedItemId>::Failure(ErrorCode::kCapacityExceeded);
  }
}

[[nodiscard]] Result<ParsedPeerItemId> ParsePeerItemId(const ProtectedItemId& item_id) {
  if (item_id.size() != kPeerItemIdSize || !item_id.starts_with(kPeerItemPrefix) ||
      item_id[kPeerItemPrefix.size() + kEncodedStoreIdSize] != '/') {
    return Result<ParsedPeerItemId>::Failure(ErrorCode::kCorruptRecord);
  }
  ParsedPeerItemId parsed{};
  const std::string_view encoded(item_id);
  if (!DecodeHex(encoded.substr(kPeerItemPrefix.size(), kEncodedStoreIdSize),
                 parsed.store_id) ||
      !DecodeHex(encoded.substr(kPeerItemPrefix.size() + kEncodedStoreIdSize + 1,
                                kEncodedRecordIdSize),
                 parsed.record_id)) {
    return Result<ParsedPeerItemId>::Failure(ErrorCode::kCorruptRecord);
  }
  return Result<ParsedPeerItemId>::Success(parsed);
}

[[nodiscard]] Result<bool> GenerationHasItems(ProtectedStore& store,
                                              const StoreId& store_id) {
  auto enumerate_result = store.Enumerate();
  if (!enumerate_result.ok()) {
    return Result<bool>::Failure(enumerate_result.error());
  }
  for (const ProtectedItemMetadata& metadata : enumerate_result.value()) {
    if (metadata.item_id == kRootItemId) {
      continue;
    }
    const auto parsed_result = ParsePeerItemId(metadata.item_id);
    if (!parsed_result.ok()) {
      return Result<bool>::Failure(parsed_result.error());
    }
    if (parsed_result.value().store_id == store_id) {
      return Result<bool>::Success(true);
    }
  }
  return Result<bool>::Success(false);
}

[[nodiscard]] Result<RootRecord> MakeRoot(IdentityCrypto& crypto,
                                          const std::uint64_t revision) {
  auto seed_result = crypto.GenerateSeed();
  if (!seed_result.ok()) {
    return Result<RootRecord>::Failure(seed_result.error());
  }
  auto store_id_result = crypto.GenerateStoreId();
  if (!store_id_result.ok()) {
    return Result<RootRecord>::Failure(store_id_result.error());
  }
  if (IsAllZero(store_id_result.value())) {
    return Result<RootRecord>::Failure(ErrorCode::kEntropyFailure);
  }

  RootRecord root{};
  root.revision = revision;
  root.seed = std::move(seed_result).value();
  root.store_id = store_id_result.value();
  auto public_key_result = crypto.DerivePublicKey(root.seed.bytes());
  if (!public_key_result.ok()) {
    return Result<RootRecord>::Failure(public_key_result.error());
  }
  root.public_key = public_key_result.value();
  auto device_id_result = crypto.DeriveDeviceId(root.public_key);
  if (!device_id_result.ok()) {
    return Result<RootRecord>::Failure(device_id_result.error());
  }
  root.device_id = device_id_result.value();
  return Result<RootRecord>::Success(std::move(root));
}

[[nodiscard]] RootMetadata RootMetadataFrom(const RootRecord& root) {
  return RootMetadata{
      .revision = root.revision,
      .public_key = root.public_key,
      .device_id = root.device_id,
      .store_id = root.store_id,
      .retired_store_id = root.retired_store_id,
  };
}

[[nodiscard]] bool RootMatches(const RootRecord& root,
                               const RootMetadata& expected) noexcept {
  if (root.revision != expected.revision ||
      !ConstantTimeEqual(root.public_key, expected.public_key) ||
      !ConstantTimeEqual(root.device_id, expected.device_id) ||
      !ConstantTimeEqual(root.store_id, expected.store_id) ||
      root.retired_store_id.has_value() != expected.retired_store_id.has_value()) {
    return false;
  }
  return !root.retired_store_id.has_value() ||
         ConstantTimeEqual(*root.retired_store_id, *expected.retired_store_id);
}

[[nodiscard]] Result<RootRecord> DecodeAndValidateRoot(IdentityCrypto& crypto,
                                                       const ProtectedItem& root_item) {
  auto decoded_result = DecodeRootRecord(root_item.payload.bytes());
  if (!decoded_result.ok()) {
    return Result<RootRecord>::Failure(decoded_result.error());
  }
  RootRecord root = std::move(decoded_result).value();
  if (root.revision != root_item.revision) {
    return Result<RootRecord>::Failure(ErrorCode::kRollbackDetected);
  }
  if (IsAllZero(root.store_id)) {
    return Result<RootRecord>::Failure(ErrorCode::kCorruptRecord);
  }
  const auto public_key_result = crypto.DerivePublicKey(root.seed.bytes());
  if (!public_key_result.ok()) {
    return Result<RootRecord>::Failure(public_key_result.error());
  }
  if (!ConstantTimeEqual(public_key_result.value(), root.public_key)) {
    return Result<RootRecord>::Failure(ErrorCode::kCorruptRecord);
  }
  const auto device_id_result = crypto.DeriveDeviceId(root.public_key);
  if (!device_id_result.ok()) {
    return Result<RootRecord>::Failure(device_id_result.error());
  }
  if (!ConstantTimeEqual(device_id_result.value(), root.device_id)) {
    return Result<RootRecord>::Failure(ErrorCode::kCorruptRecord);
  }
  return Result<RootRecord>::Success(std::move(root));
}

[[nodiscard]] Result<Mac> ComputePeerMac(IdentityCrypto& crypto, const RootRecord& root,
                                         const StoredPeerRecord& peer) {
  auto input_result = EncodePeerMacInput(peer);
  if (!input_result.ok()) {
    return Result<Mac>::Failure(input_result.error());
  }
  auto key_result = crypto.DerivePeerRecordMacKey(root.seed.bytes(), root.store_id);
  if (!key_result.ok()) {
    return Result<Mac>::Failure(key_result.error());
  }
  SecretBuffer mac_key = std::move(key_result).value();
  try {
    std::vector<std::uint8_t> message;
    message.reserve(kPeerMacLabel.size() + input_result.value().size());
    message.insert(message.end(), kPeerMacLabel.begin(), kPeerMacLabel.end());
    message.insert(message.end(), input_result.value().bytes().begin(),
                   input_result.value().bytes().end());
    auto mac_result = crypto.HmacSha256(mac_key.bytes(), message);
    mac_key.clear();
    return mac_result;
  } catch (const std::bad_alloc&) {
    mac_key.clear();
    return Result<Mac>::Failure(ErrorCode::kCapacityExceeded);
  }
}

[[nodiscard]] Result<void> ValidatePeerAgainstRoot(IdentityCrypto& crypto,
                                                   PeerPublicKeyValidator& validator,
                                                   const RootRecord& root,
                                                   const StoredPeerRecord& stored) {
  const PeerRecord& peer = stored.peer;
  if (!ConstantTimeEqual(stored.store_id, root.store_id) ||
      !ConstantTimeEqual(stored.root_device_id, root.device_id) ||
      IsAllZero(stored.record_id) || peer.record_revision == 0 ||
      peer.rotation_counter != peer.tombstones.size()) {
    return Result<void>::Failure(ErrorCode::kCorruptRecord);
  }

  auto validation_result = validator.Validate(peer.public_key);
  if (!validation_result.ok()) {
    return Result<void>::Failure(validation_result.error() ==
                                         ErrorCode::kInvalidArgument
                                     ? ErrorCode::kCorruptRecord
                                     : validation_result.error());
  }
  for (const PublicKey& tombstone : peer.tombstones) {
    validation_result = validator.Validate(tombstone);
    if (!validation_result.ok()) {
      return Result<void>::Failure(validation_result.error() ==
                                           ErrorCode::kInvalidArgument
                                       ? ErrorCode::kCorruptRecord
                                       : validation_result.error());
    }
  }

  const auto device_id_result = crypto.DeriveDeviceId(peer.public_key);
  if (!device_id_result.ok()) {
    return Result<void>::Failure(device_id_result.error());
  }
  if (!ConstantTimeEqual(device_id_result.value(), peer.device_id)) {
    return Result<void>::Failure(ErrorCode::kCorruptRecord);
  }
  const auto mac_result = ComputePeerMac(crypto, root, stored);
  if (!mac_result.ok()) {
    return Result<void>::Failure(mac_result.error());
  }
  if (!ConstantTimeEqual(mac_result.value(), stored.authenticator)) {
    return Result<void>::Failure(ErrorCode::kCorruptRecord);
  }
  return Result<void>::Success();
}

template <typename StoredPeerRange>
[[nodiscard]] Result<void> ValidateUniquePeerKeys(const StoredPeerRange& peers) {
  for (std::size_t index = 0; index < peers.size(); ++index) {
    const PeerRecord& peer = peers[index].record.peer;
    for (std::size_t other = 0; other < peers.size(); ++other) {
      if (other != index && peers[other].record.peer.public_key == peer.public_key) {
        return Result<void>::Failure(ErrorCode::kCorruptRecord);
      }
      for (const PublicKey& tombstone : peers[other].record.peer.tombstones) {
        if (tombstone == peer.public_key) {
          return Result<void>::Failure(ErrorCode::kCorruptRecord);
        }
      }
    }
    for (const PublicKey& tombstone : peer.tombstones) {
      for (std::size_t other = index + 1; other < peers.size(); ++other) {
        if (std::find(peers[other].record.peer.tombstones.begin(),
                      peers[other].record.peer.tombstones.end(),
                      tombstone) != peers[other].record.peer.tombstones.end()) {
          return Result<void>::Failure(ErrorCode::kCorruptRecord);
        }
      }
    }
  }
  return Result<void>::Success();
}

}  // namespace

struct IdentityRepository::StoredPeer {
  ProtectedItemId item_id{};
  StoredPeerRecord record{};
};

struct IdentityRepository::State {
  RootMetadata root{};
  std::vector<StoredPeer> stored_peers{};
  std::vector<PeerRecord> peer_view{};
};

IdentityRepository::IdentityRepository(ProtectedStore& store, IdentityCrypto& crypto,
                                       PeerPublicKeyValidator& peer_key_validator)
    : store_(&store),
      crypto_(&crypto),
      peer_key_validator_(&peer_key_validator),
      state_(std::make_unique<State>()) {}

IdentityRepository::~IdentityRepository() = default;

Result<void> IdentityRepository::Open() {
  if (ready_) {
    return Result<void>::Failure(ErrorCode::kInvalidState);
  }
  auto root_result = store_->Get(ProtectedItemId(kRootItemId));
  if (!root_result.ok()) {
    return Result<void>::Failure(root_result.error());
  }
  std::optional<ProtectedItem> root_item = std::move(root_result).value();
  if (root_item.has_value()) {
    return LoadFromRoot(std::move(*root_item));
  }

  auto enumerate_result = store_->Enumerate();
  if (!enumerate_result.ok()) {
    return Result<void>::Failure(enumerate_result.error());
  }
  if (enumerate_result.value().empty()) {
    return InitializeEmpty();
  }

  root_result = store_->Get(ProtectedItemId(kRootItemId));
  if (!root_result.ok()) {
    return Result<void>::Failure(root_result.error());
  }
  root_item = std::move(root_result).value();
  if (!root_item.has_value()) {
    return Result<void>::Failure(ErrorCode::kIdentityLoss);
  }
  return LoadFromRoot(std::move(*root_item));
}

Result<void> IdentityRepository::Refresh() {
  if (!ready_) {
    return Result<void>::Failure(ErrorCode::kInvalidState);
  }
  auto root_result = store_->Get(ProtectedItemId(kRootItemId));
  if (!root_result.ok()) {
    return Result<void>::Failure(root_result.error());
  }
  std::optional<ProtectedItem> root_item = std::move(root_result).value();
  if (!root_item.has_value()) {
    return Result<void>::Failure(ErrorCode::kIdentityLoss);
  }
  return LoadFromRoot(std::move(*root_item));
}

Result<void> IdentityRepository::InitializeEmpty() {
  auto root_result = MakeRoot(*crypto_, 1);
  if (!root_result.ok()) {
    return Result<void>::Failure(root_result.error());
  }
  RootRecord root = std::move(root_result).value();
  auto encoded_result = EncodeRootRecord(root);
  if (!encoded_result.ok()) {
    return Result<void>::Failure(encoded_result.error());
  }
  const auto write_result =
      store_->CompareExchangePut(ProtectedItemId(kRootItemId), std::nullopt,
                                 ProtectedItem(1, std::move(encoded_result).value()));
  if (!write_result.ok()) {
    if (write_result.error() != ErrorCode::kRevisionConflict) {
      return Result<void>::Failure(write_result.error());
    }
    auto winner_result = store_->Get(ProtectedItemId(kRootItemId));
    if (!winner_result.ok()) {
      return Result<void>::Failure(winner_result.error());
    }
    std::optional<ProtectedItem> winner = std::move(winner_result).value();
    if (!winner.has_value()) {
      return Result<void>::Failure(ErrorCode::kRevisionConflict);
    }
    return LoadFromRoot(std::move(*winner));
  }

  state_->root = RootMetadataFrom(root);
  root.seed.clear();
  state_->stored_peers.clear();
  state_->peer_view.clear();
  ready_ = true;
  return Result<void>::Success();
}

Result<void> IdentityRepository::LoadFromRoot(ProtectedItem root_item) {
  auto decoded_result = DecodeAndValidateRoot(*crypto_, root_item);
  root_item.payload.clear();
  if (!decoded_result.ok()) {
    return Result<void>::Failure(decoded_result.error());
  }
  RootRecord root = std::move(decoded_result).value();

  auto enumerate_result = store_->Enumerate();
  if (!enumerate_result.ok()) {
    return Result<void>::Failure(enumerate_result.error());
  }

  try {
    std::vector<StoredPeer> stored_peers;
    bool saw_root = false;
    for (const ProtectedItemMetadata& metadata : enumerate_result.value()) {
      if (metadata.item_id == kRootItemId) {
        if (saw_root || metadata.revision != root.revision) {
          return Result<void>::Failure(ErrorCode::kRevisionConflict);
        }
        saw_root = true;
        continue;
      }
      auto parsed_id_result = ParsePeerItemId(metadata.item_id);
      if (!parsed_id_result.ok()) {
        return Result<void>::Failure(parsed_id_result.error());
      }
      const ParsedPeerItemId& parsed_id = parsed_id_result.value();
      if (parsed_id.store_id != root.store_id) {
        continue;
      }
      if (stored_peers.size() == kMaxPeerRecords) {
        return Result<void>::Failure(ErrorCode::kCapacityExceeded);
      }

      auto item_result = store_->Get(metadata.item_id);
      if (!item_result.ok()) {
        return Result<void>::Failure(item_result.error());
      }
      std::optional<ProtectedItem> item = std::move(item_result).value();
      if (!item.has_value() || item->revision != metadata.revision) {
        return Result<void>::Failure(ErrorCode::kRevisionConflict);
      }
      auto peer_result = DecodePeerRecord(item->payload.bytes());
      if (!peer_result.ok()) {
        return Result<void>::Failure(peer_result.error());
      }
      StoredPeerRecord peer = std::move(peer_result).value();
      if (peer.peer.record_revision != item->revision) {
        return Result<void>::Failure(ErrorCode::kRollbackDetected);
      }
      if (peer.store_id != parsed_id.store_id ||
          peer.record_id != parsed_id.record_id) {
        return Result<void>::Failure(ErrorCode::kCorruptRecord);
      }
      const auto validation_result =
          ValidatePeerAgainstRoot(*crypto_, *peer_key_validator_, root, peer);
      if (!validation_result.ok()) {
        return validation_result;
      }
      stored_peers.push_back(StoredPeer{metadata.item_id, std::move(peer)});
    }
    if (!saw_root) {
      return Result<void>::Failure(ErrorCode::kRevisionConflict);
    }

    std::sort(stored_peers.begin(), stored_peers.end(),
              [](const StoredPeer& left, const StoredPeer& right) {
                return left.record.peer.device_id < right.record.peer.device_id;
              });
    for (std::size_t index = 1; index < stored_peers.size(); ++index) {
      if (!(stored_peers[index - 1].record.peer.device_id <
            stored_peers[index].record.peer.device_id)) {
        return Result<void>::Failure(ErrorCode::kCorruptRecord);
      }
    }
    const auto unique_result = ValidateUniquePeerKeys(stored_peers);
    if (!unique_result.ok()) {
      return unique_result;
    }

    std::vector<PeerRecord> peer_view;
    peer_view.reserve(stored_peers.size());
    for (const StoredPeer& peer : stored_peers) {
      peer_view.push_back(peer.record.peer);
    }

    auto final_root_result = store_->Get(ProtectedItemId(kRootItemId));
    if (!final_root_result.ok()) {
      return Result<void>::Failure(final_root_result.error());
    }
    std::optional<ProtectedItem> final_root = std::move(final_root_result).value();
    if (!final_root.has_value()) {
      return Result<void>::Failure(ErrorCode::kRevisionConflict);
    }
    auto final_decoded_result = DecodeAndValidateRoot(*crypto_, *final_root);
    final_root->payload.clear();
    if (!final_decoded_result.ok()) {
      return Result<void>::Failure(final_decoded_result.error());
    }
    RootRecord final_decoded = std::move(final_decoded_result).value();
    if (!RootMatches(final_decoded, RootMetadataFrom(root))) {
      return Result<void>::Failure(ErrorCode::kRevisionConflict);
    }
    final_decoded.seed.clear();

    state_->root = RootMetadataFrom(root);
    root.seed.clear();
    state_->stored_peers = std::move(stored_peers);
    state_->peer_view = std::move(peer_view);
    ready_ = true;
    return Result<void>::Success();
  } catch (const std::bad_alloc&) {
    return Result<void>::Failure(ErrorCode::kCapacityExceeded);
  }
}

Result<SecretBuffer> IdentityRepository::LoadIdentitySeed() const {
  if (!ready_) {
    return Result<SecretBuffer>::Failure(ErrorCode::kInvalidState);
  }
  auto root_result = store_->Get(ProtectedItemId(kRootItemId));
  if (!root_result.ok()) {
    return Result<SecretBuffer>::Failure(root_result.error());
  }
  std::optional<ProtectedItem> root_item = std::move(root_result).value();
  if (!root_item.has_value()) {
    return Result<SecretBuffer>::Failure(ErrorCode::kIdentityLoss);
  }
  auto decoded_result = DecodeAndValidateRoot(*crypto_, *root_item);
  root_item->payload.clear();
  if (!decoded_result.ok()) {
    return Result<SecretBuffer>::Failure(decoded_result.error());
  }
  RootRecord root = std::move(decoded_result).value();
  if (root.revision != state_->root.revision) {
    return Result<SecretBuffer>::Failure(ErrorCode::kRevisionConflict);
  }
  if (!RootMatches(root, state_->root)) {
    return Result<SecretBuffer>::Failure(ErrorCode::kCorruptRecord);
  }
  return Result<SecretBuffer>::Success(std::move(root.seed));
}

Result<void> IdentityRepository::EnsureRootCurrent() const {
  auto seed_result = LoadIdentitySeed();
  if (!seed_result.ok()) {
    return Result<void>::Failure(seed_result.error());
  }
  SecretBuffer seed = std::move(seed_result).value();
  seed.clear();
  return Result<void>::Success();
}

Result<void> IdentityRepository::ValidatePeer(const StoredPeer& peer) const {
  auto seed_result = LoadIdentitySeed();
  if (!seed_result.ok()) {
    return Result<void>::Failure(seed_result.error());
  }
  RootRecord root{};
  root.revision = state_->root.revision;
  root.seed = std::move(seed_result).value();
  root.public_key = state_->root.public_key;
  root.device_id = state_->root.device_id;
  root.store_id = state_->root.store_id;
  auto validation_result =
      ValidatePeerAgainstRoot(*crypto_, *peer_key_validator_, root, peer.record);
  root.seed.clear();
  return validation_result;
}

Result<Mac> IdentityRepository::ComputePeerMac(const StoredPeer& peer) const {
  auto seed_result = LoadIdentitySeed();
  if (!seed_result.ok()) {
    return Result<Mac>::Failure(seed_result.error());
  }
  RootRecord root{};
  root.revision = state_->root.revision;
  root.seed = std::move(seed_result).value();
  root.public_key = state_->root.public_key;
  root.device_id = state_->root.device_id;
  root.store_id = state_->root.store_id;
  auto mac_result = ::xnn_transfer::core::security::identity::ComputePeerMac(
      *crypto_, root, peer.record);
  root.seed.clear();
  return mac_result;
}

Result<void> IdentityRepository::PutPeer(
    StoredPeer peer, const std::optional<std::uint64_t> expected_revision) {
  const auto validation_result = ValidatePeer(peer);
  if (!validation_result.ok()) {
    return validation_result;
  }
  auto encoded_result = EncodePeerRecord(peer.record);
  if (!encoded_result.ok()) {
    return Result<void>::Failure(encoded_result.error());
  }

  try {
    std::vector<StoredPeer> candidate = state_->stored_peers;
    const auto existing = std::find_if(
        candidate.begin(), candidate.end(),
        [&peer](const StoredPeer& current) { return current.item_id == peer.item_id; });
    if (existing == candidate.end()) {
      if (expected_revision.has_value()) {
        return Result<void>::Failure(ErrorCode::kInvalidState);
      }
      candidate.push_back(peer);
    } else {
      if (!expected_revision.has_value()) {
        return Result<void>::Failure(ErrorCode::kInvalidState);
      }
      *existing = peer;
    }
    std::sort(candidate.begin(), candidate.end(),
              [](const StoredPeer& left, const StoredPeer& right) {
                return left.record.peer.device_id < right.record.peer.device_id;
              });
    const auto unique_result = ValidateUniquePeerKeys(candidate);
    if (!unique_result.ok()) {
      return Result<void>::Failure(ErrorCode::kInvalidState);
    }
    std::vector<PeerRecord> peer_view;
    peer_view.reserve(candidate.size());
    for (const StoredPeer& current : candidate) {
      peer_view.push_back(current.record.peer);
    }

    const auto current_result = EnsureRootCurrent();
    if (!current_result.ok()) {
      return current_result;
    }
    const auto write_result =
        store_->CompareExchangePut(peer.item_id, expected_revision,
                                   ProtectedItem(peer.record.peer.record_revision,
                                                 std::move(encoded_result).value()));
    if (!write_result.ok()) {
      return write_result;
    }
    state_->stored_peers = std::move(candidate);
    state_->peer_view = std::move(peer_view);
    return Result<void>::Success();
  } catch (const std::bad_alloc&) {
    return Result<void>::Failure(ErrorCode::kCapacityExceeded);
  }
}

Result<DeviceId> IdentityRepository::CommitPeer(PeerCommit peer) {
  if (!ready_) {
    return Result<DeviceId>::Failure(ErrorCode::kInvalidState);
  }
  if (peer.security_profile == 0 || peer.display_label.size() > kMaxDisplayLabelBytes) {
    return Result<DeviceId>::Failure(ErrorCode::kInvalidArgument);
  }
  if (state_->stored_peers.size() == kMaxPeerRecords) {
    return Result<DeviceId>::Failure(ErrorCode::kCapacityExceeded);
  }
  const auto key_validation = peer_key_validator_->Validate(peer.public_key);
  if (!key_validation.ok()) {
    return Result<DeviceId>::Failure(key_validation.error());
  }
  if (KeyIsKnown(peer.public_key)) {
    return Result<DeviceId>::Failure(ErrorCode::kInvalidState);
  }
  auto device_id_result = crypto_->DeriveDeviceId(peer.public_key);
  if (!device_id_result.ok()) {
    return Result<DeviceId>::Failure(device_id_result.error());
  }
  if (FindPeer(device_id_result.value()) != nullptr) {
    return Result<DeviceId>::Failure(ErrorCode::kInvalidState);
  }

  StoredPeer stored{};
  stored.record.store_id = state_->root.store_id;
  stored.record.record_id = device_id_result.value();
  stored.record.root_device_id = state_->root.device_id;
  stored.record.peer.public_key = peer.public_key;
  stored.record.peer.device_id = device_id_result.value();
  stored.record.peer.security_profile = peer.security_profile;
  stored.record.peer.trust_state = TrustState::kActive;
  stored.record.peer.record_revision = 1;
  stored.record.peer.display_label = std::move(peer.display_label);
  auto item_id_result =
      BuildPeerItemId(stored.record.store_id, stored.record.record_id);
  if (!item_id_result.ok()) {
    return Result<DeviceId>::Failure(item_id_result.error());
  }
  stored.item_id = std::move(item_id_result).value();
  const auto mac_result = ComputePeerMac(stored);
  if (!mac_result.ok()) {
    return Result<DeviceId>::Failure(mac_result.error());
  }
  stored.record.authenticator = mac_result.value();
  const auto persist_result = PutPeer(std::move(stored), std::nullopt);
  if (!persist_result.ok()) {
    return Result<DeviceId>::Failure(persist_result.error());
  }
  return Result<DeviceId>::Success(device_id_result.value());
}

Result<DeviceId> IdentityRepository::RotatePeer(const DeviceId& current_device_id,
                                                const PublicKey& new_public_key,
                                                const std::uint64_t rotation_counter) {
  if (!ready_) {
    return Result<DeviceId>::Failure(ErrorCode::kInvalidState);
  }
  const StoredPeer* const current = FindStoredPeer(current_device_id);
  if (current == nullptr) {
    return Result<DeviceId>::Failure(ErrorCode::kNotFound);
  }
  const PeerRecord& current_peer = current->record.peer;
  if (current_peer.trust_state != TrustState::kActive ||
      current_peer.rotation_counter == std::numeric_limits<std::uint64_t>::max() ||
      rotation_counter != current_peer.rotation_counter + 1 ||
      current_peer.record_revision == std::numeric_limits<std::uint64_t>::max() ||
      current_peer.tombstones.size() == kMaxPeerTombstones) {
    return Result<DeviceId>::Failure(ErrorCode::kInvalidState);
  }
  const auto key_validation = peer_key_validator_->Validate(new_public_key);
  if (!key_validation.ok()) {
    return Result<DeviceId>::Failure(key_validation.error());
  }
  if (KeyIsKnown(new_public_key)) {
    return Result<DeviceId>::Failure(ErrorCode::kInvalidState);
  }
  auto new_device_id_result = crypto_->DeriveDeviceId(new_public_key);
  if (!new_device_id_result.ok()) {
    return Result<DeviceId>::Failure(new_device_id_result.error());
  }
  if (FindPeer(new_device_id_result.value()) != nullptr) {
    return Result<DeviceId>::Failure(ErrorCode::kInvalidState);
  }

  try {
    StoredPeer candidate = *current;
    candidate.record.peer.tombstones.push_back(candidate.record.peer.public_key);
    candidate.record.peer.public_key = new_public_key;
    candidate.record.peer.device_id = new_device_id_result.value();
    candidate.record.peer.rotation_counter = rotation_counter;
    ++candidate.record.peer.record_revision;
    const auto mac_result = ComputePeerMac(candidate);
    if (!mac_result.ok()) {
      return Result<DeviceId>::Failure(mac_result.error());
    }
    candidate.record.authenticator = mac_result.value();
    const std::uint64_t expected_revision = current_peer.record_revision;
    const auto persist_result = PutPeer(std::move(candidate), expected_revision);
    if (!persist_result.ok()) {
      return Result<DeviceId>::Failure(persist_result.error());
    }
    return Result<DeviceId>::Success(new_device_id_result.value());
  } catch (const std::bad_alloc&) {
    return Result<DeviceId>::Failure(ErrorCode::kCapacityExceeded);
  }
}

Result<void> IdentityRepository::RevokePeer(const DeviceId& device_id) {
  if (!ready_) {
    return Result<void>::Failure(ErrorCode::kInvalidState);
  }
  const StoredPeer* const current = FindStoredPeer(device_id);
  if (current == nullptr) {
    return Result<void>::Failure(ErrorCode::kNotFound);
  }
  const PeerRecord& current_peer = current->record.peer;
  if (current_peer.trust_state != TrustState::kActive ||
      current_peer.record_revision == std::numeric_limits<std::uint64_t>::max()) {
    return Result<void>::Failure(ErrorCode::kInvalidState);
  }

  StoredPeer candidate = *current;
  candidate.record.peer.trust_state = TrustState::kRevoked;
  ++candidate.record.peer.record_revision;
  const auto mac_result = ComputePeerMac(candidate);
  if (!mac_result.ok()) {
    return Result<void>::Failure(mac_result.error());
  }
  candidate.record.authenticator = mac_result.value();
  return PutPeer(std::move(candidate), current_peer.record_revision);
}

Result<void> IdentityRepository::ForgetPeer(const DeviceId& device_id) {
  if (!ready_) {
    return Result<void>::Failure(ErrorCode::kInvalidState);
  }
  const StoredPeer* const current = FindStoredPeer(device_id);
  if (current == nullptr) {
    return Result<void>::Failure(ErrorCode::kNotFound);
  }
  if (current->record.peer.trust_state != TrustState::kRevoked) {
    return Result<void>::Failure(ErrorCode::kInvalidState);
  }

  try {
    const ProtectedItemId item_id = current->item_id;
    const std::uint64_t expected_revision = current->record.peer.record_revision;
    std::vector<StoredPeer> candidate = state_->stored_peers;
    candidate.erase(std::remove_if(candidate.begin(), candidate.end(),
                                   [&item_id](const StoredPeer& peer) {
                                     return peer.item_id == item_id;
                                   }),
                    candidate.end());
    std::vector<PeerRecord> peer_view;
    peer_view.reserve(candidate.size());
    for (const StoredPeer& peer : candidate) {
      peer_view.push_back(peer.record.peer);
    }

    const auto current_result = EnsureRootCurrent();
    if (!current_result.ok()) {
      return current_result;
    }
    const auto delete_result =
        store_->CompareExchangeDelete(item_id, expected_revision);
    if (!delete_result.ok()) {
      return delete_result;
    }
    state_->stored_peers = std::move(candidate);
    state_->peer_view = std::move(peer_view);
    return Result<void>::Success();
  } catch (const std::bad_alloc&) {
    return Result<void>::Failure(ErrorCode::kCapacityExceeded);
  }
}

Result<ResetOutcome> IdentityRepository::Reset() {
  if (!ready_ || state_->root.revision == std::numeric_limits<std::uint64_t>::max()) {
    return Result<ResetOutcome>::Failure(ErrorCode::kInvalidState);
  }
  const auto current_result = EnsureRootCurrent();
  if (!current_result.ok()) {
    return Result<ResetOutcome>::Failure(current_result.error());
  }
  if (state_->root.retired_store_id.has_value()) {
    const auto pending_result =
        GenerationHasItems(*store_, *state_->root.retired_store_id);
    if (!pending_result.ok()) {
      return Result<ResetOutcome>::Failure(pending_result.error());
    }
    if (pending_result.value()) {
      return Result<ResetOutcome>::Failure(ErrorCode::kInvalidState);
    }
  }
  const std::uint64_t next_revision = state_->root.revision + 1;
  auto root_result = MakeRoot(*crypto_, next_revision);
  if (!root_result.ok()) {
    return Result<ResetOutcome>::Failure(root_result.error());
  }
  RootRecord replacement = std::move(root_result).value();
  if (ConstantTimeEqual(replacement.store_id, state_->root.store_id) ||
      ConstantTimeEqual(replacement.public_key, state_->root.public_key)) {
    return Result<ResetOutcome>::Failure(ErrorCode::kEntropyFailure);
  }
  replacement.retired_store_id = state_->root.store_id;
  auto encoded_result = EncodeRootRecord(replacement);
  if (!encoded_result.ok()) {
    return Result<ResetOutcome>::Failure(encoded_result.error());
  }
  const auto write_result = store_->CompareExchangePut(
      ProtectedItemId(kRootItemId), state_->root.revision,
      ProtectedItem(next_revision, std::move(encoded_result).value()));
  if (!write_result.ok()) {
    return Result<ResetOutcome>::Failure(write_result.error());
  }

  state_->root = RootMetadataFrom(replacement);
  replacement.seed.clear();
  state_->stored_peers.clear();
  state_->peer_view.clear();
  ready_ = true;

  const auto cleanup_result = CleanupStaleItems();
  return Result<ResetOutcome>::Success(ResetOutcome{cleanup_result.ok()});
}

Result<void> IdentityRepository::CleanupStaleItems() {
  if (!ready_) {
    return Result<void>::Failure(ErrorCode::kInvalidState);
  }
  const auto current_result = EnsureRootCurrent();
  if (!current_result.ok()) {
    return current_result;
  }
  if (!state_->root.retired_store_id.has_value()) {
    return Result<void>::Success();
  }
  const StoreId retired_store_id = *state_->root.retired_store_id;
  auto enumerate_result = store_->Enumerate();
  if (!enumerate_result.ok()) {
    return Result<void>::Failure(enumerate_result.error());
  }
  for (const ProtectedItemMetadata& metadata : enumerate_result.value()) {
    if (metadata.item_id == kRootItemId) {
      continue;
    }
    const auto parsed_result = ParsePeerItemId(metadata.item_id);
    if (!parsed_result.ok()) {
      return Result<void>::Failure(parsed_result.error());
    }
    if (parsed_result.value().store_id != retired_store_id) {
      continue;
    }
    const auto delete_result =
        store_->CompareExchangeDelete(metadata.item_id, metadata.revision);
    if (!delete_result.ok()) {
      return delete_result;
    }
  }
  return Result<void>::Success();
}

Result<void> IdentityRepository::UseIdentitySeed(
    const IdentitySeedConsumer& consumer) const {
  if (!ready_) {
    return Result<void>::Failure(ErrorCode::kInvalidState);
  }
  if (!consumer) {
    return Result<void>::Failure(ErrorCode::kInvalidArgument);
  }
  auto seed_result = LoadIdentitySeed();
  if (!seed_result.ok()) {
    return Result<void>::Failure(seed_result.error());
  }
  SecretBuffer seed = std::move(seed_result).value();
  auto consumer_result = consumer(seed.bytes());
  seed.clear();
  return consumer_result;
}

bool IdentityRepository::ready() const noexcept { return ready_; }

std::uint64_t IdentityRepository::revision() const noexcept {
  return ready_ ? state_->root.revision : 0;
}

const PublicKey* IdentityRepository::root_public_key() const noexcept {
  return ready_ ? &state_->root.public_key : nullptr;
}

const DeviceId* IdentityRepository::root_device_id() const noexcept {
  return ready_ ? &state_->root.device_id : nullptr;
}

std::span<const PeerRecord> IdentityRepository::peers() const noexcept {
  return ready_ ? std::span<const PeerRecord>(state_->peer_view)
                : std::span<const PeerRecord>{};
}

const PeerRecord* IdentityRepository::FindPeer(
    const DeviceId& device_id) const noexcept {
  if (!ready_) {
    return nullptr;
  }
  const auto iterator =
      std::lower_bound(state_->peer_view.begin(), state_->peer_view.end(), device_id,
                       [](const PeerRecord& peer, const DeviceId& wanted) {
                         return peer.device_id < wanted;
                       });
  if (iterator == state_->peer_view.end() || iterator->device_id != device_id) {
    return nullptr;
  }
  return &*iterator;
}

IdentityRepository::StoredPeer* IdentityRepository::FindStoredPeer(
    const DeviceId& device_id) noexcept {
  const auto iterator =
      std::lower_bound(state_->stored_peers.begin(), state_->stored_peers.end(),
                       device_id, [](const StoredPeer& peer, const DeviceId& wanted) {
                         return peer.record.peer.device_id < wanted;
                       });
  if (iterator == state_->stored_peers.end() ||
      iterator->record.peer.device_id != device_id) {
    return nullptr;
  }
  return &*iterator;
}

const IdentityRepository::StoredPeer* IdentityRepository::FindStoredPeer(
    const DeviceId& device_id) const noexcept {
  const auto iterator =
      std::lower_bound(state_->stored_peers.begin(), state_->stored_peers.end(),
                       device_id, [](const StoredPeer& peer, const DeviceId& wanted) {
                         return peer.record.peer.device_id < wanted;
                       });
  if (iterator == state_->stored_peers.end() ||
      iterator->record.peer.device_id != device_id) {
    return nullptr;
  }
  return &*iterator;
}

bool IdentityRepository::KeyIsKnown(
    const PublicKey& public_key,
    const DeviceId* const except_device_id) const noexcept {
  for (const StoredPeer& stored : state_->stored_peers) {
    const PeerRecord& peer = stored.record.peer;
    if (except_device_id != nullptr && peer.device_id == *except_device_id) {
      continue;
    }
    if (peer.public_key == public_key ||
        std::find(peer.tombstones.begin(), peer.tombstones.end(), public_key) !=
            peer.tombstones.end()) {
      return true;
    }
  }
  return false;
}

}  // namespace xnn_transfer::core::security::identity
