#ifndef XNN_TRANSFER_SRC_SESSION_INTERNAL_HPP_
#define XNN_TRANSFER_SRC_SESSION_INTERNAL_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "xnn_transfer/core/session/session.hpp"

namespace xnn_transfer::core::session::internal {

inline constexpr std::uint32_t kBaseTransferV1 = 0x0001'0001U;

enum class PairingMessageType : std::uint16_t {
  kHello = 0x0001,
  kSelect = 0x0002,
  kSelectAck = 0x0003,
  kDecision = 0x0004,
  kAbort = 0x0005,
};

enum class WireType : std::uint8_t {
  kU8 = 1,
  kU16 = 2,
  kU32 = 3,
  kU64 = 4,
  kBytes = 5,
};

struct Field {
  std::uint16_t id{};
  WireType type{WireType::kBytes};
  Bytes value{};
};

struct Frame {
  PairingMessageType type{PairingMessageType::kHello};
  std::uint32_t sequence{};
  std::vector<Field> fields{};
};

struct Hello {
  security::tls::Role role{security::tls::Role::kInitiator};
  security::tls::Nonce256 nonce{};
  std::optional<security::tls::ValidatedEd25519PublicKey> key{};
  NegotiationOffer offer{};
};

struct Selection {
  Version selected_version{};
  std::vector<std::uint32_t> selected_capabilities{};
  ReceiveLimits effective_limits{};

  friend bool operator==(const Selection&, const Selection&) = default;
};

struct Decision {
  std::array<std::uint8_t, security::tls::kSha256Size + 2> message{};
  security::tls::Digest256 authenticator{};
};

struct ParseResult {
  Frame frame{};
  PairingError error{PairingError::kNone};

  [[nodiscard]] bool ok() const noexcept { return error == PairingError::kNone; }
};

[[nodiscard]] ParseResult ParseFrame(std::span<const std::uint8_t> encoded) noexcept;
[[nodiscard]] PairingError DecodeHello(
    const Frame& frame, security::tls::Role expected_role,
    const security::tls::ValidatedEd25519PublicKey& expected_key, Hello& output);
[[nodiscard]] PairingError DecodeSelection(const Frame& frame, Selection& output);
[[nodiscard]] PairingError DecodeDecision(const Frame& frame, Decision& output);
[[nodiscard]] PairingError DecodeAbort(const Frame& frame);

[[nodiscard]] PairingError ValidateOffer(const NegotiationOffer& offer);
[[nodiscard]] PairingError Select(const NegotiationOffer& initiator,
                                  const NegotiationOffer& responder, Selection& output);
[[nodiscard]] security::tls::Result<security::tls::NormalizedNegotiation>
BuildNormalizedNegotiation(const Hello& initiator, const Hello& responder,
                           const Selection& selection);

[[nodiscard]] Bytes EncodeHello(std::uint32_t sequence, security::tls::Role role,
                                const security::tls::ValidatedEd25519PublicKey& key,
                                const security::tls::Nonce256& nonce,
                                const NegotiationOffer& offer);
[[nodiscard]] Bytes EncodeSelection(PairingMessageType type, std::uint32_t sequence,
                                    const Selection& selection);
[[nodiscard]] Bytes EncodeDecision(std::uint32_t sequence,
                                   const security::tls::ConfirmationValue& decision);
[[nodiscard]] Bytes EncodeAbort(std::uint32_t sequence, std::uint16_t public_code);

[[nodiscard]] bool VersionLess(Version left, Version right) noexcept;
[[nodiscard]] security::tls::Role OppositeRole(security::tls::Role role) noexcept;

}  // namespace xnn_transfer::core::session::internal

#endif  // XNN_TRANSFER_SRC_SESSION_INTERNAL_HPP_
