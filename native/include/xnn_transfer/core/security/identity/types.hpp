#ifndef XNN_TRANSFER_CORE_SECURITY_IDENTITY_TYPES_HPP_
#define XNN_TRANSFER_CORE_SECURITY_IDENTITY_TYPES_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace xnn_transfer::core::security::identity {

inline constexpr std::size_t kEd25519SeedSize = 32;
inline constexpr std::size_t kEd25519PublicKeySize = 32;
inline constexpr std::size_t kDeviceIdSize = 32;
inline constexpr std::size_t kStoreIdSize = 16;
inline constexpr std::size_t kMacSize = 32;
inline constexpr std::size_t kMaxDisplayLabelBytes = 96;
inline constexpr std::size_t kMaxPeerRecords = 256;
inline constexpr std::size_t kMaxPeerTombstones = 8;
inline constexpr std::size_t kMaxProtectedItemPayloadSize = 2'048;
inline constexpr std::size_t kMaxProtectedItemIdBytes = 128;

using PublicKey = std::array<std::uint8_t, kEd25519PublicKeySize>;
using DeviceId = std::array<std::uint8_t, kDeviceIdSize>;
using StoreId = std::array<std::uint8_t, kStoreIdSize>;
using Mac = std::array<std::uint8_t, kMacSize>;

enum class TrustState : std::uint8_t {
  kActive = 1,
  kRevoked = 2,
};

struct PeerRecord {
  PublicKey public_key{};
  DeviceId device_id{};
  std::uint16_t security_profile{};
  TrustState trust_state{TrustState::kActive};
  std::uint64_t rotation_counter{};
  std::uint64_t record_revision{};
  std::string display_label{};
  std::vector<PublicKey> tombstones{};
};

enum class ErrorCode : std::uint8_t {
  kNone = 0,
  kInvalidArgument,
  kInvalidState,
  kNotFound,
  kStorageLocked,
  kStorageUnavailable,
  kPermissionDenied,
  kRevisionConflict,
  kCorruptRecord,
  kUnsupportedSchema,
  kRollbackDetected,
  kIdentityLoss,
  kCapacityExceeded,
  kEntropyFailure,
  kCryptoFailure,
};

[[nodiscard]] std::string_view ErrorCodeName(ErrorCode code) noexcept;

template <typename T>
class Result final {
 public:
  [[nodiscard]] static Result Success(T value) { return Result(std::move(value)); }

  [[nodiscard]] static Result Failure(const ErrorCode error) { return Result(error); }

  [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(storage_); }

  [[nodiscard]] ErrorCode error() const noexcept {
    if (ok()) {
      return ErrorCode::kNone;
    }
    return std::get<ErrorCode>(storage_);
  }

  [[nodiscard]] T& value() & { return std::get<T>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
  [[nodiscard]] T&& value() && { return std::move(std::get<T>(storage_)); }

 private:
  explicit Result(T value) : storage_(std::move(value)) {}
  explicit Result(const ErrorCode error) : storage_(error) {}

  std::variant<T, ErrorCode> storage_;
};

template <>
class Result<void> final {
 public:
  [[nodiscard]] static Result Success() { return Result(ErrorCode::kNone); }

  [[nodiscard]] static Result Failure(const ErrorCode error) { return Result(error); }

  [[nodiscard]] bool ok() const noexcept { return error_ == ErrorCode::kNone; }

  [[nodiscard]] ErrorCode error() const noexcept { return error_; }

 private:
  explicit Result(const ErrorCode error) : error_(error) {}

  ErrorCode error_;
};

}  // namespace xnn_transfer::core::security::identity

#endif  // XNN_TRANSFER_CORE_SECURITY_IDENTITY_TYPES_HPP_
