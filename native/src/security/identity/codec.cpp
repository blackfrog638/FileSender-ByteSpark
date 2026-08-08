#include "codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <new>
#include <string_view>

namespace xnn_transfer::core::security::identity::internal {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'X', 'N', 'N', 'I'};
constexpr std::uint8_t kCanonicalVersion = 1;
constexpr std::uint8_t kRootKind = 1;
constexpr std::uint8_t kPeerKind = 2;
constexpr std::uint8_t kPeerMacInputKind = 3;
constexpr std::size_t kEnvelopeSize = 12;
constexpr std::size_t kFieldHeaderSize = 6;
constexpr std::size_t kMaxFields = 12;

struct FieldView {
  std::uint16_t id{};
  std::span<const std::uint8_t> value{};
};

struct ParsedRecord {
  std::array<FieldView, kMaxFields> fields{};
  std::size_t field_count{};
};

struct FieldSpec {
  std::uint16_t id{};
  std::span<const std::uint8_t> value{};
};

[[nodiscard]] std::uint16_t ReadU16(const std::span<const std::uint8_t> bytes) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) |
                                    static_cast<std::uint16_t>(bytes[1]));
}

[[nodiscard]] std::uint32_t ReadU32(const std::span<const std::uint8_t> bytes) {
  std::uint32_t value = 0;
  for (const std::uint8_t byte : bytes.first(4)) {
    value =
        static_cast<std::uint32_t>((value << 8U) | static_cast<std::uint32_t>(byte));
  }
  return value;
}

[[nodiscard]] std::uint64_t ReadU64(const std::span<const std::uint8_t> bytes) {
  std::uint64_t value = 0;
  for (const std::uint8_t byte : bytes.first(8)) {
    value = (value << 8U) | static_cast<std::uint64_t>(byte);
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

void WriteU64(const std::span<std::uint8_t> output, const std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    const std::size_t shift = (7 - index) * 8;
    output[index] = static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift));
  }
}

[[nodiscard]] bool IsUnicodeNoncharacter(const std::uint32_t code_point) {
  return (code_point >= 0xfdd0U && code_point <= 0xfdefU) ||
         (code_point <= 0x10ffffU && (code_point & 0xffffU) >= 0xfffeU);
}

[[nodiscard]] bool IsContinuation(const std::uint8_t value) {
  return (value & 0xc0U) == 0x80U;
}

[[nodiscard]] bool IsValidUtf8(const std::span<const std::uint8_t> value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const std::uint8_t first = value[offset];
    std::uint32_t code_point = 0;
    std::size_t width = 0;
    if (first <= 0x7fU) {
      code_point = first;
      width = 1;
    } else if (first >= 0xc2U && first <= 0xdfU) {
      width = 2;
      if (value.size() - offset < width || !IsContinuation(value[offset + 1])) {
        return false;
      }
      code_point = (static_cast<std::uint32_t>(first & 0x1fU) << 6U) |
                   static_cast<std::uint32_t>(value[offset + 1] & 0x3fU);
    } else if (first >= 0xe0U && first <= 0xefU) {
      width = 3;
      if (value.size() - offset < width || !IsContinuation(value[offset + 1]) ||
          !IsContinuation(value[offset + 2])) {
        return false;
      }
      const std::uint8_t second = value[offset + 1];
      if ((first == 0xe0U && second < 0xa0U) || (first == 0xedU && second >= 0xa0U)) {
        return false;
      }
      code_point = (static_cast<std::uint32_t>(first & 0x0fU) << 12U) |
                   (static_cast<std::uint32_t>(second & 0x3fU) << 6U) |
                   static_cast<std::uint32_t>(value[offset + 2] & 0x3fU);
    } else if (first >= 0xf0U && first <= 0xf4U) {
      width = 4;
      if (value.size() - offset < width || !IsContinuation(value[offset + 1]) ||
          !IsContinuation(value[offset + 2]) || !IsContinuation(value[offset + 3])) {
        return false;
      }
      const std::uint8_t second = value[offset + 1];
      if ((first == 0xf0U && second < 0x90U) || (first == 0xf4U && second > 0x8fU)) {
        return false;
      }
      code_point = (static_cast<std::uint32_t>(first & 0x07U) << 18U) |
                   (static_cast<std::uint32_t>(second & 0x3fU) << 12U) |
                   (static_cast<std::uint32_t>(value[offset + 2] & 0x3fU) << 6U) |
                   static_cast<std::uint32_t>(value[offset + 3] & 0x3fU);
    } else {
      return false;
    }

    if (code_point == 0U || IsUnicodeNoncharacter(code_point)) {
      return false;
    }
    offset += width;
  }
  return true;
}

