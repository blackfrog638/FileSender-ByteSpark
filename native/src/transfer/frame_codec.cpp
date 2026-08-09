#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

#include "internal.hpp"

namespace xnn_transfer::core::transfer {
namespace {

[[nodiscard]] TransferError TransferErrorForProtocol(
    const protocol::v1::ErrorCode error) noexcept {
  switch (error) {
    case protocol::v1::ErrorCode::kNone:
      return TransferError::kNone;
    case protocol::v1::ErrorCode::kMalformedFrame:
    case protocol::v1::ErrorCode::kFrameTooLarge:
    case protocol::v1::ErrorCode::kUnsupportedVersion:
    case protocol::v1::ErrorCode::kUnsupportedCapability:
    case protocol::v1::ErrorCode::kUnsupportedMessage:
    case protocol::v1::ErrorCode::kDowngradeDetected:
      return TransferError::kMalformedFrame;
    case protocol::v1::ErrorCode::kMalformedMessage:
    case protocol::v1::ErrorCode::kUnknownCriticalField:
      return TransferError::kMalformedMessage;
    case protocol::v1::ErrorCode::kStateViolation:
      return TransferError::kStateViolation;
    case protocol::v1::ErrorCode::kMessageIdViolation:
      return TransferError::kMessageIdViolation;
    case protocol::v1::ErrorCode::kLimitExceeded:
      return TransferError::kLimitExceeded;
  }
  return TransferError::kMalformedMessage;
}

[[nodiscard]] bool ProtocolErrorIsConnectionFatal(
    const protocol::v1::ErrorCode error, const std::uint32_t stream_id) noexcept {
  switch (error) {
    case protocol::v1::ErrorCode::kMalformedFrame:
    case protocol::v1::ErrorCode::kFrameTooLarge:
    case protocol::v1::ErrorCode::kUnsupportedVersion:
    case protocol::v1::ErrorCode::kDowngradeDetected:
    case protocol::v1::ErrorCode::kUnsupportedCapability:
    case protocol::v1::ErrorCode::kUnsupportedMessage:
    case protocol::v1::ErrorCode::kMessageIdViolation:
      return true;
    case protocol::v1::ErrorCode::kNone:
    case protocol::v1::ErrorCode::kMalformedMessage:
    case protocol::v1::ErrorCode::kUnknownCriticalField:
    case protocol::v1::ErrorCode::kLimitExceeded:
      return false;
    case protocol::v1::ErrorCode::kStateViolation:
      return stream_id == 0;
  }
  return true;
}

void AppendU16(Bytes& output, const std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value));
}

void AppendU32(Bytes& output, const std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 24U));
  output.push_back(static_cast<std::uint8_t>(value >> 16U));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value));
}

void StoreU64(Bytes& output, const std::size_t offset,
              const std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < 8; ++index) {
    const std::size_t shift = (7U - index) * 8U;
    output[offset + index] =
        static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift));
  }
}

[[nodiscard]] bool CreditIndex(const CreditDirection direction,
                               std::size_t& output) noexcept {
  switch (direction) {
    case CreditDirection::kOutbound:
      output = 0;
      return true;
    case CreditDirection::kInbound:
      output = 1;
      return true;
  }
  return false;
}

}  // namespace

std::string_view TransferErrorName(const TransferError error) noexcept {
  switch (error) {
    case TransferError::kNone:
      return "NONE";
    case TransferError::kInvalidArgument:
      return "INVALID_ARGUMENT";
    case TransferError::kUnauthenticated:
      return "UNAUTHENTICATED";
    case TransferError::kMalformedFrame:
      return "MALFORMED_FRAME";
    case TransferError::kMalformedMessage:
      return "MALFORMED_MESSAGE";
    case TransferError::kStateViolation:
      return "STATE_VIOLATION";
    case TransferError::kMessageIdViolation:
      return "MESSAGE_ID_VIOLATION";
    case TransferError::kLimitExceeded:
      return "LIMIT_EXCEEDED";
    case TransferError::kInvalidOffer:
      return "INVALID_OFFER";
    case TransferError::kInvalidManifest:
      return "INVALID_MANIFEST";
    case TransferError::kPolicyRejected:
      return "POLICY_REJECTED";
    case TransferError::kNoSpace:
      return "NO_SPACE";
    case TransferError::kBusy:
      return "BUSY";
    case TransferError::kIoFailure:
      return "IO_FAILURE";
    case TransferError::kIntegrityFailed:
      return "INTEGRITY_FAILED";
    case TransferError::kTimeout:
      return "TIMEOUT";
    case TransferError::kIdempotencyConflict:
      return "IDEMPOTENCY_CONFLICT";
    case TransferError::kSourceFailure:
      return "SOURCE_FAILURE";
    case TransferError::kInternalFailure:
      return "INTERNAL_FAILURE";
  }
  return "INTERNAL_FAILURE";
}

