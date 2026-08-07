#ifndef XNN_TRANSFER_CORE_DISCOVERY_DISCOVERY_HPP_
#define XNN_TRANSFER_CORE_DISCOVERY_DISCOVERY_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xnn_transfer::core::discovery {

inline constexpr std::uint16_t kDiscoveryPort = 45'878;
inline constexpr std::size_t kDiscoveryHeaderSize = 44;
inline constexpr std::size_t kMaxDatagramSize = 512;
inline constexpr std::size_t kMaxDisplayLabelBytes = 96;
inline constexpr std::size_t kMaxDisplayLabelScalars = 64;
inline constexpr std::size_t kMaxScopes = 32;
inline constexpr std::size_t kMaxCandidates = 256;
inline constexpr std::size_t kMaxCandidatesPerScope = 64;
inline constexpr std::size_t kMaxEntries = 512;
inline constexpr std::size_t kMaxEntriesPerScope = 128;
inline constexpr std::size_t kMaxSourceBuckets = 1'024;
inline constexpr std::size_t kMaxSourceBucketsPerScope = 128;
inline constexpr std::uint64_t kTombstoneLifetimeMs = 60'000;
inline constexpr std::uint64_t kSourceBucketIdleMs = 60'000;
inline constexpr std::uint16_t kAdvertisedTtlSeconds = 15;
inline constexpr std::uint64_t kPublisherIntervalMs = 5'000;
inline constexpr std::uint64_t kPublisherJitterMs = 500;
inline constexpr std::uint64_t kPublisherRotationMs = 15 * 60 * 1'000;
inline constexpr std::uint64_t kImmediateUpdateIntervalMs = 1'000;

enum class AddressFamily : std::uint8_t {
  kIpv4 = 4,
  kIpv6 = 6,
};

struct IpAddress {
  AddressFamily family{AddressFamily::kIpv4};
  std::array<std::uint8_t, 16> bytes{};

  [[nodiscard]] static IpAddress V4(const std::array<std::uint8_t, 4>& value) noexcept;
  [[nodiscard]] static IpAddress V6(const std::array<std::uint8_t, 16>& value) noexcept;
  [[nodiscard]] std::span<const std::uint8_t> encoded() const noexcept;

  friend bool operator==(const IpAddress& left, const IpAddress& right) noexcept;
  friend bool operator<(const IpAddress& left, const IpAddress& right) noexcept;
};

struct InterfaceScope {
  std::uint64_t generation{};
  AddressFamily family{AddressFamily::kIpv4};

  friend bool operator==(const InterfaceScope&, const InterfaceScope&) = default;
  friend bool operator<(const InterfaceScope& left,
                        const InterfaceScope& right) noexcept;
};

struct NetworkInterface {
  InterfaceScope scope{};
  std::uint32_t system_index{};
  IpAddress local_address{};
  std::uint8_t prefix_length{};

  friend bool operator==(const NetworkInterface&, const NetworkInterface&) = default;
  friend bool operator<(const NetworkInterface& left,
                        const NetworkInterface& right) noexcept;
};

using InstanceToken = std::array<std::uint8_t, 16>;

struct DatagramMetadata {
  InterfaceScope observer{};
  IpAddress source{};
  IpAddress destination{};
  std::uint16_t destination_port{kDiscoveryPort};
  bool observer_eligible{true};
  bool source_is_broadcast{false};
  bool truncated{false};
};

class DisplayLabelValidator {
 public:
  virtual ~DisplayLabelValidator() = default;

  [[nodiscard]] virtual bool IsCanonical(
      std::span<const std::uint8_t> encoded) const noexcept = 0;
};

[[nodiscard]] std::shared_ptr<const DisplayLabelValidator>
MakeUtf8procDisplayLabelValidator();

enum class MessageType : std::uint8_t {
  kAnnounce = 1,
  kWithdraw = 2,
};

struct Advertisement {
  MessageType type{MessageType::kAnnounce};
  std::uint64_t sequence{};
  InstanceToken token{};
  std::uint16_t service_port{};
  std::uint16_t ttl_seconds{};
  std::array<std::uint8_t, kMaxDisplayLabelBytes> label{};
  std::uint8_t label_size{};
  std::array<std::uint8_t, kMaxDatagramSize> raw{};
  std::uint16_t raw_size{};