[[nodiscard]] Result<ParsedRecord> ParseRecord(
    const std::span<const std::uint8_t> encoded, const std::uint8_t expected_kind) {
  if (encoded.size() > kMaxProtectedItemPayloadSize) {
    return Result<ParsedRecord>::Failure(ErrorCode::kCapacityExceeded);
  }
  if (encoded.size() < kEnvelopeSize ||
      !std::equal(kMagic.begin(), kMagic.end(), encoded.begin())) {
    return Result<ParsedRecord>::Failure(ErrorCode::kCorruptRecord);
  }
  if (encoded[4] != kCanonicalVersion) {
    return Result<ParsedRecord>::Failure(ErrorCode::kUnsupportedSchema);
  }
  if (encoded[5] != expected_kind) {
    return Result<ParsedRecord>::Failure(ErrorCode::kCorruptRecord);
  }

  const std::uint16_t field_count = ReadU16(encoded.subspan(6, 2));
  const std::uint32_t body_length = ReadU32(encoded.subspan(8, 4));
  if (field_count > kMaxFields ||
      static_cast<std::size_t>(body_length) != encoded.size() - kEnvelopeSize) {
    return Result<ParsedRecord>::Failure(ErrorCode::kCorruptRecord);
  }

  ParsedRecord parsed{};
  std::size_t offset = kEnvelopeSize;
  std::uint16_t previous_id = 0;
  for (std::size_t index = 0; index < field_count; ++index) {
    if (encoded.size() - offset < kFieldHeaderSize) {
      return Result<ParsedRecord>::Failure(ErrorCode::kCorruptRecord);
    }
    const std::uint16_t id = ReadU16(encoded.subspan(offset, 2));
    const std::uint32_t value_length = ReadU32(encoded.subspan(offset + 2, 4));
    offset += kFieldHeaderSize;
    if (id == 0 || (index != 0 && id <= previous_id) ||
        value_length > encoded.size() - offset) {
      return Result<ParsedRecord>::Failure(ErrorCode::kCorruptRecord);
    }
    parsed.fields[index] =
        FieldView{id, encoded.subspan(offset, static_cast<std::size_t>(value_length))};
    parsed.field_count = index + 1;
    previous_id = id;
    offset += static_cast<std::size_t>(value_length);
  }
  if (offset != encoded.size()) {
    return Result<ParsedRecord>::Failure(ErrorCode::kCorruptRecord);
  }
  return Result<ParsedRecord>::Success(parsed);
}

template <std::size_t Size>
[[nodiscard]] bool HasExactFields(const ParsedRecord& parsed,
                                  const std::array<std::uint16_t, Size>& expected) {
  if (parsed.field_count != expected.size()) {
    return false;
  }
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (parsed.fields[index].id != expected[index]) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] Result<std::size_t> EncodedRecordSize(
    const std::span<const FieldSpec> fields) {
  std::size_t body_size = 0;
  for (const FieldSpec& field : fields) {
    if (field.value.size() >
        std::numeric_limits<std::uint32_t>::max() - kFieldHeaderSize) {
      return Result<std::size_t>::Failure(ErrorCode::kCapacityExceeded);
    }
    const std::size_t field_size = kFieldHeaderSize + field.value.size();
    if (field_size > kMaxProtectedItemPayloadSize ||
        body_size > kMaxProtectedItemPayloadSize - field_size) {
      return Result<std::size_t>::Failure(ErrorCode::kCapacityExceeded);
    }
    body_size += field_size;
  }
  if (body_size > kMaxProtectedItemPayloadSize - kEnvelopeSize) {
    return Result<std::size_t>::Failure(ErrorCode::kCapacityExceeded);
  }
  return Result<std::size_t>::Success(kEnvelopeSize + body_size);
}

[[nodiscard]] Result<void> WriteRecord(const std::uint8_t kind,
                                       const std::span<const FieldSpec> fields,
                                       const std::span<std::uint8_t> output) {
  const auto size_result = EncodedRecordSize(fields);
  if (!size_result.ok() || size_result.value() != output.size() ||
      fields.size() > std::numeric_limits<std::uint16_t>::max()) {
    return Result<void>::Failure(size_result.ok() ? ErrorCode::kInvalidArgument
                                                  : size_result.error());
  }

  std::copy(kMagic.begin(), kMagic.end(), output.begin());
  output[4] = kCanonicalVersion;
  output[5] = kind;
  WriteU16(output, 6, static_cast<std::uint16_t>(fields.size()));
  WriteU32(output, 8, static_cast<std::uint32_t>(output.size() - kEnvelopeSize));

  std::size_t offset = kEnvelopeSize;
  for (const FieldSpec& field : fields) {
    WriteU16(output, offset, field.id);
    WriteU32(output, offset + 2, static_cast<std::uint32_t>(field.value.size()));
    offset += kFieldHeaderSize;
    std::copy(field.value.begin(), field.value.end(),
              output.begin() + static_cast<std::ptrdiff_t>(offset));
    offset += field.value.size();
  }
  return Result<void>::Success();
}

