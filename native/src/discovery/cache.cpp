#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xnn_transfer/core/discovery/discovery.hpp"

namespace xnn_transfer::core::discovery {
namespace {

constexpr double kGlobalRate = 256.0;
constexpr double kGlobalBurst = 512.0;
constexpr double kScopeRate = 128.0;
constexpr double kScopeBurst = 256.0;
constexpr double kSourceRate = 8.0;
constexpr double kSourceBurst = 16.0;
constexpr std::size_t kMaximumRetainedPayloadBytes = 131'072;

constexpr std::array<std::uint8_t, 4> kIpv4Group{239, 255, 88, 78};
constexpr std::array<std::uint8_t, 16> kIpv6Group{0xff, 0x12, 0x00, 0x00, 0x00, 0x00,
                                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                  0x58, 0x4e, 0x4e, 0x44};

enum class CacheState {
  kCreated,
  kRunning,
  kStopped,
};

struct TokenBucket {
  double rate{};
  double burst{};
  double tokens{};
  std::uint64_t updated_ms{};

  [[nodiscard]] double Available(const std::uint64_t now_ms) const noexcept {
    const std::uint64_t elapsed_ms = now_ms - updated_ms;
    const double refill = static_cast<double>(elapsed_ms) * rate / 1'000.0;
    return std::min(burst, tokens + refill);
  }

  void Consume(const std::uint64_t now_ms) noexcept {
    tokens = Available(now_ms) - 1.0;
    updated_ms = now_ms;
  }
};

[[nodiscard]] TokenBucket FullBucket(const double rate, const double burst,
                                     const std::uint64_t now_ms) {
  return TokenBucket{
      .rate = rate, .burst = burst, .tokens = burst, .updated_ms = now_ms};
}

struct ScopeState {
  InterfaceScope scope{};
  TokenBucket bucket{};
};

struct LocalToken {
  InterfaceScope scope{};
  InstanceToken token{};
};

struct SourceBucket {
  InterfaceScope scope{};
  IpAddress source{};
  TokenBucket bucket{};
};

struct Entry {
  bool candidate{};
  Candidate value{};
  std::array<std::uint8_t, kMaxDatagramSize> raw{};
  std::uint16_t raw_size{};
  std::uint64_t retain_until_ms{};
};

[[nodiscard]] bool AddMilliseconds(const std::uint64_t now_ms,
                                   const std::uint64_t duration_ms,
                                   std::uint64_t& result) noexcept {
  if (duration_ms > std::numeric_limits<std::uint64_t>::max() - now_ms) {
    return false;
  }
  result = now_ms + duration_ms;
  return true;
}

[[nodiscard]] bool IsZero(const std::span<const std::uint8_t> bytes) noexcept {
  return std::all_of(bytes.begin(), bytes.end(),
                     [](const std::uint8_t value) { return value == 0; });
}

[[nodiscard]] bool IsValidSource(const DatagramMetadata& metadata) noexcept {
  if (metadata.source.family != metadata.observer.family ||
      metadata.destination.family != metadata.observer.family ||
      metadata.source_is_broadcast) {
    return false;
  }

  const std::span<const std::uint8_t> source = metadata.source.encoded();
  if (metadata.source.family == AddressFamily::kIpv4) {
    const std::uint8_t first = source[0];
    return first != 0 && first != 127 && first != 255 && (first < 224 || first > 239);
  }

  if (IsZero(source) || source[0] == 0xff) {
    return false;
  }
  const bool loopback =
      std::all_of(source.begin(), source.end() - 1,
                  [](const std::uint8_t value) { return value == 0; }) &&
      source.back() == 1;
  const bool ipv4_mapped =
      std::all_of(source.begin(), source.begin() + 10,
                  [](const std::uint8_t value) { return value == 0; }) &&
      source[10] == 0xff && source[11] == 0xff;
  return !loopback && !ipv4_mapped;
}

[[nodiscard]] bool IsDiscoveryDestination(const DatagramMetadata& metadata) noexcept {
  if (metadata.destination_port != kDiscoveryPort) {
    return false;
  }
  const std::span<const std::uint8_t> destination = metadata.destination.encoded();
  if (metadata.destination.family == AddressFamily::kIpv4) {
    return std::equal(destination.begin(), destination.end(), kIpv4Group.begin());
  }
  return std::equal(destination.begin(), destination.end(), kIpv6Group.begin());
}

[[nodiscard]] CandidateEvent MakeEvent(const EventKind kind, const Candidate& candidate,
                                       const ExpiryReason reason = ExpiryReason::kTtl) {
  return CandidateEvent{.kind = kind, .candidate = candidate, .expiry_reason = reason};
}

}  // namespace

class DiscoveryCache::Impl final {
 public:
  explicit Impl(std::shared_ptr<const DisplayLabelValidator> label_validator)
      : label_validator_(std::move(label_validator)) {
    scopes_.reserve(kMaxScopes);
    local_tokens_.reserve(kMaxScopes);
    source_buckets_.reserve(kMaxSourceBuckets);
    entries_.reserve(kMaxEntries);
  }