  [[nodiscard]] std::span<const std::uint8_t> display_label() const noexcept;
  [[nodiscard]] std::span<const std::uint8_t> raw_datagram() const noexcept;
};

struct EncodedDatagram {
  std::array<std::uint8_t, kMaxDatagramSize> bytes{};
  std::uint16_t size{};

  [[nodiscard]] std::span<const std::uint8_t> payload() const noexcept;
};

enum class ParseError {
  kNone,
  kTooShort,
  kTooLarge,
  kBadMagic,
  kUnsupportedVersion,
  kLengthMismatch,
  kBadHeaderLength,
  kNonzeroFlags,
  kNonzeroReserved,
  kUnknownMessage,
  kZeroSequence,
  kZeroToken,
  kInvalidAnnounce,
  kInvalidWithdraw,
  kTruncatedTlv,
  kReservedTlv,
  kTlvOrder,
  kDuplicateTlv,
  kTooManyTlvs,
  kUnknownCriticalTlv,
  kInvalidLabel,
};

struct ParseResult {
  Advertisement advertisement{};
  ParseError error{ParseError::kNone};

  [[nodiscard]] bool ok() const noexcept { return error == ParseError::kNone; }
};

[[nodiscard]] ParseResult ParseAdvertisement(
    std::span<const std::uint8_t> payload,
    const DisplayLabelValidator& label_validator) noexcept;
[[nodiscard]] bool EncodeAdvertisement(MessageType type, std::uint64_t sequence,
                                       const InstanceToken& token,
                                       std::uint16_t service_port,
                                       std::uint16_t ttl_seconds,
                                       std::span<const std::uint8_t> display_label,
                                       const DisplayLabelValidator& label_validator,
                                       EncodedDatagram& output) noexcept;

struct CandidateKey {
  InterfaceScope observer{};
  IpAddress source{};
  InstanceToken token{};

  friend bool operator==(const CandidateKey&, const CandidateKey&) = default;
  friend bool operator<(const CandidateKey& left, const CandidateKey& right) noexcept;
};

struct Candidate {
  CandidateKey key{};
  std::uint16_t service_port{};
  std::string display_label{};
  std::uint64_t deadline_ms{};
  std::uint64_t highest_sequence{};
};

enum class ExpiryReason {
  kTtl,
  kWithdrawn,
  kInterfaceRemoved,
  kWake,
};

enum class EventKind {
  kAppeared,
  kUpdated,
  kExpired,
};

struct CandidateEvent {
  EventKind kind{EventKind::kAppeared};
  Candidate candidate{};
  ExpiryReason expiry_reason{ExpiryReason::kTtl};
};

enum class ReceiveDisposition {
  kAppeared,
  kUpdated,
  kRefreshed,
  kWithdrawn,
  kTombstoned,
  kDroppedStopped,
  kDroppedClockRegression,
  kDroppedMetadata,
  kDroppedRateLimit,
  kDroppedTruncated,
  kDroppedMalformed,
  kDroppedSelf,
  kDroppedStale,
  kDroppedDuplicate,
  kDroppedSequenceConflict,
  kDroppedCapacity,
  kDroppedTimeOverflow,
};

struct ReceiveResult {
  ReceiveDisposition disposition{ReceiveDisposition::kDroppedMalformed};
  ParseError parse_error{ParseError::kNone};
  std::vector<CandidateEvent> events{};
};

struct CacheStats {
  std::size_t candidates{};
  std::size_t tombstones{};
  std::size_t source_buckets{};
  std::size_t retained_payload_bytes{};
};

class DiscoveryCache final {
 public:
  explicit DiscoveryCache(std::shared_ptr<const DisplayLabelValidator> label_validator);
  ~DiscoveryCache();

  DiscoveryCache(const DiscoveryCache&) = delete;
  DiscoveryCache& operator=(const DiscoveryCache&) = delete;
  DiscoveryCache(DiscoveryCache&&) = delete;
  DiscoveryCache& operator=(DiscoveryCache&&) = delete;