[[nodiscard]] std::span<const std::uint8_t> StringBytes(const std::string& value) {
  return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

[[nodiscard]] Result<SecretBuffer> EncodePeer(const StoredPeerRecord& record,
                                              const bool include_authenticator) {
  const PeerRecord& peer = record.peer;
  if (peer.security_profile == 0 || peer.record_revision == 0 ||
      peer.display_label.size() > kMaxDisplayLabelBytes ||
      !IsValidUtf8(StringBytes(peer.display_label)) ||
      peer.tombstones.size() > kMaxPeerTombstones ||
      peer.rotation_counter != peer.tombstones.size() ||
      (peer.trust_state != TrustState::kActive &&
       peer.trust_state != TrustState::kRevoked)) {
    return Result<SecretBuffer>::Failure(ErrorCode::kInvalidArgument);
  }
  for (std::size_t index = 0; index < peer.tombstones.size(); ++index) {
    if (peer.tombstones[index] == peer.public_key ||
        std::find(peer.tombstones.begin(),
                  peer.tombstones.begin() + static_cast<std::ptrdiff_t>(index),
                  peer.tombstones[index]) !=
            peer.tombstones.begin() + static_cast<std::ptrdiff_t>(index)) {
      return Result<SecretBuffer>::Failure(ErrorCode::kInvalidArgument);
    }
  }

  std::array<std::uint8_t, 2> profile{};
  WriteU16(profile, 0, peer.security_profile);
  const std::array<std::uint8_t, 1> state{static_cast<std::uint8_t>(peer.trust_state)};
  std::array<std::uint8_t, 8> rotation{};
  WriteU64(rotation, peer.rotation_counter);
  std::array<std::uint8_t, 8> revision{};
  WriteU64(revision, peer.record_revision);

  try {
    SecretBuffer tombstones(1 + (peer.tombstones.size() * kEd25519PublicKeySize));
    tombstones.mutable_bytes()[0] = static_cast<std::uint8_t>(peer.tombstones.size());
    std::size_t tombstone_offset = 1;
    for (const PublicKey& tombstone : peer.tombstones) {
      std::copy(tombstone.begin(), tombstone.end(),
                tombstones.mutable_bytes().begin() +
                    static_cast<std::ptrdiff_t>(tombstone_offset));
      tombstone_offset += tombstone.size();
    }

    const std::array<FieldSpec, 12> all_fields{{
        {1, record.store_id},
        {2, record.record_id},
        {3, record.root_device_id},
        {4, peer.public_key},
        {5, peer.device_id},
        {6, profile},
        {7, state},
        {8, rotation},
        {9, revision},
        {10, StringBytes(peer.display_label)},
        {11, tombstones.bytes()},
        {12, record.authenticator},
    }};
    const std::size_t field_count = include_authenticator ? 12 : 11;
    const std::span<const FieldSpec> fields(all_fields.data(), field_count);
    const auto size_result = EncodedRecordSize(fields);
    if (!size_result.ok()) {
      return Result<SecretBuffer>::Failure(size_result.error());
    }
    SecretBuffer encoded(size_result.value());
    const auto write_result =
        WriteRecord(include_authenticator ? kPeerKind : kPeerMacInputKind, fields,
                    encoded.mutable_bytes());
    if (!write_result.ok()) {
      return Result<SecretBuffer>::Failure(write_result.error());
    }
    return Result<SecretBuffer>::Success(std::move(encoded));
  } catch (const std::bad_alloc&) {
    return Result<SecretBuffer>::Failure(ErrorCode::kCapacityExceeded);
  }
}

}  // namespace