  [[nodiscard]] bool Start(const std::span<const InterfaceScope> scopes,
                           const std::uint64_t now_ms) {
    const std::scoped_lock lock(mutex_);
    if (state_ != CacheState::kCreated || label_validator_ == nullptr) {
      return false;
    }
    now_ms_ = now_ms;
    global_bucket_ = FullBucket(kGlobalRate, kGlobalBurst, now_ms);
    ReplaceScopes(scopes, now_ms);
    state_ = CacheState::kRunning;
    return true;
  }

  void Stop() {
    const std::scoped_lock lock(mutex_);
    state_ = CacheState::kStopped;
    scopes_.clear();
    local_tokens_.clear();
    source_buckets_.clear();
    entries_.clear();
    retained_payload_bytes_ = 0;
  }

  [[nodiscard]] bool SetLocalToken(const InterfaceScope& scope,
                                   const InstanceToken& token) {
    const std::scoped_lock lock(mutex_);
    if (state_ != CacheState::kRunning || IsZero(token) || !HasScope(scope)) {
      return false;
    }
    const auto existing =
        std::find_if(local_tokens_.begin(), local_tokens_.end(),
                     [&scope](const LocalToken& item) { return item.scope == scope; });
    if (existing != local_tokens_.end()) {
      existing->token = token;
      return true;
    }
    if (local_tokens_.size() == kMaxScopes) {
      return false;
    }
    local_tokens_.push_back(LocalToken{.scope = scope, .token = token});
    return true;
  }

  [[nodiscard]] ReceiveResult Receive(const DatagramMetadata& metadata,
                                      const std::span<const std::uint8_t> payload,
                                      const std::uint64_t now_ms) {
    const std::scoped_lock lock(mutex_);
    ReceiveResult result{};
    if (state_ != CacheState::kRunning) {
      result.disposition = ReceiveDisposition::kDroppedStopped;
      return result;
    }
    if (now_ms < now_ms_) {
      result.disposition = ReceiveDisposition::kDroppedClockRegression;
      return result;
    }
    now_ms_ = now_ms;
    EraseExpiredTombstones(now_ms);

    if (!metadata.observer_eligible || !HasScope(metadata.observer) ||
        !IsValidSource(metadata) || !IsDiscoveryDestination(metadata)) {
      result.disposition = ReceiveDisposition::kDroppedMetadata;
      return result;
    }
    if (!Admit(metadata.observer, metadata.source, now_ms)) {
      result.disposition = ReceiveDisposition::kDroppedRateLimit;
      return result;
    }
    if (metadata.truncated) {
      result.disposition = ReceiveDisposition::kDroppedTruncated;
      return result;
    }

    const ParseResult parsed = ParseAdvertisement(payload, *label_validator_);
    if (!parsed.ok()) {
      result.disposition = ReceiveDisposition::kDroppedMalformed;
      result.parse_error = parsed.error;
      return result;
    }
    const Advertisement& advertisement = parsed.advertisement;
    if (IsSelf(metadata.observer, advertisement.token)) {
      result.disposition = ReceiveDisposition::kDroppedSelf;
      return result;
    }

    const CandidateKey key{.observer = metadata.observer,
                           .source = metadata.source,
                           .token = advertisement.token};
    const auto iterator = FindEntry(key);
    if (advertisement.type == MessageType::kWithdraw) {
      return Withdraw(key, advertisement, iterator, now_ms, std::move(result.events));
    }
    return Announce(key, advertisement, iterator, now_ms, std::move(result.events));
  }

