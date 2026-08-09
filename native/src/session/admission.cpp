#include <algorithm>
#include <limits>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "xnn_transfer/core/session/session.hpp"

namespace xnn_transfer::core::session {
namespace {

constexpr std::size_t kMaxTrackedSourceBuckets = 64;

[[nodiscard]] bool AllZero(const std::span<const std::uint8_t> bytes) noexcept {
  std::uint8_t accumulator = 0;
  for (const std::uint8_t byte : bytes) {
    accumulator = static_cast<std::uint8_t>(accumulator | byte);
  }
  return accumulator == 0;
}

[[nodiscard]] bool IsRole(const security::tls::Role role) noexcept {
  return role == security::tls::Role::kInitiator ||
         role == security::tls::Role::kResponder;
}

[[nodiscard]] std::uint64_t CheckedDeadline(const std::uint64_t base,
                                            const std::uint64_t duration) noexcept {
  return base > std::numeric_limits<std::uint64_t>::max() - duration
             ? std::numeric_limits<std::uint64_t>::max()
             : base + duration;
}

}  // namespace

namespace detail {

struct PairingAdmissionState {
  mutable std::mutex mutex{};

  struct Bucket {
    std::size_t tokens{};
    std::uint64_t last_refill_ms{};
  };

  struct SourceBucket {
    SourceToken source{};
    Bucket bucket{};
  };

  struct Entry {
    AttemptHandle connection_id{};
    std::uint64_t lease_generation{};
    SourceToken source{};
    PublicKey local_key{};
    PublicKey peer_key{};
    security::tls::Role local_role{security::tls::Role::kInitiator};
    bool user_initiated{};
    bool visible{};
    std::uint64_t admitted_at_ms{};
    std::uint64_t window_deadline_ms{};
  };

  static void Refill(Bucket& bucket, const std::uint64_t now_ms,
                     const std::uint64_t interval_ms,
                     const std::size_t capacity) noexcept {
    if (now_ms <= bucket.last_refill_ms || bucket.tokens == capacity) {
      if (now_ms > bucket.last_refill_ms && bucket.tokens == capacity) {
        bucket.last_refill_ms = now_ms;
      }
      return;
    }
    const std::uint64_t intervals = (now_ms - bucket.last_refill_ms) / interval_ms;
    if (intervals == 0) {
      return;
    }
    const std::size_t refill =
        intervals >= capacity ? capacity : static_cast<std::size_t>(intervals);
    bucket.tokens = std::min(capacity, bucket.tokens + refill);
    bucket.last_refill_ms =
        CheckedDeadline(bucket.last_refill_ms, intervals * interval_ms);
  }

  [[nodiscard]] std::size_t ActiveForSource(
      const SourceToken& source,
      const AttemptHandle* ignored = nullptr) const noexcept {
    return static_cast<std::size_t>(std::count_if(
        entries.begin(), entries.end(), [&source, ignored](const Entry& entry) {
          return entry.source == source &&
                 (ignored == nullptr || entry.connection_id != *ignored);
        }));
  }

  [[nodiscard]] bool SourceIsActive(const SourceToken& source) const noexcept {
    return ActiveForSource(source) != 0;
  }

  void PruneSourceBuckets(const std::uint64_t now_ms) {
    for (SourceBucket& source : source_buckets) {
      Refill(source.bucket, now_ms, kSourceAdmissionRefillMs,
             kSourceAdmissionBucketCapacity);
    }
    source_buckets.erase(std::remove_if(source_buckets.begin(), source_buckets.end(),
                                        [this](const SourceBucket& source) {
                                          return source.bucket.tokens ==
                                                     kSourceAdmissionBucketCapacity &&
                                                 !SourceIsActive(source.source);
                                        }),
                         source_buckets.end());
  }

  [[nodiscard]] SourceBucket* FindSource(const SourceToken& source) noexcept {
    const auto iterator = std::find_if(source_buckets.begin(), source_buckets.end(),
                                       [&source](const SourceBucket& candidate) {
                                         return candidate.source == source;
                                       });
    return iterator == source_buckets.end() ? nullptr : &*iterator;
  }

  [[nodiscard]] Entry* Find(const AttemptHandle& connection_id) noexcept {
    const auto iterator = std::find_if(entries.begin(), entries.end(),
                                       [&connection_id](const Entry& entry) {
                                         return entry.connection_id == connection_id;
                                       });
    return iterator == entries.end() ? nullptr : &*iterator;
  }

  [[nodiscard]] const Entry* Find(const AttemptHandle& connection_id) const noexcept {
    const auto iterator = std::find_if(entries.begin(), entries.end(),
                                       [&connection_id](const Entry& entry) {
                                         return entry.connection_id == connection_id;
                                       });
    return iterator == entries.end() ? nullptr : &*iterator;
  }

