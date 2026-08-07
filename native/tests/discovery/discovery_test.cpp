#include "xnn_transfer/core/discovery/discovery.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using xnn_transfer::core::discovery::AddressFamily;
using xnn_transfer::core::discovery::CacheStats;
using xnn_transfer::core::discovery::CandidateEvent;
using xnn_transfer::core::discovery::DatagramMetadata;
using xnn_transfer::core::discovery::DiscoveryCache;
using xnn_transfer::core::discovery::DisplayLabelValidator;
using xnn_transfer::core::discovery::EventKind;
using xnn_transfer::core::discovery::ExpiryReason;
using xnn_transfer::core::discovery::InstanceToken;
using xnn_transfer::core::discovery::InterfaceScope;
using xnn_transfer::core::discovery::IpAddress;
using xnn_transfer::core::discovery::kDiscoveryHeaderSize;
using xnn_transfer::core::discovery::kDiscoveryPort;
using xnn_transfer::core::discovery::kMaxCandidatesPerScope;
using xnn_transfer::core::discovery::kMaxDatagramSize;
using xnn_transfer::core::discovery::kMaxSourceBucketsPerScope;
using xnn_transfer::core::discovery::MessageType;
using xnn_transfer::core::discovery::ParseAdvertisement;
using xnn_transfer::core::discovery::ParseError;
using xnn_transfer::core::discovery::ReceiveDisposition;

using Bytes = std::vector<std::uint8_t>;

int failures = 0;

void Expect(const bool condition, const std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

class TestLabelValidator final : public DisplayLabelValidator {
 public:
  bool IsCanonical(
      const std::span<const std::uint8_t> encoded) const noexcept override {
    if (encoded.empty() || encoded.size() > 64 || encoded.front() == ' ' ||
        encoded.back() == ' ') {
      return false;
    }
    for (const std::uint8_t value : encoded) {
      if (value < 0x20U || value > 0x7eU) {
        return false;
      }
    }
    return true;
  }
};

void AppendU16(Bytes& output, const std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value));
}

void AppendU64(Bytes& output, const std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    output.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
  }
}

void SetU16(Bytes& output, const std::size_t offset, const std::uint16_t value) {
  output[offset] = static_cast<std::uint8_t>(value >> 8U);
  output[offset] = static_cast<std::uint8_t>(value >> 8U);
  output[offset + 1] = static_cast<std::uint8_t>(value);
}

Bytes DecodeHex(const std::string_view encoded) {
  auto nibble = [](const char value) -> std::uint8_t {
    if (value >= '0' && value <= '9') {
      return static_cast<std::uint8_t>(value - '0');
    }
    return static_cast<std::uint8_t>(value - 'a' + 10);
  };
  Bytes result;
  result.reserve(encoded.size() / 2);
  for (std::size_t offset = 0; offset < encoded.size(); offset += 2) {
    result.push_back(static_cast<std::uint8_t>((nibble(encoded[offset]) << 4U) |
                                               nibble(encoded[offset + 1])));
  }
  return result;
}

InstanceToken Token(const std::uint8_t suffix = 0xff) {
  InstanceToken token{};
  for (std::size_t index = 0; index < token.size(); ++index) {
    token[index] = static_cast<std::uint8_t>(index);
  }
  token.back() = suffix;
  return token;
}