Result<SecretBuffer> EncodeRootRecord(const RootRecord& record) {
  const bool invalid_retired_store =
      record.retired_store_id.has_value() &&
      (*record.retired_store_id == record.store_id ||
       std::all_of(record.retired_store_id->begin(), record.retired_store_id->end(),
                   [](const std::uint8_t byte) { return byte == 0; }));
  if (record.revision == 0 || record.seed.size() != kEd25519SeedSize ||
      invalid_retired_store) {
    return Result<SecretBuffer>::Failure(record.seed.size() != kEd25519SeedSize
                                             ? ErrorCode::kIdentityLoss
                                             : ErrorCode::kInvalidArgument);
  }
  std::array<std::uint8_t, 8> revision{};
  WriteU64(revision, record.revision);
  std::array<FieldSpec, 6> fields{{
      {1, record.seed.bytes()},
      {2, record.public_key},
      {3, record.device_id},
      {4, record.store_id},
      {5, revision},
      {},
  }};
  std::size_t field_count = 5;
  if (record.retired_store_id.has_value()) {
    fields[5] = FieldSpec{6, *record.retired_store_id};
    field_count = 6;
  }
  const std::span<const FieldSpec> encoded_fields(fields.data(), field_count);
  const auto size_result = EncodedRecordSize(encoded_fields);
  if (!size_result.ok()) {
    return Result<SecretBuffer>::Failure(size_result.error());
  }
  try {
    SecretBuffer encoded(size_result.value());
    const auto write_result =
        WriteRecord(kRootKind, encoded_fields, encoded.mutable_bytes());
    if (!write_result.ok()) {
      return Result<SecretBuffer>::Failure(write_result.error());
    }
    return Result<SecretBuffer>::Success(std::move(encoded));
  } catch (const std::bad_alloc&) {
    return Result<SecretBuffer>::Failure(ErrorCode::kCapacityExceeded);
  }
}

Result<RootRecord> DecodeRootRecord(const std::span<const std::uint8_t> encoded) {
  const auto parsed_result = ParseRecord(encoded, kRootKind);
  if (!parsed_result.ok()) {
    return Result<RootRecord>::Failure(parsed_result.error());
  }
  const ParsedRecord& parsed = parsed_result.value();
  constexpr std::array<std::uint16_t, 5> kBaseFields{1, 2, 3, 4, 5};
  constexpr std::array<std::uint16_t, 6> kRetiredFields{1, 2, 3, 4, 5, 6};
  const bool has_retired_store = HasExactFields(parsed, kRetiredFields);
  if (!HasExactFields(parsed, kBaseFields) && !has_retired_store) {
    const bool has_seed = parsed.field_count != 0 && parsed.fields[0].id == 1;
    return Result<RootRecord>::Failure(has_seed ? ErrorCode::kCorruptRecord
                                                : ErrorCode::kIdentityLoss);
  }
  if (parsed.fields[0].value.size() != kEd25519SeedSize) {
    return Result<RootRecord>::Failure(parsed.fields[0].value.empty()
                                           ? ErrorCode::kIdentityLoss
                                           : ErrorCode::kCorruptRecord);
  }
  if (parsed.fields[1].value.size() != kEd25519PublicKeySize ||
      parsed.fields[2].value.size() != kDeviceIdSize ||
      parsed.fields[3].value.size() != kStoreIdSize ||
      parsed.fields[4].value.size() != 8 ||
      (has_retired_store && parsed.fields[5].value.size() != kStoreIdSize)) {
    return Result<RootRecord>::Failure(ErrorCode::kCorruptRecord);
  }

  try {
    RootRecord root{};
    root.seed = SecretBuffer(parsed.fields[0].value);
    std::copy(parsed.fields[1].value.begin(), parsed.fields[1].value.end(),
              root.public_key.begin());
    std::copy(parsed.fields[2].value.begin(), parsed.fields[2].value.end(),
              root.device_id.begin());
    std::copy(parsed.fields[3].value.begin(), parsed.fields[3].value.end(),
              root.store_id.begin());
    if (has_retired_store) {
      StoreId retired_store_id{};
      std::copy(parsed.fields[5].value.begin(), parsed.fields[5].value.end(),
                retired_store_id.begin());
      if (retired_store_id == root.store_id ||
          std::all_of(retired_store_id.begin(), retired_store_id.end(),
                      [](const std::uint8_t byte) { return byte == 0; })) {
        return Result<RootRecord>::Failure(ErrorCode::kCorruptRecord);
      }
      root.retired_store_id = retired_store_id;
    }
    root.revision = ReadU64(parsed.fields[4].value);
    if (root.revision == 0) {
      return Result<RootRecord>::Failure(ErrorCode::kCorruptRecord);
    }
    return Result<RootRecord>::Success(std::move(root));
  } catch (const std::bad_alloc&) {
    return Result<RootRecord>::Failure(ErrorCode::kCapacityExceeded);
  }
}

Result<SecretBuffer> EncodePeerRecord(const StoredPeerRecord& record) {
  return EncodePeer(record, true);
}