ConnectionMessageSequence::ConnectionMessageSequence(
    const std::uint64_t next_outbound, const std::uint64_t next_inbound) noexcept
    : next_outbound_(next_outbound),
      next_inbound_(next_inbound),
      outbound_exhausted_(next_outbound == 0),
      inbound_exhausted_(next_inbound == 0) {}

bool ConnectionMessageSequence::NextOutbound(std::uint64_t& output) noexcept {
  const std::lock_guard lock(mutex_);
  if (outbound_exhausted_) {
    return false;
  }
  output = next_outbound_;
  if (next_outbound_ == std::numeric_limits<std::uint64_t>::max()) {
    outbound_exhausted_ = true;
  } else {
    ++next_outbound_;
  }
  return true;
}

bool ConnectionMessageSequence::ObserveInbound(
    const std::uint64_t message_id) noexcept {
  const std::lock_guard lock(mutex_);
  if (inbound_exhausted_) {
    return false;
  }
  if (message_id != next_inbound_) {
    inbound_exhausted_ = true;
    return false;
  }
  if (next_inbound_ == std::numeric_limits<std::uint64_t>::max()) {
    inbound_exhausted_ = true;
  } else {
    ++next_inbound_;
  }
  return true;
}

bool ConnectionMessageSequence::OutboundExhausted() const noexcept {
  const std::lock_guard lock(mutex_);
  return outbound_exhausted_;
}

ConnectionCreditBudget::ConnectionCreditBudget(
    const std::uint32_t limit_bytes, const std::uint16_t maximum_streams) noexcept
    : limit_bytes_(std::min(limit_bytes, kMaximumConnectionWindow)),
      maximum_streams_(std::min<std::uint16_t>(maximum_streams, 32)) {}

bool ConnectionCreditBudget::TryOpenStream(
    const std::uint32_t stream_id, const bool local_is_creator,
    const security::tls::Role local_role) noexcept {
  return OpenStream(stream_id, local_is_creator, local_role) ==
         StreamOpenResult::kOpened;
}

StreamOpenResult ConnectionCreditBudget::OpenStream(
    const std::uint32_t stream_id, const bool local_is_creator,
    const security::tls::Role local_role) noexcept {
  const std::lock_guard lock(mutex_);
  if (stream_id == 0 || (local_role != security::tls::Role::kInitiator &&
                         local_role != security::tls::Role::kResponder)) {
    return StreamOpenResult::kInvalid;
  }
  if (!role_initialized_) {
    local_role_ = local_role;
    next_local_stream_id_ = local_role == security::tls::Role::kInitiator ? 1U : 2U;
    next_peer_stream_id_ = local_role == security::tls::Role::kInitiator ? 2U : 1U;
    role_initialized_ = true;
  } else if (local_role_ != local_role) {
    return StreamOpenResult::kInvalid;
  }

  std::uint32_t& expected =
      local_is_creator ? next_local_stream_id_ : next_peer_stream_id_;
  bool& exhausted =
      local_is_creator ? local_stream_ids_exhausted_ : peer_stream_ids_exhausted_;
  if (exhausted || stream_id != expected ||
      std::find(active_stream_ids_.begin(),
                active_stream_ids_.begin() + active_streams_,
                stream_id) != active_stream_ids_.begin() + active_streams_) {
    return StreamOpenResult::kInvalid;
  }
  const auto consume_id = [&expected, &exhausted]() {
    if (expected > std::numeric_limits<std::uint32_t>::max() - 2U) {
      exhausted = true;
    } else {
      expected += 2U;
    }
  };
  if (active_streams_ >= maximum_streams_) {
    if (!local_is_creator) {
      consume_id();
    }
    return StreamOpenResult::kCapacityRejected;
  }
  active_stream_ids_[active_streams_] = stream_id;
  ++active_streams_;
  consume_id();
  return StreamOpenResult::kOpened;
}