Bytes MakeDatagram(const MessageType type = MessageType::kAnnounce,
                   const std::uint64_t sequence = 1,
                   const InstanceToken token = Token(),
                   const std::uint16_t service_port = 45'879,
                   const std::uint16_t ttl_seconds = 15,
                   const std::string_view label = {}) {
  Bytes tlvs;
  if (!label.empty()) {
    AppendU16(tlvs, 1);
    AppendU16(tlvs, static_cast<std::uint16_t>(label.size()));
    tlvs.insert(tlvs.end(), label.begin(), label.end());
  }

  Bytes output;
  output.reserve(kDiscoveryHeaderSize + tlvs.size());
  output.insert(output.end(), {'X', 'N', 'N', 'D'});
  output.push_back(1);
  output.push_back(0);
  output.push_back(static_cast<std::uint8_t>(type));
  output.push_back(0);
  AppendU16(output, static_cast<std::uint16_t>(kDiscoveryHeaderSize + tlvs.size()));
  AppendU16(output, static_cast<std::uint16_t>(kDiscoveryHeaderSize));
  AppendU64(output, sequence);
  output.insert(output.end(), token.begin(), token.end());
  AppendU16(output, service_port);
  AppendU16(output, ttl_seconds);
  AppendU16(output, static_cast<std::uint16_t>(tlvs.size()));
  AppendU16(output, 0);
  output.insert(output.end(), tlvs.begin(), tlvs.end());
  return output;
}

Bytes Withdraw(const std::uint64_t sequence, const InstanceToken token = Token()) {
  return MakeDatagram(MessageType::kWithdraw, sequence, token, 0, 0);
}

IpAddress V4(const std::uint8_t last) { return IpAddress::V4({192, 0, 2, last}); }

DatagramMetadata Metadata(const InterfaceScope scope,
                          const std::uint8_t source_last = 10) {
  return DatagramMetadata{.observer = scope,
                          .source = V4(source_last),
                          .destination = IpAddress::V4({239, 255, 88, 78}),
                          .destination_port = kDiscoveryPort};
}

std::shared_ptr<const DisplayLabelValidator> Validator() {
  return std::make_shared<TestLabelValidator>();
}

void ExpectStats(const CacheStats stats, const std::size_t candidates,
                 const std::size_t tombstones, const std::string_view context) {
  Expect(stats.candidates == candidates, std::string(context) + " candidate count");
  Expect(stats.tombstones == tombstones, std::string(context) + " tombstone count");
}