  [[nodiscard]] std::vector<CandidateEvent> Advance(const std::uint64_t now_ms) {
    const std::scoped_lock lock(mutex_);
    std::vector<CandidateEvent> events;
    if (state_ != CacheState::kRunning || now_ms < now_ms_) {
      return events;
    }
    now_ms_ = now_ms;
    Purge(now_ms, events);
    return events;
  }

  [[nodiscard]] std::vector<CandidateEvent> ApplyInterfaceSnapshot(
      const std::span<const InterfaceScope> scopes, const std::uint64_t now_ms) {
    const std::scoped_lock lock(mutex_);
    std::vector<CandidateEvent> events;
    if (state_ != CacheState::kRunning || now_ms < now_ms_) {
      return events;
    }
    now_ms_ = now_ms;
    EraseExpiredTombstones(now_ms);
    std::vector<InterfaceScope> replacement = CanonicalScopes(scopes);
    RemoveAbsentScopes(replacement, ExpiryReason::kInterfaceRemoved, events);
    InstallScopes(std::move(replacement), now_ms);
    return events;
  }

  [[nodiscard]] std::vector<CandidateEvent> Wake(
      const std::span<const InterfaceScope> scopes, const std::uint64_t now_ms) {
    const std::scoped_lock lock(mutex_);
    std::vector<CandidateEvent> events;
    if (state_ != CacheState::kRunning || now_ms < now_ms_) {
      return events;
    }
    now_ms_ = now_ms;

    EraseExpiredTombstones(now_ms);
    for (Entry& entry : entries_) {
      if (!entry.candidate) {
        continue;
      }
      events.push_back(
          MakeEvent(EventKind::kExpired, entry.value, ExpiryReason::kWake));
      retained_payload_bytes_ -= entry.raw_size;
      entry.candidate = false;
      entry.raw_size = 0;
      entry.value.service_port = 0;
      entry.value.display_label.clear();
      entry.value.deadline_ms = 0;
      if (!AddMilliseconds(now_ms, kTombstoneLifetimeMs, entry.retain_until_ms)) {
        entry.retain_until_ms = std::numeric_limits<std::uint64_t>::max();
      }
    }

    std::vector<InterfaceScope> replacement = CanonicalScopes(scopes);
    RemoveAbsentScopes(replacement, ExpiryReason::kInterfaceRemoved, events);
    local_tokens_.clear();
    InstallScopes(std::move(replacement), now_ms);
    return events;
  }

  [[nodiscard]] std::vector<Candidate> Snapshot() const {
    const std::scoped_lock lock(mutex_);
    std::vector<Candidate> candidates;
    candidates.reserve(CandidateCount());
    for (const Entry& entry : entries_) {
      if (entry.candidate) {
        candidates.push_back(entry.value);
      }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                return left.key < right.key;
              });
    return candidates;
  }

  [[nodiscard]] std::optional<std::uint64_t> NextDeadlineMs() const {
    const std::scoped_lock lock(mutex_);
    std::optional<std::uint64_t> deadline;
    if (state_ != CacheState::kRunning) {
      return deadline;
    }
    for (const Entry& entry : entries_) {
      if (entry.candidate &&
          (!deadline.has_value() || entry.value.deadline_ms < *deadline)) {
        deadline = entry.value.deadline_ms;
      }
    }
    return deadline;
  }

  [[nodiscard]] CacheStats stats() const {
    const std::scoped_lock lock(mutex_);
    const std::size_t candidates = CandidateCount();
    return CacheStats{.candidates = candidates,
                      .tombstones = entries_.size() - candidates,
                      .source_buckets = source_buckets_.size(),
                      .retained_payload_bytes = retained_payload_bytes_};
  }