Result<SecretBuffer> EncodePeerMacInput(const StoredPeerRecord& record) {
  return EncodePeer(record, false);
}

Result<StoredPeerRecord> DecodePeerRecord(const std::span<const std::uint8_t> encoded) {
  const auto parsed_result = ParseRecord(encoded, kPeerKind);
  if (!parsed_result.ok()) {
    return Result<StoredPeerRecord>::Failure(parsed_result.error());
  }
  const ParsedRecord& parsed = parsed_result.value();
  constexpr std::array<std::uint16_t, 12> kFields{1, 2, 3, 4,  5,  6,
                                                  7, 8, 9, 10, 11, 12};
  if (!HasExactFields(parsed, kFields) ||
      parsed.fields[0].value.size() != kStoreIdSize ||
      parsed.fields[1].value.size() != kDeviceIdSize ||
      parsed.fields[2].value.size() != kDeviceIdSize ||
      parsed.fields[3].value.size() != kEd25519PublicKeySize ||
      parsed.fields[4].value.size() != kDeviceIdSize ||
      parsed.fields[5].value.size() != 2 || parsed.fields[6].value.size() != 1 ||
      parsed.fields[7].value.size() != 8 || parsed.fields[8].value.size() != 8 ||
      parsed.fields[9].value.size() > kMaxDisplayLabelBytes ||
      !IsValidUtf8(parsed.fields[9].value) || parsed.fields[10].value.empty() ||
      parsed.fields[11].value.size() != kMacSize) {
    return Result<StoredPeerRecord>::Failure(ErrorCode::kCorruptRecord);
  }

  const std::uint16_t profile = ReadU16(parsed.fields[5].value);
  const std::uint8_t raw_state = parsed.fields[6].value[0];
  const std::uint64_t rotation = ReadU64(parsed.fields[7].value);
  const std::uint64_t revision = ReadU64(parsed.fields[8].value);
  const std::size_t tombstone_count = parsed.fields[10].value[0];
  if (profile == 0 ||
      (raw_state != static_cast<std::uint8_t>(TrustState::kActive) &&
       raw_state != static_cast<std::uint8_t>(TrustState::kRevoked)) ||
      revision == 0 || tombstone_count > kMaxPeerTombstones ||
      rotation != tombstone_count ||
      parsed.fields[10].value.size() != 1 + (tombstone_count * kEd25519PublicKeySize)) {
    return Result<StoredPeerRecord>::Failure(ErrorCode::kCorruptRecord);
  }

  try {
    StoredPeerRecord record{};
    std::copy(parsed.fields[0].value.begin(), parsed.fields[0].value.end(),
              record.store_id.begin());
    std::copy(parsed.fields[1].value.begin(), parsed.fields[1].value.end(),
              record.record_id.begin());
    std::copy(parsed.fields[2].value.begin(), parsed.fields[2].value.end(),
              record.root_device_id.begin());
    std::copy(parsed.fields[3].value.begin(), parsed.fields[3].value.end(),
              record.peer.public_key.begin());
    std::copy(parsed.fields[4].value.begin(), parsed.fields[4].value.end(),
              record.peer.device_id.begin());
    record.peer.security_profile = profile;
    record.peer.trust_state = static_cast<TrustState>(raw_state);
    record.peer.rotation_counter = rotation;
    record.peer.record_revision = revision;
    record.peer.display_label.assign(
        reinterpret_cast<const char*>(parsed.fields[9].value.data()),
        parsed.fields[9].value.size());
    record.peer.tombstones.reserve(tombstone_count);
    for (std::size_t index = 0; index < tombstone_count; ++index) {
      PublicKey tombstone{};
      const std::size_t offset = 1 + (index * tombstone.size());
      std::copy_n(parsed.fields[10].value.begin() + static_cast<std::ptrdiff_t>(offset),
                  tombstone.size(), tombstone.begin());
      if (tombstone == record.peer.public_key ||
          std::find(record.peer.tombstones.begin(), record.peer.tombstones.end(),
                    tombstone) != record.peer.tombstones.end()) {
        return Result<StoredPeerRecord>::Failure(ErrorCode::kCorruptRecord);
      }
      record.peer.tombstones.push_back(tombstone);
    }
    std::copy(parsed.fields[11].value.begin(), parsed.fields[11].value.end(),
              record.authenticator.begin());
    return Result<StoredPeerRecord>::Success(std::move(record));
  } catch (const std::bad_alloc&) {
    return Result<StoredPeerRecord>::Failure(ErrorCode::kCapacityExceeded);
  }
}

}  // namespace xnn_transfer::core::security::identity::internal