void ConnectionCreditBudget::CloseStream(const std::uint32_t stream_id) noexcept {
  const std::lock_guard lock(mutex_);
  const auto end = active_stream_ids_.begin() + active_streams_;
  const auto found = std::find(active_stream_ids_.begin(), end, stream_id);
  if (found == end) {
    return;
  }
  --active_streams_;
  *found = active_stream_ids_[active_streams_];
  active_stream_ids_[active_streams_] = 0;
}

bool ConnectionCreditBudget::TryReserve(const CreditDirection direction,
                                        const std::uint32_t bytes) noexcept {
  std::size_t index = 0;
  if (!CreditIndex(direction, index)) {
    return false;
  }
  const std::lock_guard lock(mutex_);
  if (bytes > limit_bytes_ || reserved_bytes_[index] > limit_bytes_ - bytes) {
    return false;
  }
  reserved_bytes_[index] += bytes;
  return true;
}

bool ConnectionCreditBudget::TryAdjust(const CreditDirection direction,
                                       const std::uint32_t release_bytes,
                                       const std::uint32_t reserve_bytes) noexcept {
  std::size_t index = 0;
  if (!CreditIndex(direction, index)) {
    return false;
  }
  const std::lock_guard lock(mutex_);
  if (release_bytes > reserved_bytes_[index]) {
    return false;
  }
  const std::uint32_t after_release = reserved_bytes_[index] - release_bytes;
  if (reserve_bytes > limit_bytes_ || after_release > limit_bytes_ - reserve_bytes) {
    return false;
  }
  reserved_bytes_[index] = after_release + reserve_bytes;
  return true;
}

void ConnectionCreditBudget::Release(const CreditDirection direction,
                                     const std::uint32_t bytes) noexcept {
  std::size_t index = 0;
  if (!CreditIndex(direction, index)) {
    return;
  }
  const std::lock_guard lock(mutex_);
  if (bytes > reserved_bytes_[index]) {
    return;
  }
  reserved_bytes_[index] -= bytes;
}

std::uint32_t ConnectionCreditBudget::limit_bytes() const noexcept {
  const std::lock_guard lock(mutex_);
  return limit_bytes_;
}

std::uint32_t ConnectionCreditBudget::reserved_bytes(
    const CreditDirection direction) const noexcept {
  std::size_t index = 0;
  if (!CreditIndex(direction, index)) {
    return 0;
  }
  const std::lock_guard lock(mutex_);
  return reserved_bytes_[index];
}

std::uint16_t ConnectionCreditBudget::maximum_streams() const noexcept {
  const std::lock_guard lock(mutex_);
  return maximum_streams_;
}

std::uint16_t ConnectionCreditBudget::active_streams() const noexcept {
  const std::lock_guard lock(mutex_);
  return active_streams_;
}