  [[nodiscard]] bool running() const {
    const std::scoped_lock lock(mutex_);
    return state_ == CacheState::kRunning;
  }

 private:
  using EntryIterator = std::vector<Entry>::iterator;

  [[nodiscard]] std::vector<InterfaceScope> CanonicalScopes(
      const std::span<const InterfaceScope> scopes) const {
    std::vector<InterfaceScope> result;
    result.reserve(kMaxScopes);
    for (const InterfaceScope& scope : scopes) {
      const auto position = std::lower_bound(result.begin(), result.end(), scope);
      if (position != result.end() && *position == scope) {
        continue;
      }
      if (result.size() < kMaxScopes) {
        result.insert(position, scope);
        continue;
      }
      if (position != result.end()) {
        result.insert(position, scope);
        result.pop_back();
      }
    }
    return result;
  }

  void ReplaceScopes(const std::span<const InterfaceScope> scopes,
                     const std::uint64_t now_ms) {
    InstallScopes(CanonicalScopes(scopes), now_ms);
  }

  void InstallScopes(std::vector<InterfaceScope> scopes, const std::uint64_t now_ms) {
    std::vector<ScopeState> replacement;
    replacement.reserve(scopes.size());
    for (const InterfaceScope& scope : scopes) {
      const auto existing = std::find_if(
          scopes_.begin(), scopes_.end(),
          [&scope](const ScopeState& item) { return item.scope == scope; });
      replacement.push_back(
          existing == scopes_.end()
              ? ScopeState{.scope = scope,
                           .bucket = FullBucket(kScopeRate, kScopeBurst, now_ms)}
              : *existing);
    }
    scopes_ = std::move(replacement);
  }

  void RemoveAbsentScopes(const std::vector<InterfaceScope>& replacement,
                          const ExpiryReason reason,
                          std::vector<CandidateEvent>& events) {
    const auto present = [&replacement](const InterfaceScope& scope) {
      return std::binary_search(replacement.begin(), replacement.end(), scope);
    };
    for (auto iterator = entries_.begin(); iterator != entries_.end();) {
      if (present(iterator->value.key.observer)) {
        ++iterator;
        continue;
      }
      if (iterator->candidate) {
        events.push_back(MakeEvent(EventKind::kExpired, iterator->value, reason));
        retained_payload_bytes_ -= iterator->raw_size;
      }
      iterator = entries_.erase(iterator);
    }
    std::erase_if(local_tokens_,
                  [&present](const LocalToken& item) { return !present(item.scope); });
    std::erase_if(source_buckets_, [&present](const SourceBucket& item) {
      return !present(item.scope);
    });
  }

  [[nodiscard]] bool HasScope(const InterfaceScope& scope) const {
    return std::any_of(
        scopes_.begin(), scopes_.end(),
        [&scope](const ScopeState& item) { return item.scope == scope; });
  }

  [[nodiscard]] bool IsSelf(const InterfaceScope& scope,
                            const InstanceToken& token) const {
    return std::any_of(local_tokens_.begin(), local_tokens_.end(),
                       [&scope, &token](const LocalToken& item) {
                         return item.scope == scope && item.token == token;
                       });
  }