  [[nodiscard]] bool Start(std::span<const InterfaceScope> scopes,
                           std::uint64_t now_ms);
  void Stop();

  [[nodiscard]] bool SetLocalToken(const InterfaceScope& scope,
                                   const InstanceToken& token);
  [[nodiscard]] ReceiveResult Receive(const DatagramMetadata& metadata,
                                      std::span<const std::uint8_t> payload,
                                      std::uint64_t now_ms);
  [[nodiscard]] std::vector<CandidateEvent> Advance(std::uint64_t now_ms);
  [[nodiscard]] std::vector<CandidateEvent> ApplyInterfaceSnapshot(
      std::span<const InterfaceScope> scopes, std::uint64_t now_ms);
  [[nodiscard]] std::vector<CandidateEvent> Wake(std::span<const InterfaceScope> scopes,
                                                 std::uint64_t now_ms);

  [[nodiscard]] std::vector<Candidate> Snapshot() const;
  [[nodiscard]] std::optional<std::uint64_t> NextDeadlineMs() const;
  [[nodiscard]] CacheStats stats() const;
  [[nodiscard]] bool running() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

class MonotonicClock {
 public:
  virtual ~MonotonicClock() = default;

  [[nodiscard]] virtual std::uint64_t NowMs() const noexcept = 0;
};

class EntropySource {
 public:
  virtual ~EntropySource() = default;

  [[nodiscard]] virtual bool Fill(std::span<std::uint8_t> output) noexcept = 0;
};

class DiscoveryTimer {
 public:
  using Handler = std::function<void()>;

  virtual ~DiscoveryTimer() = default;
  [[nodiscard]] virtual bool ScheduleAt(std::uint64_t deadline_ms, Handler handler) = 0;
  virtual void Cancel() = 0;
  virtual void Stop() = 0;
};

class DatagramTransport {
 public:
  using ReceiveHandler =
      std::function<void(const DatagramMetadata&, std::span<const std::uint8_t>)>;

  virtual ~DatagramTransport() = default;
  [[nodiscard]] virtual bool Start(std::span<const NetworkInterface> interfaces,
                                   ReceiveHandler receive_handler) = 0;
  [[nodiscard]] virtual bool Reconfigure(
      std::span<const NetworkInterface> interfaces) = 0;
  [[nodiscard]] virtual bool Send(const InterfaceScope& scope,
                                  std::span<const std::uint8_t> payload) = 0;
  virtual void Stop() = 0;
};

class InterfaceMonitor {
 public:
  using ChangeHandler = std::function<void()>;

  virtual ~InterfaceMonitor() = default;
  [[nodiscard]] virtual std::vector<NetworkInterface> Snapshot() = 0;
  [[nodiscard]] virtual bool Start(ChangeHandler change_handler) = 0;
  virtual void Stop() = 0;
};

struct DiscoveryConfig {
  std::uint16_t service_port{};
  std::string display_label{};
};

class DiscoveryService final {
 public:
  using EventHandler = std::function<void(const CandidateEvent&)>;

  DiscoveryService(DiscoveryConfig config,
                   std::shared_ptr<const DisplayLabelValidator> label_validator,
                   std::unique_ptr<MonotonicClock> clock,
                   std::unique_ptr<EntropySource> entropy,
                   std::unique_ptr<DiscoveryTimer> timer,
                   std::unique_ptr<DatagramTransport> transport,
                   std::unique_ptr<InterfaceMonitor> interface_monitor,
                   EventHandler event_handler);
  ~DiscoveryService();

  DiscoveryService(const DiscoveryService&) = delete;
  DiscoveryService& operator=(const DiscoveryService&) = delete;
  DiscoveryService(DiscoveryService&&) = delete;
  DiscoveryService& operator=(DiscoveryService&&) = delete;

  [[nodiscard]] bool Start();
  void Stop();
  [[nodiscard]] bool Wake();
  [[nodiscard]] bool UpdateAdvertisement(DiscoveryConfig config);

  [[nodiscard]] std::vector<Candidate> Snapshot() const;
  [[nodiscard]] bool running() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xnn_transfer::core::discovery

#endif  // XNN_TRANSFER_CORE_DISCOVERY_DISCOVERY_HPP_