namespace internal {

bool BodyBuilder::Add(const std::uint16_t id, const protocol::v1::WireType wire_type,
                      const bool critical, const std::span<const std::uint8_t> value) {
  if (!ok_ || id == 0 || (has_previous_id_ && id <= previous_id_) ||
      value.size() > std::numeric_limits<std::uint32_t>::max()) {
    ok_ = false;
    return false;
  }
  try {
    const std::size_t required = 8U + value.size();
    if (required > protocol::v1::kMaxBodyLength ||
        bytes_.size() > protocol::v1::kMaxBodyLength - required) {
      ok_ = false;
      return false;
    }
    AppendU16(bytes_, id);
    bytes_.push_back(static_cast<std::uint8_t>(wire_type));
    bytes_.push_back(critical ? 0x01U : 0x00U);
    AppendU32(bytes_, static_cast<std::uint32_t>(value.size()));
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    previous_id_ = id;
    has_previous_id_ = true;
    return true;
  } catch (const std::bad_alloc&) {
    ok_ = false;
    return false;
  }
}

bool BodyBuilder::AddU8(const std::uint16_t id, const std::uint8_t value,
                        const bool critical) {
  return Add(id, protocol::v1::WireType::kU8, critical,
             std::span<const std::uint8_t>(&value, 1));
}

bool BodyBuilder::AddU16(const std::uint16_t id, const std::uint16_t value,
                         const bool critical) {
  const std::array<std::uint8_t, 2> encoded{
      static_cast<std::uint8_t>(value >> 8U),
      static_cast<std::uint8_t>(value),
  };
  return Add(id, protocol::v1::WireType::kU16, critical, encoded);
}

bool BodyBuilder::AddU32(const std::uint16_t id, const std::uint32_t value,
                         const bool critical) {
  const std::array<std::uint8_t, 4> encoded{
      static_cast<std::uint8_t>(value >> 24U),
      static_cast<std::uint8_t>(value >> 16U),
      static_cast<std::uint8_t>(value >> 8U),
      static_cast<std::uint8_t>(value),
  };
  return Add(id, protocol::v1::WireType::kU32, critical, encoded);
}

bool BodyBuilder::AddU64(const std::uint16_t id, const std::uint64_t value,
                         const bool critical) {
  std::array<std::uint8_t, 8> encoded{};
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    const std::size_t shift = (encoded.size() - index - 1U) * 8U;
    encoded[index] = static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift));
  }
  return Add(id, protocol::v1::WireType::kU64, critical, encoded);
}

bool BodyBuilder::AddBool(const std::uint16_t id, const bool value,
                          const bool critical) {
  const std::uint8_t encoded = value ? 1U : 0U;
  return Add(id, protocol::v1::WireType::kBool, critical,
             std::span<const std::uint8_t>(&encoded, 1));
}

bool BodyBuilder::AddBytes(const std::uint16_t id,
                           const std::span<const std::uint8_t> value,
                           const bool critical) {
  return Add(id, protocol::v1::WireType::kBytes, critical, value);
}