void TestWireParser() {
  const TestLabelValidator validator;
  const Bytes valid = MakeDatagram();
  const Bytes expected = DecodeHex(
      "584e4e4401000100002c002c0000000000000001000102030405060708090a0b0c0d"
      "0effb337000f00000000");
  Expect(valid == expected, "minimum announcement encoding matches discovery v1 bytes");
  const auto parsed = ParseAdvertisement(valid, validator);
  Expect(parsed.ok(), "minimum announcement parses");
  Expect(parsed.advertisement.sequence == 1, "sequence decodes");
  Expect(parsed.advertisement.service_port == 45'879, "service port decodes");
  Expect(parsed.advertisement.raw_datagram().size() == valid.size(),
         "raw datagram is copied into bounded result");

  Bytes truncated = valid;
  truncated.pop_back();
  Expect(ParseAdvertisement(truncated, validator).error == ParseError::kTooShort,
         "short fixed header rejects");

  Bytes oversized(kMaxDatagramSize + 1, 0);
  Expect(ParseAdvertisement(oversized, validator).error == ParseError::kTooLarge,
         "oversized datagram rejects before header access");

  Bytes unknown_version = valid;
  unknown_version[5] = 1;
  Expect(ParseAdvertisement(unknown_version, validator).error ==
             ParseError::kUnsupportedVersion,
         "unknown minor rejects");

  Bytes wrong_length = valid;
  SetU16(wrong_length, 8, static_cast<std::uint16_t>(wrong_length.size() - 1));
  Expect(
      ParseAdvertisement(wrong_length, validator).error == ParseError::kLengthMismatch,
      "inconsistent total length rejects");

  Bytes duplicate_label =
      MakeDatagram(MessageType::kAnnounce, 1, Token(), 45'879, 15, "Desk");
  AppendU16(duplicate_label, 1);
  AppendU16(duplicate_label, 1);
  duplicate_label.push_back('X');
  SetU16(duplicate_label, 8, static_cast<std::uint16_t>(duplicate_label.size()));
  SetU16(duplicate_label, 40,
         static_cast<std::uint16_t>(duplicate_label.size() - kDiscoveryHeaderSize));
  Expect(
      ParseAdvertisement(duplicate_label, validator).error == ParseError::kDuplicateTlv,
      "duplicate known field rejects");

  Bytes critical = valid;
  AppendU16(critical, 0x8100);
  AppendU16(critical, 0);
  SetU16(critical, 8, static_cast<std::uint16_t>(critical.size()));
  SetU16(critical, 40, 4);
  Expect(
      ParseAdvertisement(critical, validator).error == ParseError::kUnknownCriticalTlv,
      "unknown critical field rejects");

  const Bytes invalid_label =
      MakeDatagram(MessageType::kAnnounce, 1, Token(), 45'879, 15, " Desk");
  Expect(
      ParseAdvertisement(invalid_label, validator).error == ParseError::kInvalidLabel,
      "label provider controls canonical acceptance");

  const Bytes withdrawal = Withdraw(5);
  const auto withdrawal_result = ParseAdvertisement(withdrawal, validator);
  Expect(withdrawal_result.ok(), "canonical withdrawal parses");
  Expect(withdrawal_result.advertisement.type == MessageType::kWithdraw,
         "withdrawal type decodes");

  IpAddress noncanonical_ipv4 = V4(1);
  noncanonical_ipv4.bytes[15] = 0xff;
  Expect(noncanonical_ipv4 == V4(1),
         "ipv4 equality ignores bytes outside the four-byte address");
}

void TestDuplicateExpiryAndTombstone() {
  const InterfaceScope scope{.generation = 1, .family = AddressFamily::kIpv4};
  DiscoveryCache cache(Validator());
  Expect(cache.Start(std::span(&scope, 1), 0), "cache starts");
  const DatagramMetadata metadata = Metadata(scope);
  const Bytes sequence_one = MakeDatagram();

  auto result = cache.Receive(metadata, sequence_one, 0);
  Expect(result.disposition == ReceiveDisposition::kAppeared,
         "first announcement appears");
  Expect(result.events.size() == 1 && result.events[0].kind == EventKind::kAppeared,
         "appeared event publishes");
  Expect(cache.Snapshot()[0].deadline_ms == 15'000,
         "deadline uses receive monotonic time");

  result = cache.Receive(metadata, sequence_one, 5'000);
  Expect(result.disposition == ReceiveDisposition::kDroppedDuplicate,
         "exact duplicate drops");
  Expect(cache.Snapshot()[0].deadline_ms == 15'000,
         "duplicate does not refresh deadline");

  Expect(cache.Advance(14'999).empty(), "candidate is live before deadline");
  const auto expired = cache.Advance(15'000);
  Expect(expired.size() == 1 && expired[0].expiry_reason == ExpiryReason::kTtl,
         "candidate expires exactly at deadline");
  ExpectStats(cache.stats(), 0, 1, "after ttl expiry");

  result = cache.Receive(metadata, sequence_one, 16'000);
  Expect(result.disposition == ReceiveDisposition::kDroppedStale,
         "expired sequence cannot recreate candidate");

  result = cache.Receive(metadata, MakeDatagram(MessageType::kAnnounce, 2), 17'000);
  Expect(result.disposition == ReceiveDisposition::kAppeared,
         "newer sequence replaces tombstone");
  Expect(cache.Snapshot()[0].deadline_ms == 32'000,
         "new sequence receives a new lease");
}

void TestMalformedInputDoesNotAdvanceExpiry() {
  const InterfaceScope scope{.generation = 10, .family = AddressFamily::kIpv4};
  DiscoveryCache cache(Validator());
  Expect(cache.Start(std::span(&scope, 1), 0), "malformed-input cache starts");
  const DatagramMetadata metadata = Metadata(scope);

  Expect(cache.Receive(metadata, MakeDatagram(), 0).disposition ==
             ReceiveDisposition::kAppeared,
         "candidate exists before malformed input");
  Bytes bad_magic = MakeDatagram(MessageType::kAnnounce, 2);
  bad_magic[0] = 'B';
  const auto malformed = cache.Receive(metadata, bad_magic, 15'000);

  Expect(malformed.disposition == ReceiveDisposition::kDroppedMalformed,
         "bad magic remains a malformed drop");
  Expect(malformed.events.empty(), "malformed input does not publish candidate events");
  Expect(cache.Snapshot().size() == 1, "malformed input does not expire a candidate");

  const auto expired = cache.Advance(15'000);
  Expect(expired.size() == 1 && expired[0].expiry_reason == ExpiryReason::kTtl,
         "timer step owns ttl expiry after malformed input");
}

void TestUpdateConflictAndWithdrawal() {
  const InterfaceScope scope{.generation = 2, .family = AddressFamily::kIpv4};
  DiscoveryCache cache(Validator());
  Expect(cache.Start(std::span(&scope, 1), 0), "update cache starts");
  const DatagramMetadata metadata = Metadata(scope);

  Expect(cache.Receive(metadata, MakeDatagram(), 0).disposition ==
             ReceiveDisposition::kAppeared,
         "base candidate appears");
  Expect(cache.Receive(metadata, MakeDatagram(MessageType::kAnnounce, 2), 4'000)
                 .disposition == ReceiveDisposition::kRefreshed,
         "newer equal metadata refreshes");
  Expect(cache.Snapshot()[0].deadline_ms == 19'000,
         "newer sequence refreshes deadline");

  const Bytes conflicting =
      MakeDatagram(MessageType::kAnnounce, 2, Token(), 45'880, 15, "Desk");
  Expect(cache.Receive(metadata, conflicting, 5'000).disposition ==
             ReceiveDisposition::kDroppedSequenceConflict,
         "equal sequence with different bytes conflicts");
  Expect(cache.Snapshot()[0].deadline_ms == 19'000, "conflict does not refresh");

  const Bytes updated =
      MakeDatagram(MessageType::kAnnounce, 3, Token(), 45'880, 15, "Desk");
  const auto update = cache.Receive(metadata, updated, 6'000);
  Expect(update.disposition == ReceiveDisposition::kUpdated,
         "newer visible metadata updates");
  Expect(update.events.size() == 1 && update.events[0].kind == EventKind::kUpdated,
         "updated event publishes");

  const auto withdrawal = cache.Receive(metadata, Withdraw(4), 7'000);
  Expect(withdrawal.disposition == ReceiveDisposition::kWithdrawn,
         "newer withdrawal removes candidate");
  Expect(withdrawal.events.size() == 1 &&
             withdrawal.events[0].expiry_reason == ExpiryReason::kWithdrawn,
         "withdraw reason publishes");
  ExpectStats(cache.stats(), 0, 1, "after withdrawal");

  Expect(cache.Receive(metadata, updated, 8'000).disposition ==
             ReceiveDisposition::kDroppedStale,
         "older announcement stays behind withdrawal");
  Expect(cache.Receive(metadata, MakeDatagram(MessageType::kAnnounce, 5), 9'000)
                 .disposition == ReceiveDisposition::kAppeared,
         "newer announcement may replace withdrawal tombstone");
}

void TestMetadataSelfAndRateLimits() {
  const InterfaceScope scope{.generation = 3, .family = AddressFamily::kIpv4};
  DiscoveryCache cache(Validator());
  Expect(cache.Start(std::span(&scope, 1), 0), "metadata cache starts");
  const Bytes datagram = MakeDatagram();

  DatagramMetadata wrong_destination = Metadata(scope);
  wrong_destination.destination = IpAddress::V4({239, 255, 88, 79});
  Expect(cache.Receive(wrong_destination, datagram, 0).disposition ==
             ReceiveDisposition::kDroppedMetadata,
         "wrong multicast group rejects");
  Expect(cache.stats().source_buckets == 0,
         "metadata rejection occurs before source bucket allocation");

  DatagramMetadata truncated = Metadata(scope);
  truncated.truncated = true;
  Expect(cache.Receive(truncated, datagram, 0).disposition ==
             ReceiveDisposition::kDroppedTruncated,
         "os truncation indication rejects");
  Expect(cache.stats().source_buckets == 1,
         "truncated valid metadata consumes source budget");

  Expect(cache.SetLocalToken(scope, Token()), "local token installs");
  Expect(cache.Receive(Metadata(scope), datagram, 0).disposition ==
             ReceiveDisposition::kDroppedSelf,
         "interface and family scoped local token filters self");

  const InterfaceScope other_scope{.generation = 4, .family = AddressFamily::kIpv4};
  const std::array scopes{scope, other_scope};
  Expect(cache.ApplyInterfaceSnapshot(scopes, 0).empty(),
         "adding a scope does not expire candidates");
  Expect(cache.Receive(Metadata(other_scope), datagram, 0).disposition ==
             ReceiveDisposition::kAppeared,
         "same token on another observer scope is not self");

  DiscoveryCache rate_cache(Validator());
  Expect(rate_cache.Start(std::span(&scope, 1), 0), "rate cache starts");
  std::size_t rate_limited = 0;
  for (std::size_t index = 0; index < 17; ++index) {
    const auto result = rate_cache.Receive(Metadata(scope), datagram, 0);
    if (result.disposition == ReceiveDisposition::kDroppedRateLimit) {
      ++rate_limited;
    }
  }
  Expect(rate_limited == 1, "per-source burst permits exactly sixteen");
}

void TestInterfaceWakeAndStop() {
  const InterfaceScope first{.generation = 5, .family = AddressFamily::kIpv4};
  const InterfaceScope second{.generation = 6, .family = AddressFamily::kIpv4};
  const std::array scopes{first, second};
  DiscoveryCache cache(Validator());
  Expect(cache.Start(scopes, 0), "interface cache starts");

  Expect(cache.Receive(Metadata(first, 10), MakeDatagram(), 0).disposition ==
             ReceiveDisposition::kAppeared,
         "first interface candidate appears");
  Expect(cache.Receive(Metadata(second, 11), MakeDatagram(), 0).disposition ==
             ReceiveDisposition::kAppeared,
         "second interface candidate appears");

  const auto removed = cache.ApplyInterfaceSnapshot(std::span(&second, 1), 1'000);
  Expect(removed.size() == 1 &&
             removed[0].expiry_reason == ExpiryReason::kInterfaceRemoved,
         "interface removal immediately expires its candidate");
  ExpectStats(cache.stats(), 1, 0, "after interface removal");

  const auto wake = cache.Wake(std::span(&second, 1), 2'000);
  Expect(wake.size() == 1 && wake[0].expiry_reason == ExpiryReason::kWake,
         "wake expires visible candidates independent of ttl");
  ExpectStats(cache.stats(), 0, 1, "after wake");
  Expect(cache.Receive(Metadata(second, 11), MakeDatagram(), 3'000).disposition ==
             ReceiveDisposition::kDroppedStale,
         "wake tombstone suppresses old sequence");

  cache.Stop();
  Expect(!cache.running(), "stop closes cache");
  ExpectStats(cache.stats(), 0, 0, "stop clears volatile state");
  Expect(cache.Receive(Metadata(second), MakeDatagram(), 4'000).disposition ==
             ReceiveDisposition::kDroppedStopped,
         "receive after stop cannot mutate state");
  Expect(cache.Advance(100'000).empty(), "timer after stop cannot publish events");
}

void TestCapacityBounds() {
  const InterfaceScope scope{.generation = 7, .family = AddressFamily::kIpv4};
  DiscoveryCache cache(Validator());
  Expect(cache.Start(std::span(&scope, 1), 0), "capacity cache starts");

  for (std::size_t index = 0; index < kMaxCandidatesPerScope; ++index) {
    const auto result =
        cache.Receive(Metadata(scope, static_cast<std::uint8_t>(index + 1)),
                      MakeDatagram(MessageType::kAnnounce, 1,
                                   Token(static_cast<std::uint8_t>(index + 1))),
                      0);
    Expect(result.disposition == ReceiveDisposition::kAppeared,
           "candidate below per-scope limit appears");
  }
  ExpectStats(cache.stats(), kMaxCandidatesPerScope, 0, "candidate capacity filled");
  Expect(cache.Receive(Metadata(scope, 100),
                       MakeDatagram(MessageType::kAnnounce, 1, Token(100)), 0)
                 .disposition == ReceiveDisposition::kDroppedCapacity,
         "candidate above per-scope limit drops without eviction");

  DiscoveryCache bucket_cache(Validator());
  Expect(bucket_cache.Start(std::span(&scope, 1), 0), "bucket capacity cache starts");
  std::size_t malformed = 0;
  std::size_t limited = 0;
  Bytes bad_magic = MakeDatagram();
  bad_magic[0] = 'B';
  for (std::size_t index = 0; index < kMaxSourceBucketsPerScope + 1; ++index) {
    std::array<std::uint8_t, 16> source{};
    source[0] = 0x20;
    source[1] = 0x01;
    source[2] = 0x0d;
    source[3] = 0xb8;
    source[14] = static_cast<std::uint8_t>(index >> 8U);
    source[15] = static_cast<std::uint8_t>(index);
    const InterfaceScope ipv6_scope{.generation = 8, .family = AddressFamily::kIpv6};
    if (index == 0) {
      Expect(bucket_cache.ApplyInterfaceSnapshot(std::span(&ipv6_scope, 1), 0).empty(),
             "ipv6 scope replacement has no visible candidate to expire");
    }
    DatagramMetadata metadata{
        .observer = ipv6_scope,
        .source = IpAddress::V6(source),
        .destination = IpAddress::V6({0xff, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0x58, 0x4e, 0x4e, 0x44})};
    const auto disposition = bucket_cache.Receive(metadata, bad_magic, 0).disposition;
    malformed += disposition == ReceiveDisposition::kDroppedMalformed;
    limited += disposition == ReceiveDisposition::kDroppedRateLimit;
  }
  Expect(malformed == kMaxSourceBucketsPerScope,
         "bounded source table parses existing capacity");
  Expect(limited == 1, "new source drops when bucket table is full");
}

void TestConcurrentStopBarrier() {
  const InterfaceScope scope{.generation = 9, .family = AddressFamily::kIpv4};
  DiscoveryCache cache(Validator());
  Expect(cache.Start(std::span(&scope, 1), 0), "concurrent cache starts");

  std::atomic<bool> start{false};
  std::array<std::thread, 8> workers;
  for (std::size_t index = 0; index < workers.size(); ++index) {
    workers[index] = std::thread([&, index] {
      while (!start.load(std::memory_order_acquire)) {
      }
      for (std::uint64_t sequence = 1; sequence <= 200; ++sequence) {
        const auto result =
            cache.Receive(Metadata(scope, static_cast<std::uint8_t>(index + 20)),
                          MakeDatagram(MessageType::kAnnounce, sequence,
                                       Token(static_cast<std::uint8_t>(index + 20))),
                          sequence);
        (void)result;
      }
    });
  }
  start.store(true, std::memory_order_release);
  cache.Stop();
  for (std::thread& worker : workers) {
    worker.join();
  }
  Expect(!cache.running(), "concurrent stop remains terminal");
  ExpectStats(cache.stats(), 0, 0,
              "no concurrent receive mutates state after stop barrier");
}

}  // namespace

int main() {
  TestWireParser();
  TestDuplicateExpiryAndTombstone();
  TestMalformedInputDoesNotAdvanceExpiry();
  TestUpdateConflictAndWithdrawal();
  TestMetadataSelfAndRateLimits();
  TestInterfaceWakeAndStop();
  TestCapacityBounds();
  TestConcurrentStopBarrier();

  if (failures != 0) {
    std::cerr << failures << " discovery assertions failed\n";
    return 1;
  }
  std::cout << "Native discovery tests passed.\n";
  return 0;
}
