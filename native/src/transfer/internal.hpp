#ifndef XNN_TRANSFER_SRC_TRANSFER_INTERNAL_HPP_
#define XNN_TRANSFER_SRC_TRANSFER_INTERNAL_HPP_

#include <cstddef>
#include <cstdint>
#include <span>

#include "xnn_transfer/core/transfer/transfer.hpp"

namespace xnn_transfer::core::transfer::internal {

class BodyBuilder final {
 public:
  [[nodiscard]] bool AddU8(std::uint16_t id, std::uint8_t value, bool critical = true);
  [[nodiscard]] bool AddU16(std::uint16_t id, std::uint16_t value,
                            bool critical = true);
  [[nodiscard]] bool AddU32(std::uint16_t id, std::uint32_t value,
                            bool critical = true);
  [[nodiscard]] bool AddU64(std::uint16_t id, std::uint64_t value,
                            bool critical = true);
  [[nodiscard]] bool AddBool(std::uint16_t id, bool value, bool critical = true);
  [[nodiscard]] bool AddBytes(std::uint16_t id, std::span<const std::uint8_t> value,
                              bool critical = true);
  [[nodiscard]] bool AddUtf8(std::uint16_t id, std::string_view value,
                             bool critical = true);

  [[nodiscard]] const Bytes& bytes() const noexcept { return bytes_; }
  [[nodiscard]] bool ok() const noexcept { return ok_; }

 private:
  [[nodiscard]] bool Add(std::uint16_t id, protocol::v1::WireType wire_type,
                         bool critical, std::span<const std::uint8_t> value);

  Bytes bytes_{};
  std::uint16_t previous_id_{};
  bool has_previous_id_{};
  bool ok_{true};
};

struct ParsedTransferFrame {
  protocol::v1::ParsedFrame frame{};
  TransferError error{TransferError::kNone};
  WireErrorCode wire_error{WireErrorCode::kNone};
  bool connection_fatal{};

  [[nodiscard]] bool ok() const noexcept { return error == TransferError::kNone; }
};

[[nodiscard]] bool ValidateContext(const TransferContext& context,
                                   bool local_is_stream_creator) noexcept;
[[nodiscard]] bool Authorized(const TransferContext& context) noexcept;
[[nodiscard]] bool AllZero(const TransferId& transfer_id) noexcept;
[[nodiscard]] bool ValidCommitment(std::span<const std::uint8_t> commitment) noexcept;
[[nodiscard]] bool ValidLimits(const TransferLimits& limits) noexcept;
[[nodiscard]] bool ObserveTime(std::uint64_t now_ms,
                               std::uint64_t& last_now_ms) noexcept;
[[nodiscard]] std::uint64_t CheckedDeadline(std::uint64_t now_ms,
                                            std::uint64_t duration_ms) noexcept;
[[nodiscard]] bool DeadlineReached(std::uint64_t now_ms,
                                   std::uint64_t deadline_ms) noexcept;
[[nodiscard]] bool IsTerminal(TransferState state) noexcept;
[[nodiscard]] bool DecodeWireError(std::uint64_t encoded,
                                   WireErrorCode& output) noexcept;
[[nodiscard]] TransferError TransferErrorForWire(WireErrorCode error) noexcept;
[[nodiscard]] bool WireErrorIsConnectionFatal(WireErrorCode error) noexcept;
[[nodiscard]] bool WireErrorIsFatal(WireErrorCode code) noexcept;
[[nodiscard]] bool WireErrorMayRetry(WireErrorCode error) noexcept;
[[nodiscard]] bool WireErrorIsRejectReason(WireErrorCode code) noexcept;

[[nodiscard]] bool EncodeFrame(const TransferContext& context,
                               protocol::v1::MessageType type,
                               ConnectionMessageSequence& message_ids,
                               const BodyBuilder& body, Bytes& output);
[[nodiscard]] bool EncodeFileChunkFrame(const TransferContext& context,
                                        ConnectionMessageSequence& message_ids,
                                        const TransferId& transfer_id,
                                        std::uint64_t offset,
                                        std::span<const std::uint8_t> data,
                                        std::span<const std::uint8_t> commitment,
                                        Bytes& output);
[[nodiscard]] bool EncodeErrorFrame(const TransferContext& context,
                                    ConnectionMessageSequence& message_ids,
                                    WireErrorCode code, bool retryable, Bytes& output);

[[nodiscard]] ParsedTransferFrame ParseInbound(
    const TransferContext& context, ConnectionMessageSequence& message_ids,
    std::span<const std::uint8_t> encoded) noexcept;
[[nodiscard]] bool ParseBody(ParsedTransferFrame& parsed) noexcept;

[[nodiscard]] const protocol::v1::FieldView* Field(
    const protocol::v1::ParsedFrame& frame, std::uint16_t id) noexcept;
[[nodiscard]] std::uint64_t Unsigned(const protocol::v1::ParsedFrame& frame,
                                     std::uint16_t id) noexcept;
[[nodiscard]] std::span<const std::uint8_t> FieldBytes(
    const protocol::v1::ParsedFrame& frame, std::uint16_t id) noexcept;
[[nodiscard]] bool ReadTransferId(const protocol::v1::ParsedFrame& frame,
                                  TransferId& output) noexcept;

[[nodiscard]] TransferUpdate FailureUpdate(TransferState state, TransferError error,
                                           WireErrorCode wire_error,
                                           bool connection_fatal = false);

}  // namespace xnn_transfer::core::transfer::internal

#endif  // XNN_TRANSFER_SRC_TRANSFER_INTERNAL_HPP_