bool BodyBuilder::AddUtf8(const std::uint16_t id, const std::string_view value,
                          const bool critical) {
  return Add(id, protocol::v1::WireType::kUtf8, critical,
             {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

bool ValidateContext(const TransferContext& context,
                     const bool local_is_stream_creator) noexcept {
  if (context.authority == nullptr || context.stream_id == 0 ||
      context.version != protocol::v1::Version{} ||
      (context.local_role != security::tls::Role::kInitiator &&
       context.local_role != security::tls::Role::kResponder) ||
      !ValidLimits(context.limits)) {
    return false;
  }
  security::tls::Role creator = context.local_role;
  if (!local_is_stream_creator) {
    creator = context.local_role == security::tls::Role::kInitiator
                  ? security::tls::Role::kResponder
                  : security::tls::Role::kInitiator;
  }
  const bool odd = (context.stream_id & 1U) != 0U;
  return (creator == security::tls::Role::kInitiator && odd) ||
         (creator == security::tls::Role::kResponder && !odd);
}

bool Authorized(const TransferContext& context) noexcept {
  return context.authority != nullptr &&
         context.authority->IsAuthorized(context.session_handle);
}

bool AllZero(const TransferId& transfer_id) noexcept {
  std::uint8_t aggregate = 0;
  for (const std::uint8_t byte : transfer_id) {
    aggregate = static_cast<std::uint8_t>(aggregate | byte);
  }
  return aggregate == 0;
}

bool ValidCommitment(const std::span<const std::uint8_t> commitment) noexcept {
  return commitment.size() >= 16U && commitment.size() <= 64U;
}

bool ValidLimits(const TransferLimits& limits) noexcept {
  return limits.maximum_body >= 8'192U &&
         limits.maximum_body <= protocol::v1::kMaxBodyLength &&
         limits.maximum_in_flight >= 1'048'576U &&
         limits.maximum_in_flight <= kMaximumConnectionWindow &&
         limits.maximum_active_streams != 0 && limits.maximum_active_streams <= 32 &&
         limits.preferred_chunk_size >= kMinimumChunkSize &&
         limits.preferred_chunk_size <= kMaximumChunkSize &&
         limits.preferred_initial_window >= limits.preferred_chunk_size &&
         limits.preferred_initial_window <= kMaximumTransferWindow &&
         limits.preferred_initial_window <= limits.maximum_in_flight;
}

bool ObserveTime(const std::uint64_t now_ms, std::uint64_t& last_now_ms) noexcept {
  if (now_ms < last_now_ms) {
    return false;
  }
  last_now_ms = now_ms;
  return true;
}

std::uint64_t CheckedDeadline(const std::uint64_t now_ms,
                              const std::uint64_t duration_ms) noexcept {
  if (duration_ms > std::numeric_limits<std::uint64_t>::max() - now_ms) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return now_ms + duration_ms;
}

bool DeadlineReached(const std::uint64_t now_ms,
                     const std::uint64_t deadline_ms) noexcept {
  return deadline_ms != 0 && now_ms >= deadline_ms;
}

bool IsTerminal(const TransferState state) noexcept {
  return state == TransferState::kCommitted || state == TransferState::kCompleted ||
         state == TransferState::kRejected || state == TransferState::kFailed;
}

bool DecodeWireError(const std::uint64_t encoded, WireErrorCode& output) noexcept {
  switch (static_cast<WireErrorCode>(encoded)) {
    case WireErrorCode::kMalformedFrame:
    case WireErrorCode::kFrameTooLarge:
    case WireErrorCode::kMalformedMessage:
    case WireErrorCode::kUnsupportedVersion:
    case WireErrorCode::kDowngradeDetected:
    case WireErrorCode::kUnsupportedCapability:
    case WireErrorCode::kUnsupportedMessage:
    case WireErrorCode::kUnknownCriticalField:
    case WireErrorCode::kStateViolation:
    case WireErrorCode::kMessageIdViolation:
    case WireErrorCode::kLimitExceeded:
    case WireErrorCode::kTimeout:
    case WireErrorCode::kInvalidOffer:
    case WireErrorCode::kInvalidManifest:
    case WireErrorCode::kPolicyRejected:
    case WireErrorCode::kNoSpace:
    case WireErrorCode::kBusy:
    case WireErrorCode::kIoFailure:
    case WireErrorCode::kIntegrityFailed:
    case WireErrorCode::kCancelled:
    case WireErrorCode::kResumeUnavailable:
    case WireErrorCode::kExpired:
    case WireErrorCode::kIdempotencyConflict:
    case WireErrorCode::kCompleted:
      output = static_cast<WireErrorCode>(encoded);
      return true;
    case WireErrorCode::kNone:
      return false;
  }
  return false;
}

TransferError TransferErrorForWire(const WireErrorCode error) noexcept {
  switch (error) {
    case WireErrorCode::kMalformedFrame:
    case WireErrorCode::kFrameTooLarge:
    case WireErrorCode::kUnsupportedVersion:
    case WireErrorCode::kDowngradeDetected:
    case WireErrorCode::kUnsupportedCapability:
    case WireErrorCode::kUnsupportedMessage:
      return TransferError::kMalformedFrame;
    case WireErrorCode::kMalformedMessage:
    case WireErrorCode::kUnknownCriticalField:
      return TransferError::kMalformedMessage;
    case WireErrorCode::kStateViolation:
      return TransferError::kStateViolation;
    case WireErrorCode::kMessageIdViolation:
      return TransferError::kMessageIdViolation;
    case WireErrorCode::kLimitExceeded:
      return TransferError::kLimitExceeded;
    case WireErrorCode::kTimeout:
      return TransferError::kTimeout;
    case WireErrorCode::kInvalidOffer:
      return TransferError::kInvalidOffer;
    case WireErrorCode::kInvalidManifest:
      return TransferError::kInvalidManifest;
    case WireErrorCode::kPolicyRejected:
    case WireErrorCode::kCancelled:
    case WireErrorCode::kResumeUnavailable:
    case WireErrorCode::kExpired:
    case WireErrorCode::kCompleted:
      return TransferError::kPolicyRejected;
    case WireErrorCode::kNoSpace:
      return TransferError::kNoSpace;
    case WireErrorCode::kBusy:
      return TransferError::kBusy;
    case WireErrorCode::kIoFailure:
      return TransferError::kIoFailure;
    case WireErrorCode::kIntegrityFailed:
      return TransferError::kIntegrityFailed;
    case WireErrorCode::kIdempotencyConflict:
      return TransferError::kIdempotencyConflict;
    case WireErrorCode::kNone:
      return TransferError::kNone;
  }
  return TransferError::kMalformedMessage;
}

bool WireErrorIsConnectionFatal(const WireErrorCode error) noexcept {
  switch (error) {
    case WireErrorCode::kMalformedFrame:
    case WireErrorCode::kFrameTooLarge:
    case WireErrorCode::kUnsupportedVersion:
    case WireErrorCode::kDowngradeDetected:
    case WireErrorCode::kUnsupportedCapability:
    case WireErrorCode::kUnsupportedMessage:
    case WireErrorCode::kMessageIdViolation:
      return true;
    default:
      return false;
  }
}

bool WireErrorIsFatal(const WireErrorCode error) noexcept {
  if (WireErrorIsConnectionFatal(error)) {
    return true;
  }
  switch (error) {
    case WireErrorCode::kNone:
    case WireErrorCode::kPolicyRejected:
    case WireErrorCode::kNoSpace:
    case WireErrorCode::kBusy:
    case WireErrorCode::kIoFailure:
    case WireErrorCode::kCancelled:
    case WireErrorCode::kResumeUnavailable:
    case WireErrorCode::kExpired:
    case WireErrorCode::kCompleted:
      return false;
    case WireErrorCode::kMalformedMessage:
    case WireErrorCode::kUnknownCriticalField:
    case WireErrorCode::kLimitExceeded:
    case WireErrorCode::kStateViolation:
      return true;
    case WireErrorCode::kTimeout:
    case WireErrorCode::kInvalidOffer:
    case WireErrorCode::kInvalidManifest:
    case WireErrorCode::kIntegrityFailed:
    case WireErrorCode::kIdempotencyConflict:
      return true;
    case WireErrorCode::kMalformedFrame:
    case WireErrorCode::kFrameTooLarge:
    case WireErrorCode::kUnsupportedVersion:
    case WireErrorCode::kDowngradeDetected:
    case WireErrorCode::kUnsupportedCapability:
    case WireErrorCode::kUnsupportedMessage:
    case WireErrorCode::kMessageIdViolation:
      return true;
  }
  return true;
}

bool WireErrorMayRetry(const WireErrorCode error) noexcept {
  return error == WireErrorCode::kTimeout || error == WireErrorCode::kNoSpace ||
         error == WireErrorCode::kBusy || error == WireErrorCode::kIoFailure;
}

bool WireErrorIsRejectReason(const WireErrorCode error) noexcept {
  return error == WireErrorCode::kPolicyRejected || error == WireErrorCode::kNoSpace ||
         error == WireErrorCode::kBusy || error == WireErrorCode::kIoFailure;
}

bool EncodeFrame(const TransferContext& context, const protocol::v1::MessageType type,
                 ConnectionMessageSequence& message_ids, const BodyBuilder& body,
                 Bytes& output) {
  if (!body.ok() || body.bytes().size() > context.limits.maximum_body ||
      body.bytes().size() > protocol::v1::kMaxBodyLength) {
    return false;
  }
  Bytes encoded;
  try {
    encoded.reserve(protocol::v1::kFixedHeaderLength + body.bytes().size());
    encoded.insert(encoded.end(), {'X', 'N', 'N', 'T'});
    AppendU16(encoded, static_cast<std::uint16_t>(protocol::v1::kFixedHeaderLength));
    encoded.push_back(context.version.major);
    encoded.push_back(context.version.minor);
    AppendU16(encoded, static_cast<std::uint16_t>(type));
    AppendU16(encoded, 0);
    AppendU32(encoded, context.stream_id);
    encoded.insert(encoded.end(), 8, 0);
    AppendU32(encoded, static_cast<std::uint32_t>(body.bytes().size()));
    encoded.insert(encoded.end(), body.bytes().begin(), body.bytes().end());
  } catch (const std::bad_alloc&) {
    return false;
  }

  StoreU64(encoded, 16, 1);
  const protocol::v1::ParseResult self_check =
      protocol::v1::ParseFrame(encoded, context.version);
  if (!self_check.ok() || self_check.frame.header.message_type != type ||
      self_check.frame.header.stream_id != context.stream_id) {
    return false;
  }

  std::uint64_t message_id = 0;
  if (!message_ids.NextOutbound(message_id) || message_id == 0) {
    return false;
  }
  StoreU64(encoded, 16, message_id);
  output = std::move(encoded);
  return true;
}

bool EncodeFileChunkFrame(const TransferContext& context,
                          ConnectionMessageSequence& message_ids,
                          const TransferId& transfer_id, const std::uint64_t offset,
                          const std::span<const std::uint8_t> data,
                          const std::span<const std::uint8_t> commitment,
                          Bytes& output) {
  BodyBuilder prefix;
  static_cast<void>(prefix.AddBytes(1, transfer_id));
  static_cast<void>(prefix.AddU32(2, 0));
  static_cast<void>(prefix.AddU64(3, offset));
  if (!prefix.ok() || data.empty() || data.size() > kMaximumChunkSize ||
      !ValidCommitment(commitment)) {
    return false;
  }
  constexpr std::size_t kTrailingFieldHeaders = 16;
  const std::size_t variable_size = data.size() + commitment.size();
  if (variable_size > protocol::v1::kMaxBodyLength - kTrailingFieldHeaders ||
      prefix.bytes().size() >
          protocol::v1::kMaxBodyLength - kTrailingFieldHeaders - variable_size) {
    return false;
  }
  const std::size_t body_size =
      prefix.bytes().size() + kTrailingFieldHeaders + variable_size;
  if (body_size > context.limits.maximum_body ||
      body_size > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }

  Bytes encoded;
  try {
    encoded.reserve(protocol::v1::kFixedHeaderLength + body_size);
    encoded.insert(encoded.end(), {'X', 'N', 'N', 'T'});
    AppendU16(encoded, static_cast<std::uint16_t>(protocol::v1::kFixedHeaderLength));
    encoded.push_back(context.version.major);
    encoded.push_back(context.version.minor);
    AppendU16(encoded,
              static_cast<std::uint16_t>(protocol::v1::MessageType::kFileChunk));
    AppendU16(encoded, 0);
    AppendU32(encoded, context.stream_id);
    encoded.insert(encoded.end(), 8, 0);
    AppendU32(encoded, static_cast<std::uint32_t>(body_size));
    encoded.insert(encoded.end(), prefix.bytes().begin(), prefix.bytes().end());

    const auto append_bytes_field =
        [&encoded](const std::uint16_t id, const std::span<const std::uint8_t> value) {
          AppendU16(encoded, id);
          encoded.push_back(static_cast<std::uint8_t>(protocol::v1::WireType::kBytes));
          encoded.push_back(0x01U);
          AppendU32(encoded, static_cast<std::uint32_t>(value.size()));
          encoded.insert(encoded.end(), value.begin(), value.end());
        };
    append_bytes_field(4, data);
    append_bytes_field(5, commitment);
  } catch (const std::bad_alloc&) {
    return false;
  }

  StoreU64(encoded, 16, 1);
  const protocol::v1::ParseResult self_check =
      protocol::v1::ParseFrame(encoded, context.version);
  if (!self_check.ok() ||
      self_check.frame.header.message_type != protocol::v1::MessageType::kFileChunk ||
      self_check.frame.header.stream_id != context.stream_id) {
    return false;
  }
  std::uint64_t message_id = 0;
  if (!message_ids.NextOutbound(message_id) || message_id == 0) {
    return false;
  }
  StoreU64(encoded, 16, message_id);
  output = std::move(encoded);
  return true;
}

bool EncodeErrorFrame(const TransferContext& context,
                      ConnectionMessageSequence& message_ids, const WireErrorCode code,
                      const bool retryable, Bytes& output) {
  BodyBuilder body;
  static_cast<void>(body.AddU16(1, static_cast<std::uint16_t>(code)));
  static_cast<void>(body.AddBool(2, WireErrorIsFatal(code)));
  if (retryable) {
    static_cast<void>(body.AddBool(3, true, false));
  }
  return EncodeFrame(context, protocol::v1::MessageType::kError, message_ids, body,
                     output);
}

ParsedTransferFrame ParseInbound(const TransferContext& context,
                                 ConnectionMessageSequence& message_ids,
                                 const std::span<const std::uint8_t> encoded) noexcept {
  ParsedTransferFrame result{};
  if (!Authorized(context)) {
    result.error = TransferError::kUnauthenticated;
    result.connection_fatal = true;
    return result;
  }

  const protocol::v1::ParseResult parsed =
      protocol::v1::ParseFrameEnvelope(encoded, context.version);
  if (!parsed.ok()) {
    result.error = TransferErrorForProtocol(parsed.error.code);
    result.wire_error =
        static_cast<WireErrorCode>(static_cast<std::uint16_t>(parsed.error.code));
    result.connection_fatal = ProtocolErrorIsConnectionFatal(
        parsed.error.code, parsed.frame.header.stream_id);
    return result;
  }
  if (!message_ids.ObserveInbound(parsed.frame.header.message_id)) {
    result.error = TransferError::kMessageIdViolation;
    result.wire_error = WireErrorCode::kMessageIdViolation;
    result.connection_fatal = true;
    return result;
  }
  if (parsed.frame.header.body_length > context.limits.maximum_body) {
    result.error = TransferError::kLimitExceeded;
    result.wire_error = WireErrorCode::kLimitExceeded;
    return result;
  }
  if (parsed.frame.header.stream_id != context.stream_id) {
    result.error = TransferError::kStateViolation;
    result.wire_error = WireErrorCode::kStateViolation;
    return result;
  }
  result.frame = parsed.frame;
  return result;
}

bool ParseBody(ParsedTransferFrame& parsed) noexcept {
  const protocol::v1::Error error = protocol::v1::ParseFrameBody(parsed.frame);
  if (error.ok()) {
    return true;
  }
  parsed.error = TransferErrorForProtocol(error.code);
  parsed.wire_error =
      static_cast<WireErrorCode>(static_cast<std::uint16_t>(error.code));
  parsed.connection_fatal =
      ProtocolErrorIsConnectionFatal(error.code, parsed.frame.header.stream_id);
  return false;
}

const protocol::v1::FieldView* Field(const protocol::v1::ParsedFrame& frame,
                                     const std::uint16_t id) noexcept {
  return frame.body_fields.FindFirst(id);
}

std::uint64_t Unsigned(const protocol::v1::ParsedFrame& frame,
                       const std::uint16_t id) noexcept {
  const protocol::v1::FieldView* const field = Field(frame, id);
  return field == nullptr ? 0 : protocol::v1::DecodeUnsigned(*field);
}

std::span<const std::uint8_t> FieldBytes(const protocol::v1::ParsedFrame& frame,
                                         const std::uint16_t id) noexcept {
  const protocol::v1::FieldView* const field = Field(frame, id);
  return field == nullptr ? std::span<const std::uint8_t>{} : field->value;
}

bool ReadTransferId(const protocol::v1::ParsedFrame& frame,
                    TransferId& output) noexcept {
  const std::span<const std::uint8_t> encoded = FieldBytes(frame, 1);
  if (encoded.size() != output.size()) {
    return false;
  }
  std::copy(encoded.begin(), encoded.end(), output.begin());
  return true;
}

TransferUpdate FailureUpdate(const TransferState state, const TransferError error,
                             const WireErrorCode wire_error,
                             const bool connection_fatal) {
  return {
      .state = state,
      .error = error,
      .wire_error = wire_error,
      .terminal = IsTerminal(state),
      .connection_fatal = connection_fatal,
  };
}

}  // namespace internal
}  // namespace xnn_transfer::core::transfer