  [[nodiscard]] Entry* Find(const AttemptHandle& connection_id,
                            const std::uint64_t lease_generation) noexcept {
    Entry* const entry = Find(connection_id);
    return entry != nullptr && entry->lease_generation == lease_generation ? entry
                                                                           : nullptr;
  }

  [[nodiscard]] const Entry* Find(const AttemptHandle& connection_id,
                                  const std::uint64_t lease_generation) const noexcept {
    const Entry* const entry = Find(connection_id);
    return entry != nullptr && entry->lease_generation == lease_generation ? entry
                                                                           : nullptr;
  }

  [[nodiscard]] static bool SameKeyPair(const Entry& entry,
                                        const PairingAdmissionRequest& request) {
    return (entry.local_key == request.local_key &&
            entry.peer_key == request.peer_key) ||
           (entry.local_key == request.peer_key && entry.peer_key == request.local_key);
  }

  [[nodiscard]] static PublicKey InitiatorKey(const Entry& entry) {
    return entry.local_role == security::tls::Role::kInitiator ? entry.local_key
                                                               : entry.peer_key;
  }

  [[nodiscard]] static PublicKey InitiatorKey(const PairingAdmissionRequest& request) {
    return request.local_role == security::tls::Role::kInitiator ? request.local_key
                                                                 : request.peer_key;
  }

  void Release(const AttemptHandle& connection_id) noexcept {
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&connection_id](const Entry& entry) {
                                   return entry.connection_id == connection_id;
                                 }),
                  entries.end());
  }

  void Release(const AttemptHandle& connection_id,
               const std::uint64_t lease_generation) noexcept {
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [&connection_id, lease_generation](const Entry& entry) {
                         return entry.connection_id == connection_id &&
                                entry.lease_generation == lease_generation;
                       }),
        entries.end());
  }

  bool initialized{};
  bool window_enabled{};
  std::uint64_t window_deadline_ms{};
  Bucket global_bucket{};
  std::uint64_t next_lease_generation{1};
  std::vector<SourceBucket> source_buckets{};
  std::vector<Entry> entries{};
};

}  // namespace detail

PairingAdmissionLease::PairingAdmissionLease(
    std::shared_ptr<detail::PairingAdmissionState> state, AttemptHandle connection_id,
    const std::uint64_t lease_generation)
    : state_(std::move(state)),
      connection_id_(connection_id),
      lease_generation_(lease_generation) {}

PairingAdmissionLease::~PairingAdmissionLease() {
  if (state_ != nullptr) {
    const std::lock_guard lock(state_->mutex);
    state_->Release(connection_id_, lease_generation_);
  }
}

PairingAdmissionLease::PairingAdmissionLease(PairingAdmissionLease&&) noexcept =
    default;
PairingAdmissionLease& PairingAdmissionLease::operator=(
    PairingAdmissionLease&& other) noexcept {
  if (this != &other) {
    if (state_ != nullptr) {
      const std::lock_guard lock(state_->mutex);
      state_->Release(connection_id_, lease_generation_);
    }
    state_ = std::move(other.state_);
    connection_id_ = std::exchange(other.connection_id_, AttemptHandle{});
    lease_generation_ = std::exchange(other.lease_generation_, 0);
  }
  return *this;
}

bool PairingAdmissionLease::active() const noexcept {
  if (state_ == nullptr) {
    return false;
  }
  const std::lock_guard lock(state_->mutex);
  return state_->Find(connection_id_, lease_generation_) != nullptr;
}

PairingError PairingAdmissionLease::MarkVisible() noexcept {
  if (state_ == nullptr) {
    return PairingError::kStateViolation;
  }
  const std::lock_guard lock(state_->mutex);
  detail::PairingAdmissionState::Entry* const entry =
      state_->Find(connection_id_, lease_generation_);
  if (entry == nullptr) {
    return PairingError::kBusy;
  }
  if (entry->visible) {
    return PairingError::kNone;
  }
  const std::size_t visible = static_cast<std::size_t>(
      std::count_if(state_->entries.begin(), state_->entries.end(),
                    [](const detail::PairingAdmissionState::Entry& candidate) {
                      return candidate.visible;
                    }));
  if (visible == kMaxVisiblePairingAttempts) {
    return PairingError::kBusy;
  }
  entry->visible = true;
  return PairingError::kNone;
}

const AttemptHandle& PairingAdmissionLease::connection_id() const noexcept {
  return connection_id_;
}