  [[nodiscard]] bool Admit(const InterfaceScope& scope, const IpAddress& source,
                           const std::uint64_t now_ms) {
    std::erase_if(source_buckets_, [now_ms](const SourceBucket& item) {
      return now_ms - item.bucket.updated_ms >= kSourceBucketIdleMs;
    });

    const auto scope_iterator =
        std::find_if(scopes_.begin(), scopes_.end(),
                     [&scope](const ScopeState& item) { return item.scope == scope; });
    if (scope_iterator == scopes_.end()) {
      return false;
    }

    auto source_iterator =
        std::find_if(source_buckets_.begin(), source_buckets_.end(),
                     [&scope, &source](const SourceBucket& item) {
                       return item.scope == scope && item.source == source;
                     });
    if (source_iterator == source_buckets_.end()) {
      const std::size_t scope_bucket_count = static_cast<std::size_t>(std::count_if(
          source_buckets_.begin(), source_buckets_.end(),
          [&scope](const SourceBucket& item) { return item.scope == scope; }));
      if (source_buckets_.size() >= kMaxSourceBuckets ||
          scope_bucket_count >= kMaxSourceBucketsPerScope) {
        return false;
      }
      source_buckets_.push_back(
          SourceBucket{.scope = scope,
                       .source = source,
                       .bucket = FullBucket(kSourceRate, kSourceBurst, now_ms)});
      source_iterator = source_buckets_.end() - 1;
    }

    if (global_bucket_.Available(now_ms) < 1.0 ||
        scope_iterator->bucket.Available(now_ms) < 1.0 ||
        source_iterator->bucket.Available(now_ms) < 1.0) {
      return false;
    }
    global_bucket_.Consume(now_ms);
    scope_iterator->bucket.Consume(now_ms);
    source_iterator->bucket.Consume(now_ms);
    return true;
  }

  [[nodiscard]] EntryIterator FindEntry(const CandidateKey& key) {
    return std::find_if(entries_.begin(), entries_.end(),
                        [&key](const Entry& entry) { return entry.value.key == key; });
  }

  [[nodiscard]] std::size_t CandidateCount() const {
    return static_cast<std::size_t>(
        std::count_if(entries_.begin(), entries_.end(),
                      [](const Entry& entry) { return entry.candidate; }));
  }

  [[nodiscard]] std::size_t CandidateCount(const InterfaceScope& scope) const {
    return static_cast<std::size_t>(
        std::count_if(entries_.begin(), entries_.end(), [&scope](const Entry& entry) {
          return entry.candidate && entry.value.key.observer == scope;
        }));
  }

  [[nodiscard]] std::size_t EntryCount(const InterfaceScope& scope) const {
    return static_cast<std::size_t>(std::count_if(
        entries_.begin(), entries_.end(),
        [&scope](const Entry& entry) { return entry.value.key.observer == scope; }));
  }

  [[nodiscard]] bool CandidateCapacity(const InterfaceScope& scope) const {
    return CandidateCount() < kMaxCandidates &&
           CandidateCount(scope) < kMaxCandidatesPerScope;
  }

  [[nodiscard]] bool EntryCapacity(const InterfaceScope& scope) const {
    return entries_.size() < kMaxEntries && EntryCount(scope) < kMaxEntriesPerScope;
  }

  [[nodiscard]] Candidate CandidateFrom(const CandidateKey& key,
                                        const Advertisement& advertisement,
                                        const std::uint64_t deadline_ms) const {
    return Candidate{
        .key = key,
        .service_port = advertisement.service_port,
        .display_label = std::string(
            reinterpret_cast<const char*>(advertisement.display_label().data()),
            advertisement.display_label().size()),
        .deadline_ms = deadline_ms,
        .highest_sequence = advertisement.sequence};
  }

