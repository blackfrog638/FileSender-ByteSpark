#ifndef XNN_TRANSFER_PROTOCOL_V1_PARSER_HPP_
#define XNN_TRANSFER_PROTOCOL_V1_PARSER_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace xnn_transfer::protocol::v1 {

inline constexpr std::size_t kFixedHeaderLength = 28;
inline constexpr std::size_t kMaxHeaderLength = 256;
inline constexpr std::size_t kMaxBodyLength = 1'048'576;
inline constexpr std::size_t kMaxEncodedFrameLength = kMaxHeaderLength + kMaxBodyLength;
inline constexpr std::size_t kMaxFields = 256;

enum class WireType : std::uint8_t {
  kU8 = 1,
  kU16 = 2,
  kU32 = 3,
  kU64 = 4,
  kBytes = 5,
  kUtf8 = 6,
  kBool = 7,
};

enum class MessageType : std::uint16_t {
  kHello = 0x0001,
  kNegotiate = 0x0002,
  kNegotiateAck = 0x0003,
  kError = 0x0004,
  kPing = 0x0005,
  kPong = 0x0006,
  kGoAway = 0x0007,
  kTransportFinished = 0x0008,
  kTransferOffer = 0x0100,
  kManifestEntry = 0x0101,
  kManifestEnd = 0x0102,
  kTransferAccept = 0x0103,
  kTransferReject = 0x0104,
  kFileBegin = 0x0110,
  kFileChunk = 0x0111,
  kChunkAck = 0x0112,
  kFileEnd = 0x0113,
  kFileCommit = 0x0114,
  kTransferComplete = 0x0120,
  kTransferCompleteAck = 0x0121,
  kCancel = 0x0130,
  kCancelAck = 0x0131,
  kResumeRequest = 0x0140,
  kResumeState = 0x0141,
  kResumeEnd = 0x0142,
};

enum class Direction : std::uint8_t {
  kInitiatorToResponder,
  kResponderToInitiator,
};

enum class ErrorCode : std::uint16_t {
  kNone = 0,
  kMalformedFrame = 0x0001,
  kFrameTooLarge = 0x0002,
  kMalformedMessage = 0x0003,
  kUnsupportedVersion = 0x0004,
  kDowngradeDetected = 0x0005,
  kUnsupportedCapability = 0x0006,
  kUnsupportedMessage = 0x0007,
  kUnknownCriticalField = 0x0008,
  kStateViolation = 0x0009,
  kMessageIdViolation = 0x000a,
  kLimitExceeded = 0x000b,
};

struct Error {
  ErrorCode code{ErrorCode::kNone};
  std::string_view detail{};

  [[nodiscard]] constexpr bool ok() const noexcept { return code == ErrorCode::kNone; }
};

[[nodiscard]] std::string_view ErrorCodeName(ErrorCode code) noexcept;

struct Version {
  std::uint8_t major{1};
  std::uint8_t minor{0};

  friend constexpr bool operator==(Version, Version) = default;
};

struct FrameHeader {
  std::uint16_t header_length{};
  Version version{};
  MessageType message_type{};
  std::uint16_t flags{};
  std::uint32_t stream_id{};
  std::uint64_t message_id{};
  std::uint32_t body_length{};
};

struct FieldView {
  std::uint16_t id{};
  WireType wire_type{};
  std::uint8_t flags{};
  std::span<const std::uint8_t> value{};

  [[nodiscard]] constexpr bool critical() const noexcept {
    return (flags & 0x01U) != 0U;
  }
};

struct FieldCollection {
  std::array<FieldView, kMaxFields> fields{};
  std::size_t count{};

  [[nodiscard]] const FieldView* FindFirst(std::uint16_t id) const noexcept;
  [[nodiscard]] std::size_t Count(std::uint16_t id) const noexcept;
};

// ParsedFrame contains views into the caller-owned encoded frame. The encoded
// bytes must outlive every use of the result.
struct ParsedFrame {
  FrameHeader header{};
  FieldCollection header_fields{};
  FieldCollection body_fields{};
  std::span<const std::uint8_t> raw{};
  std::span<const std::uint8_t> header_extensions{};
  std::span<const std::uint8_t> body{};
};

struct ParseResult {
  ParsedFrame frame{};
  Error error{};

  [[nodiscard]] constexpr bool ok() const noexcept { return error.ok(); }
};

[[nodiscard]] ParseResult ParseFrame(std::span<const std::uint8_t> encoded,
                                     Version expected_version = Version{}) noexcept;

[[nodiscard]] std::uint64_t DecodeUnsigned(const FieldView& field) noexcept;

// This class validates parser-level v1 negotiation transcripts. It does not
// open a channel, authenticate a peer, or verify TRANSPORT_FINISHED bytes.
class TranscriptParser final {
 public:
  TranscriptParser() = default;

  [[nodiscard]] Error Process(Direction direction,
                              std::span<const std::uint8_t> encoded) noexcept;
  [[nodiscard]] bool binding_frames_complete() const noexcept {
    return binding_frames_complete_;
  }

 private:
  struct HelloData {
    bool present{};
    Version minimum{};
    Version maximum{};
    std::uint8_t role{};
    std::array<std::uint32_t, kMaxFields> capabilities{};
    std::size_t capability_count{};
    std::array<std::uint32_t, kMaxFields> required_capabilities{};
    std::size_t required_capability_count{};
    std::uint32_t receive_max_body{};
    std::uint32_t receive_max_in_flight{};
    std::uint16_t receive_max_streams{};
  };

  struct NegotiationData {
    bool present{};
    Version selected_version{};
    std::array<std::uint32_t, kMaxFields> capabilities{};
    std::size_t capability_count{};
    std::uint32_t effective_max_body{};
    std::uint32_t effective_max_in_flight{};
    std::uint16_t effective_max_streams{};
  };

  [[nodiscard]] Error ProcessHello(Direction direction,
                                   const ParsedFrame& frame) noexcept;
  [[nodiscard]] Error ProcessNegotiate(Direction direction,
                                       const ParsedFrame& frame) noexcept;
  [[nodiscard]] Error ProcessNegotiateAck(Direction direction,
                                          const ParsedFrame& frame) noexcept;
  [[nodiscard]] Error ProcessTransportFinished(Direction direction,
                                               const ParsedFrame& frame) noexcept;
  [[nodiscard]] Error ProcessPing(Direction direction,
                                  const ParsedFrame& frame) noexcept;
  [[nodiscard]] Error ProcessPong(Direction direction,
                                  const ParsedFrame& frame) noexcept;

  std::array<std::uint64_t, 2> next_message_id_{1, 1};
  std::array<bool, 2> message_id_exhausted_{};
  std::array<HelloData, 2> hellos_{};
  NegotiationData negotiation_{};
  bool negotiation_acknowledged_{};
  std::array<bool, 2> transport_finished_{};
  bool binding_frames_complete_{};
  std::array<bool, 2> pong_expected_{};
  std::array<std::uint64_t, 2> expected_pong_token_{};
};

}  // namespace xnn_transfer::protocol::v1

#endif  // XNN_TRANSFER_PROTOCOL_V1_PARSER_HPP_