PublicKey PairingAdmissionLease::local_key() const noexcept {
  if (state_ == nullptr) {
    return {};
  }
  const std::lock_guard lock(state_->mutex);
  const detail::PairingAdmissionState::Entry* const entry =
      state_->Find(connection_id_, lease_generation_);
  return entry == nullptr ? PublicKey{} : entry->local_key;
}

PublicKey PairingAdmissionLease::peer_key() const noexcept {
  if (state_ == nullptr) {
    return {};
  }
  const std::lock_guard lock(state_->mutex);
  const detail::PairingAdmissionState::Entry* const entry =
      state_->Find(connection_id_, lease_generation_);
  return entry == nullptr ? PublicKey{} : entry->peer_key;
}

security::tls::Role PairingAdmissionLease::local_role() const noexcept {
  if (state_ == nullptr) {
    return security::tls::Role::kInitiator;
  }
  const std::lock_guard lock(state_->mutex);
  const detail::PairingAdmissionState::Entry* const entry =
      state_->Find(connection_id_, lease_generation_);
  return entry == nullptr ? security::tls::Role::kInitiator : entry->local_role;
}

std::uint64_t PairingAdmissionLease::admitted_at_ms() const noexcept {
  if (state_ == nullptr) {
    return 0;
  }
  const std::lock_guard lock(state_->mutex);
  const detail::PairingAdmissionState::Entry* const entry =
      state_->Find(connection_id_, lease_generation_);
  return entry == nullptr ? 0 : entry->admitted_at_ms;
}

std::uint64_t PairingAdmissionLease::window_deadline_ms() const noexcept {
  if (state_ == nullptr) {
    return 0;
  }
  const std::lock_guard lock(state_->mutex);
  const detail::PairingAdmissionState::Entry* const entry =
      state_->Find(connection_id_, lease_generation_);
  return entry == nullptr ? 0 : entry->window_deadline_ms;
}

PairingAdmissionController::PairingAdmissionController()
    : state_(std::make_shared<detail::PairingAdmissionState>()) {}
PairingAdmissionController::~PairingAdmissionController() = default;
PairingAdmissionController::PairingAdmissionController(
    PairingAdmissionController&&) noexcept = default;
PairingAdmissionController& PairingAdmissionController::operator=(
    PairingAdmissionController&&) noexcept = default;

bool PairingAdmissionController::OpenWindow(const std::uint64_t now_ms,
                                            const std::uint64_t duration_ms) {
  if (state_ == nullptr || duration_ms == 0 || duration_ms > kMaximumPairingWindowMs) {
    return false;
  }
  const std::lock_guard lock(state_->mutex);
  if (state_->window_enabled && now_ms < state_->window_deadline_ms) {
    return false;
  }
  if (!state_->initialized) {
    state_->initialized = true;
    state_->global_bucket = {
        .tokens = kGlobalAdmissionBucketCapacity,
        .last_refill_ms = now_ms,
    };
  }
  state_->window_enabled = true;
  state_->window_deadline_ms = CheckedDeadline(now_ms, duration_ms);
  return true;
}

void PairingAdmissionController::CloseWindow() noexcept {
  if (state_ == nullptr) {
    return;
  }
  const std::lock_guard lock(state_->mutex);
  state_->window_enabled = false;
  state_->entries.clear();
}