  [[nodiscard]] ReceiveResult Announce(const CandidateKey& key,
                                       const Advertisement& advertisement,
                                       const EntryIterator iterator,
                                       const std::uint64_t now_ms,
                                       std::vector<CandidateEvent> events) {
    ReceiveResult result{.events = std::move(events)};
    std::uint64_t deadline_ms = 0;
    if (!AddMilliseconds(now_ms,
                         static_cast<std::uint64_t>(advertisement.ttl_seconds) * 1'000U,
                         deadline_ms)) {
      result.disposition = ReceiveDisposition::kDroppedTimeOverflow;
      return result;
    }

    if (iterator == entries_.end()) {
      if (!CandidateCapacity(key.observer) || !EntryCapacity(key.observer) ||
          retained_payload_bytes_ + advertisement.raw_size >
              kMaximumRetainedPayloadBytes) {
        result.disposition = ReceiveDisposition::kDroppedCapacity;
        return result;
      }
      Entry entry{.candidate = true,
                  .value = CandidateFrom(key, advertisement, deadline_ms),
                  .raw_size = advertisement.raw_size};
      std::copy(advertisement.raw.begin(), advertisement.raw.end(), entry.raw.begin());
      retained_payload_bytes_ += entry.raw_size;
      entries_.push_back(std::move(entry));
      result.events.push_back(MakeEvent(EventKind::kAppeared, entries_.back().value));
      result.disposition = ReceiveDisposition::kAppeared;
      return result;
    }

    if (!iterator->candidate &&
        advertisement.sequence <= iterator->value.highest_sequence) {
      result.disposition = ReceiveDisposition::kDroppedStale;
      return result;
    }
    if (advertisement.sequence < iterator->value.highest_sequence) {
      result.disposition = ReceiveDisposition::kDroppedStale;
      return result;
    }
    if (advertisement.sequence == iterator->value.highest_sequence) {
      const bool duplicate =
          iterator->candidate && iterator->raw_size == advertisement.raw_size &&
          std::equal(advertisement.raw.begin(),
                     advertisement.raw.begin() + advertisement.raw_size,
                     iterator->raw.begin());
      result.disposition = duplicate ? ReceiveDisposition::kDroppedDuplicate
                                     : ReceiveDisposition::kDroppedSequenceConflict;
      return result;
    }
    if (!iterator->candidate && !CandidateCapacity(key.observer)) {
      result.disposition = ReceiveDisposition::kDroppedCapacity;
      return result;
    }

    const Candidate replacement = CandidateFrom(key, advertisement, deadline_ms);
    const bool was_tombstone = !iterator->candidate;
    const bool visible_changed =
        was_tombstone || iterator->value.service_port != replacement.service_port ||
        iterator->value.display_label != replacement.display_label;
    if (retained_payload_bytes_ - iterator->raw_size + advertisement.raw_size >
        kMaximumRetainedPayloadBytes) {
      result.disposition = ReceiveDisposition::kDroppedCapacity;
      return result;
    }
    retained_payload_bytes_ -= iterator->raw_size;
    iterator->candidate = true;
    iterator->value = replacement;
    iterator->raw_size = advertisement.raw_size;
    std::copy(advertisement.raw.begin(), advertisement.raw.end(),
              iterator->raw.begin());
    iterator->retain_until_ms = 0;
    retained_payload_bytes_ += iterator->raw_size;

    if (was_tombstone) {
      result.events.push_back(MakeEvent(EventKind::kAppeared, iterator->value));
      result.disposition = ReceiveDisposition::kAppeared;
    } else if (visible_changed) {
      result.events.push_back(MakeEvent(EventKind::kUpdated, iterator->value));
      result.disposition = ReceiveDisposition::kUpdated;
    } else {
      result.disposition = ReceiveDisposition::kRefreshed;
    }
    return result;
  }

  [[nodiscard]] ReceiveResult Withdraw(const CandidateKey& key,
                                       const Advertisement& advertisement,
                                       const EntryIterator iterator,
                                       const std::uint64_t now_ms,
                                       std::vector<CandidateEvent> events) {
    ReceiveResult result{.events = std::move(events)};
    std::uint64_t retain_until_ms = 0;
    if (!AddMilliseconds(now_ms, kTombstoneLifetimeMs, retain_until_ms)) {
      result.disposition = ReceiveDisposition::kDroppedTimeOverflow;
      return result;
    }

    if (iterator == entries_.end()) {
      if (!EntryCapacity(key.observer)) {
        result.disposition = ReceiveDisposition::kDroppedCapacity;
        return result;
      }
      entries_.push_back(Entry{
          .candidate = false,
          .value = Candidate{.key = key, .highest_sequence = advertisement.sequence},
          .retain_until_ms = retain_until_ms});
      result.disposition = ReceiveDisposition::kTombstoned;
      return result;
    }
    if (advertisement.sequence <= iterator->value.highest_sequence) {
      result.disposition = ReceiveDisposition::kDroppedStale;
      return result;
    }
    if (iterator->candidate) {
      result.events.push_back(
          MakeEvent(EventKind::kExpired, iterator->value, ExpiryReason::kWithdrawn));
      retained_payload_bytes_ -= iterator->raw_size;
    }
    iterator->candidate = false;
    iterator->value.service_port = 0;
    iterator->value.display_label.clear();
    iterator->value.deadline_ms = 0;
    iterator->value.highest_sequence = advertisement.sequence;
    iterator->raw_size = 0;
    iterator->retain_until_ms = retain_until_ms;
    result.disposition = ReceiveDisposition::kWithdrawn;
    return result;
  }

