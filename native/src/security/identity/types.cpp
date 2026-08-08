#include "xnn_transfer/core/security/identity/types.hpp"

namespace xnn_transfer::core::security::identity {

std::string_view ErrorCodeName(const ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kNone:
      return "NONE";
    case ErrorCode::kInvalidArgument:
      return "INVALID_ARGUMENT";
    case ErrorCode::kInvalidState:
      return "INVALID_STATE";
    case ErrorCode::kNotFound:
      return "NOT_FOUND";
    case ErrorCode::kStorageLocked:
      return "STORAGE_LOCKED";
    case ErrorCode::kStorageUnavailable:
      return "STORAGE_UNAVAILABLE";
    case ErrorCode::kPermissionDenied:
      return "PERMISSION_DENIED";
    case ErrorCode::kRevisionConflict:
      return "REVISION_CONFLICT";
    case ErrorCode::kCorruptRecord:
      return "CORRUPT_RECORD";
    case ErrorCode::kUnsupportedSchema:
      return "UNSUPPORTED_SCHEMA";
    case ErrorCode::kRollbackDetected:
      return "ROLLBACK_DETECTED";
    case ErrorCode::kIdentityLoss:
      return "IDENTITY_LOSS";
    case ErrorCode::kCapacityExceeded:
      return "CAPACITY_EXCEEDED";
    case ErrorCode::kEntropyFailure:
      return "ENTROPY_FAILURE";
    case ErrorCode::kCryptoFailure:
      return "CRYPTO_FAILURE";
  }
  return "UNKNOWN_ERROR";
}

}  // namespace xnn_transfer::core::security::identity