PairingAdmissionResult PairingAdmissionController::Admit(
    const PairingAdmissionRequest& request) {
  if (state_ == nullptr) {
    return {.error = PairingError::kBusy};
  }
  const std::lock_guard lock(state_->mutex);
  if (!state_->window_enabled || request.now_ms >= state_->window_deadline_ms) {
    return {.error = PairingError::kBusy};
  }
  if (!IsRole(request.local_role) || AllZero(request.connection_id) ||
      request.local_key == request.peer_key ||
      state_->Find(request.connection_id) != nullptr) {
    return {.error = PairingError::kCertificateRejected};
  }
  detail::PairingAdmissionState::Refill(state_->global_bucket, request.now_ms,
                                        kGlobalAdmissionRefillMs,
                                        kGlobalAdmissionBucketCapacity);
  state_->PruneSourceBuckets(request.now_ms);

  detail::PairingAdmissionState::Entry* duplicate = nullptr;
  for (detail::PairingAdmissionState::Entry& entry : state_->entries) {
    if (detail::PairingAdmissionState::SameKeyPair(entry, request)) {
      duplicate = &entry;
      break;
    }
  }

  std::optional<AttemptHandle> displaced;
  if (duplicate != nullptr) {
    if (duplicate->visible) {
      return {.error = PairingError::kBusy};
    }
    const PublicKey existing_initiator =
        detail::PairingAdmissionState::InitiatorKey(*duplicate);
    const PublicKey candidate_initiator =
        detail::PairingAdmissionState::InitiatorKey(request);
    if (existing_initiator == candidate_initiator) {
      return {.error = PairingError::kBusy};
    }
    const PublicKey canonical_initiator = std::min(request.local_key, request.peer_key);
    if (candidate_initiator != canonical_initiator) {
      return {.error = PairingError::kBusy};
    }
    displaced = duplicate->connection_id;
  }

  const std::size_t active_count =
      state_->entries.size() - static_cast<std::size_t>(displaced.has_value());
  if (active_count >= kMaxIncompletePairingHandshakes) {
    return {.error = PairingError::kBusy};
  }
  if (!request.user_initiated) {
    const std::size_t non_user_count = static_cast<std::size_t>(std::count_if(
        state_->entries.begin(), state_->entries.end(),
        [&displaced](const detail::PairingAdmissionState::Entry& entry) {
          return !entry.user_initiated &&
                 (!displaced.has_value() || entry.connection_id != *displaced);
        }));
    if (non_user_count >=
        kMaxIncompletePairingHandshakes - kReservedUserInitiatedPairingSlots) {
      return {.error = PairingError::kBusy};
    }
  }
  if (state_->ActiveForSource(request.source,
                              displaced.has_value() ? &*displaced : nullptr) >=
      kMaxIncompletePairingHandshakesPerSource) {
    return {.error = PairingError::kBusy};
  }
  if (state_->global_bucket.tokens == 0) {
    return {.error = PairingError::kBusy};
  }

  detail::PairingAdmissionState::SourceBucket* source =
      state_->FindSource(request.source);
  if (source == nullptr) {
    if (state_->source_buckets.size() == kMaxTrackedSourceBuckets) {
      return {.error = PairingError::kBusy};
    }
    try {
      state_->source_buckets.push_back(detail::PairingAdmissionState::SourceBucket{
          .source = request.source,
          .bucket =
              {
                  .tokens = kSourceAdmissionBucketCapacity,
                  .last_refill_ms = request.now_ms,
              },
      });
      source = &state_->source_buckets.back();
    } catch (const std::bad_alloc&) {
      return {.error = PairingError::kBusy};
    }
  }
  if (source->bucket.tokens == 0) {
    return {.error = PairingError::kBusy};
  }
  if (state_->next_lease_generation == 0) {
    return {.error = PairingError::kBusy};
  }
  const std::uint64_t lease_generation = state_->next_lease_generation;

  try {
    state_->entries.push_back(detail::PairingAdmissionState::Entry{
        .connection_id = request.connection_id,
        .lease_generation = lease_generation,
        .source = request.source,
        .local_key = request.local_key,
        .peer_key = request.peer_key,
        .local_role = request.local_role,
        .user_initiated = request.user_initiated,
        .admitted_at_ms = request.now_ms,
        .window_deadline_ms = state_->window_deadline_ms,
    });
  } catch (const std::bad_alloc&) {
    return {.error = PairingError::kBusy};
  }

  std::unique_ptr<PairingAdmissionLease> lease;
  try {
    lease.reset(
        new PairingAdmissionLease(state_, request.connection_id, lease_generation));
  } catch (const std::bad_alloc&) {
    state_->Release(request.connection_id, lease_generation);
    return {.error = PairingError::kBusy};
  }

  if (displaced.has_value()) {
    state_->Release(*displaced);
  }
  ++state_->next_lease_generation;
  --state_->global_bucket.tokens;
  --source->bucket.tokens;
  return {
      .error = PairingError::kNone,
      .displaced_connection = displaced,
      .lease = std::move(lease),
  };
}

bool PairingAdmissionController::window_open(
    const std::uint64_t now_ms) const noexcept {
  if (state_ == nullptr) {
    return false;
  }
  const std::lock_guard lock(state_->mutex);
  return state_->window_enabled && now_ms < state_->window_deadline_ms;
}

std::size_t PairingAdmissionController::active_connections() const noexcept {
  if (state_ == nullptr) {
    return 0;
  }
  const std::lock_guard lock(state_->mutex);
  return state_->entries.size();
}

std::size_t PairingAdmissionController::visible_attempts() const noexcept {
  if (state_ == nullptr) {
    return 0;
  }
  const std::lock_guard lock(state_->mutex);
  return static_cast<std::size_t>(std::count_if(
      state_->entries.begin(), state_->entries.end(),
      [](const detail::PairingAdmissionState::Entry& entry) { return entry.visible; }));
}

}  // namespace xnn_transfer::core::session