  void Purge(const std::uint64_t now_ms, std::vector<CandidateEvent>& events) {
    for (Entry& entry : entries_) {
      if (!entry.candidate || now_ms < entry.value.deadline_ms) {
        continue;
      }
      events.push_back(MakeEvent(EventKind::kExpired, entry.value, ExpiryReason::kTtl));
      retained_payload_bytes_ -= entry.raw_size;
      entry.candidate = false;
      entry.value.service_port = 0;
      entry.value.display_label.clear();
      entry.value.deadline_ms = 0;
      entry.raw_size = 0;
      if (!AddMilliseconds(now_ms, kTombstoneLifetimeMs, entry.retain_until_ms)) {
        entry.retain_until_ms = std::numeric_limits<std::uint64_t>::max();
      }
    }
    EraseExpiredTombstones(now_ms);
  }

  void EraseExpiredTombstones(const std::uint64_t now_ms) {
    std::erase_if(entries_, [now_ms](const Entry& entry) {
      return !entry.candidate && now_ms >= entry.retain_until_ms;
    });
  }

  mutable std::mutex mutex_;
  std::shared_ptr<const DisplayLabelValidator> label_validator_;
  CacheState state_{CacheState::kCreated};
  std::uint64_t now_ms_{};
  TokenBucket global_bucket_{};
  std::vector<ScopeState> scopes_;
  std::vector<LocalToken> local_tokens_;
  std::vector<SourceBucket> source_buckets_;
  std::vector<Entry> entries_;
  std::size_t retained_payload_bytes_{};
};

DiscoveryCache::DiscoveryCache(
    std::shared_ptr<const DisplayLabelValidator> label_validator)
    : impl_(std::make_unique<Impl>(std::move(label_validator))) {}

DiscoveryCache::~DiscoveryCache() = default;

bool DiscoveryCache::Start(const std::span<const InterfaceScope> scopes,
                           const std::uint64_t now_ms) {
  return impl_->Start(scopes, now_ms);
}

void DiscoveryCache::Stop() { impl_->Stop(); }

bool DiscoveryCache::SetLocalToken(const InterfaceScope& scope,
                                   const InstanceToken& token) {
  return impl_->SetLocalToken(scope, token);
}

ReceiveResult DiscoveryCache::Receive(const DatagramMetadata& metadata,
                                      const std::span<const std::uint8_t> payload,
                                      const std::uint64_t now_ms) {
  return impl_->Receive(metadata, payload, now_ms);
}

std::vector<CandidateEvent> DiscoveryCache::Advance(const std::uint64_t now_ms) {
  return impl_->Advance(now_ms);
}

std::vector<CandidateEvent> DiscoveryCache::ApplyInterfaceSnapshot(
    const std::span<const InterfaceScope> scopes, const std::uint64_t now_ms) {
  return impl_->ApplyInterfaceSnapshot(scopes, now_ms);
}

std::vector<CandidateEvent> DiscoveryCache::Wake(
    const std::span<const InterfaceScope> scopes, const std::uint64_t now_ms) {
  return impl_->Wake(scopes, now_ms);
}

std::vector<Candidate> DiscoveryCache::Snapshot() const { return impl_->Snapshot(); }

CacheStats DiscoveryCache::stats() const { return impl_->stats(); }

std::optional<std::uint64_t> DiscoveryCache::NextDeadlineMs() const {
  return impl_->NextDeadlineMs();
}

bool DiscoveryCache::running() const { return impl_->running(); }

}  // namespace xnn_transfer::core::discovery
