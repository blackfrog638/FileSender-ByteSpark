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

  struct Owner {
    std::uint64_t generation{};
    std::uint64_t window_generation{};
    bool window_enabled{};
    std::uint64_t window_deadline_ms{};
  };

  struct Entry {
    AttemptHandle connection_id{};
    std::uint64_t lease_generation{};
    std::uint64_t owner_generation{};
    SourceToken source{};
    PublicKey local_key{};
    PublicKey peer_key{};
    security::tls::Role local_role{security::tls::Role::kInitiator};
    bool user_initiated{};
    bool visible{};
    bool bound{};
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

  [[nodiscard]] Owner* FindOwner(const std::uint64_t generation) noexcept {
    const auto iterator = std::find_if(
        owners.begin(), owners.end(),
        [generation](const Owner& owner) { return owner.generation == generation; });
    return iterator == owners.end() ? nullptr : &*iterator;
  }

  [[nodiscard]] const Owner* FindOwner(const std::uint64_t generation) const noexcept {
    const auto iterator = std::find_if(
        owners.begin(), owners.end(),
        [generation](const Owner& owner) { return owner.generation == generation; });
    return iterator == owners.end() ? nullptr : &*iterator;
  }

  [[nodiscard]] std::uint64_t RegisterOwner() {
    if (next_owner_generation == 0U) {
      return 0U;
    }
    const std::uint64_t generation = next_owner_generation;
    owners.push_back(Owner{.generation = generation});
    ++next_owner_generation;
    return generation;
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

  void ReleaseOwner(const std::uint64_t generation) noexcept {
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [generation](const Entry& entry) {
                                   return entry.owner_generation == generation;
                                 }),
                  entries.end());
    owners.erase(std::remove_if(owners.begin(), owners.end(),
                                [generation](const Owner& owner) {
                                  return owner.generation == generation;
                                }),
                 owners.end());
  }

  bool initialized{};
  Bucket global_bucket{};
  std::uint64_t next_lease_generation{1};
  std::uint64_t next_owner_generation{1};
  std::uint64_t next_window_generation{1};
  std::vector<SourceBucket> source_buckets{};
  std::vector<Entry> entries{};
  std::vector<Owner> owners{};
};

}  // namespace detail

namespace {

struct ProcessAdmissionRegistry {
  std::mutex mutex{};
  std::shared_ptr<detail::PairingAdmissionState> state{
      std::make_shared<detail::PairingAdmissionState>()};
};

[[nodiscard]] ProcessAdmissionRegistry& AdmissionRegistry() {
  static ProcessAdmissionRegistry registry;
  return registry;
}

}  // namespace

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

bool PairingAdmissionLease::bound() const noexcept {
  if (state_ == nullptr) {
    return false;
  }
  const std::lock_guard lock(state_->mutex);
  const detail::PairingAdmissionState::Entry* const entry =
      state_->Find(connection_id_, lease_generation_);
  return entry != nullptr && entry->bound;
}

PairingError PairingAdmissionLease::MarkVisible() noexcept {
  if (state_ == nullptr) {
    return PairingError::kStateViolation;
  }
  const std::lock_guard lock(state_->mutex);
  detail::PairingAdmissionState::Entry* const entry =
      state_->Find(connection_id_, lease_generation_);
  if (entry == nullptr || !entry->bound) {
    return PairingError::kBusy;
  }
  if (entry->visible) {
    return PairingError::kNone;
  }
  const std::size_t visible = static_cast<std::size_t>(
      std::count_if(state_->entries.begin(), state_->entries.end(),
                    [entry](const detail::PairingAdmissionState::Entry& candidate) {
                      return candidate.owner_generation == entry->owner_generation &&
                             candidate.visible;
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
    : state_(std::make_shared<detail::PairingAdmissionState>()) {
  const std::lock_guard lock(state_->mutex);
  owner_generation_ = state_->RegisterOwner();
}

PairingAdmissionController::PairingAdmissionController(
    std::shared_ptr<detail::PairingAdmissionState> state,
    const std::uint64_t owner_generation)
    : state_(std::move(state)), owner_generation_(owner_generation) {}

PairingAdmissionController::~PairingAdmissionController() = default;

PairingAdmissionController::PairingAdmissionController(
    PairingAdmissionController&& other) noexcept
    : state_(std::move(other.state_)),
      owner_generation_(std::exchange(other.owner_generation_, 0U)) {}

PairingAdmissionController& PairingAdmissionController::operator=(
    PairingAdmissionController&& other) noexcept {
  if (this != &other) {
    state_ = std::move(other.state_);
    owner_generation_ = std::exchange(other.owner_generation_, 0U);
  }
  return *this;
}

PairingAdmissionController PairingAdmissionController::ProcessScoped() {
  ProcessAdmissionRegistry& registry = AdmissionRegistry();
  const std::lock_guard registry_lock(registry.mutex);
  const std::shared_ptr<detail::PairingAdmissionState> state = registry.state;
  const std::lock_guard state_lock(state->mutex);
  return PairingAdmissionController(state, state->RegisterOwner());
}

bool PairingAdmissionController::ResetProcessStateForTesting() {
  ProcessAdmissionRegistry& registry = AdmissionRegistry();
  const std::lock_guard registry_lock(registry.mutex);
  {
    const std::lock_guard state_lock(registry.state->mutex);
    if (!registry.state->owners.empty() || !registry.state->entries.empty()) {
      return false;
    }
  }
  registry.state = std::make_shared<detail::PairingAdmissionState>();
  return true;
}

void PairingAdmissionController::RetireOwner() noexcept {
  if (state_ == nullptr || owner_generation_ == 0U) {
    return;
  }
  const std::lock_guard lock(state_->mutex);
  state_->ReleaseOwner(owner_generation_);
  owner_generation_ = 0U;
}

bool PairingAdmissionController::OpenWindow(const std::uint64_t now_ms,
                                            const std::uint64_t duration_ms) {
  if (state_ == nullptr || duration_ms == 0 || duration_ms > kMaximumPairingWindowMs) {
    return false;
  }
  const std::lock_guard lock(state_->mutex);
  detail::PairingAdmissionState::Owner* const owner =
      state_->FindOwner(owner_generation_);
  if (owner == nullptr ||
      (owner->window_enabled && now_ms < owner->window_deadline_ms) ||
      state_->next_window_generation == 0U) {
    return false;
  }
  if (!state_->initialized) {
    state_->initialized = true;
    state_->global_bucket = {
        .tokens = kGlobalAdmissionBucketCapacity,
        .last_refill_ms = now_ms,
    };
  }
  owner->window_generation = state_->next_window_generation;
  ++state_->next_window_generation;
  owner->window_enabled = true;
  owner->window_deadline_ms = CheckedDeadline(now_ms, duration_ms);
  return true;
}

void PairingAdmissionController::CloseWindow() noexcept {
  if (state_ == nullptr) {
    return;
  }
  const std::lock_guard lock(state_->mutex);
  detail::PairingAdmissionState::Owner* const owner =
      state_->FindOwner(owner_generation_);
  if (owner == nullptr) {
    return;
  }
  owner->window_enabled = false;
  state_->entries.erase(
      std::remove_if(state_->entries.begin(), state_->entries.end(),
                     [this](const detail::PairingAdmissionState::Entry& entry) {
                       return entry.owner_generation == owner_generation_ &&
                              entry.bound;
                     }),
      state_->entries.end());
}

std::unique_ptr<PairingAdmissionLease> PairingAdmissionController::ReserveHandshake(
    const AttemptHandle& connection_id, const SourceToken& source_token,
    const bool user_initiated, const std::uint64_t now_ms) {
  if (state_ == nullptr) {
    return nullptr;
  }
  const std::lock_guard lock(state_->mutex);
  if (state_->FindOwner(owner_generation_) == nullptr || AllZero(connection_id) ||
      AllZero(source_token) || state_->Find(connection_id) != nullptr) {
    return nullptr;
  }
  if (!state_->initialized) {
    state_->initialized = true;
    state_->global_bucket = {
        .tokens = kGlobalAdmissionBucketCapacity,
        .last_refill_ms = now_ms,
    };
  }
  detail::PairingAdmissionState::Refill(state_->global_bucket, now_ms,
                                        kGlobalAdmissionRefillMs,
                                        kGlobalAdmissionBucketCapacity);
  state_->PruneSourceBuckets(now_ms);

  if (state_->entries.size() >= kMaxIncompletePairingHandshakes) {
    return nullptr;
  }
  if (!user_initiated) {
    const std::size_t non_user_count = static_cast<std::size_t>(
        std::count_if(state_->entries.begin(), state_->entries.end(),
                      [](const detail::PairingAdmissionState::Entry& entry) {
                        return !entry.user_initiated;
                      }));
    if (non_user_count >=
        kMaxIncompletePairingHandshakes - kReservedUserInitiatedPairingSlots) {
      return nullptr;
    }
  }
  if (state_->ActiveForSource(source_token) >=
      kMaxIncompletePairingHandshakesPerSource) {
    return nullptr;
  }
  if (state_->global_bucket.tokens == 0) {
    return nullptr;
  }

  detail::PairingAdmissionState::SourceBucket* source =
      state_->FindSource(source_token);
  if (source == nullptr) {
    if (state_->source_buckets.size() == kMaxTrackedSourceBuckets) {
      return nullptr;
    }
    try {
      state_->source_buckets.push_back(detail::PairingAdmissionState::SourceBucket{
          .source = source_token,
          .bucket =
              {
                  .tokens = kSourceAdmissionBucketCapacity,
                  .last_refill_ms = now_ms,
              },
      });
      source = &state_->source_buckets.back();
    } catch (const std::bad_alloc&) {
      return nullptr;
    }
  }
  if (source->bucket.tokens == 0) {
    return nullptr;
  }
  if (state_->next_lease_generation == 0) {
    return nullptr;
  }
  const std::uint64_t lease_generation = state_->next_lease_generation;

  try {
    state_->entries.push_back(detail::PairingAdmissionState::Entry{
        .connection_id = connection_id,
        .lease_generation = lease_generation,
        .owner_generation = owner_generation_,
        .source = source_token,
        .user_initiated = user_initiated,
        .admitted_at_ms = now_ms,
    });
  } catch (const std::bad_alloc&) {
    return nullptr;
  }

  std::unique_ptr<PairingAdmissionLease> lease;
  try {
    lease.reset(new PairingAdmissionLease(state_, connection_id, lease_generation));
  } catch (const std::bad_alloc&) {
    state_->Release(connection_id, lease_generation);
    return nullptr;
  }
  ++state_->next_lease_generation;
  --state_->global_bucket.tokens;
  --source->bucket.tokens;
  return lease;
}

PairingAdmissionResult PairingAdmissionController::Bind(
    std::unique_ptr<PairingAdmissionLease> lease,
    const PairingAdmissionRequest& request,
    const std::uint64_t pairing_window_generation) {
  if (state_ == nullptr || lease == nullptr || lease->state_ != state_) {
    return {.error = PairingError::kBusy};
  }
  const std::lock_guard lock(state_->mutex);
  detail::PairingAdmissionState::Entry* const reserved =
      state_->Find(lease->connection_id_, lease->lease_generation_);
  detail::PairingAdmissionState::Owner* const owner =
      state_->FindOwner(owner_generation_);
  if (reserved == nullptr || reserved->bound ||
      reserved->owner_generation != owner_generation_ || owner == nullptr ||
      reserved->connection_id != request.connection_id ||
      reserved->source != request.source ||
      reserved->user_initiated != request.user_initiated || !owner->window_enabled ||
      request.now_ms >= owner->window_deadline_ms || pairing_window_generation == 0U ||
      pairing_window_generation != owner->window_generation) {
    return {.error = PairingError::kBusy};
  }
  if (!IsRole(request.local_role) || request.local_key == request.peer_key) {
    return {.error = PairingError::kCertificateRejected};
  }

  detail::PairingAdmissionState::Entry* duplicate = nullptr;
  for (detail::PairingAdmissionState::Entry& entry : state_->entries) {
    if (&entry != reserved && entry.owner_generation == owner_generation_ &&
        entry.bound && detail::PairingAdmissionState::SameKeyPair(entry, request)) {
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

  reserved->local_key = request.local_key;
  reserved->peer_key = request.peer_key;
  reserved->local_role = request.local_role;
  reserved->window_deadline_ms = owner->window_deadline_ms;
  reserved->bound = true;
  if (displaced.has_value()) {
    state_->Release(*displaced);
  }
  return {
      .error = PairingError::kNone,
      .displaced_connection = displaced,
      .lease = std::move(lease),
  };
}

PairingAdmissionResult PairingAdmissionController::Admit(
    const PairingAdmissionRequest& request) {
  if (!IsRole(request.local_role) || AllZero(request.connection_id) ||
      request.local_key == request.peer_key) {
    return {.error = PairingError::kCertificateRejected};
  }
  const std::uint64_t pairing_window_generation = window_generation(request.now_ms);
  std::unique_ptr<PairingAdmissionLease> lease = ReserveHandshake(
      request.connection_id, request.source, request.user_initiated, request.now_ms);
  if (lease == nullptr) {
    return {.error = PairingError::kBusy};
  }
  return Bind(std::move(lease), request, pairing_window_generation);
}

bool PairingAdmissionController::window_open(
    const std::uint64_t now_ms) const noexcept {
  if (state_ == nullptr) {
    return false;
  }
  const std::lock_guard lock(state_->mutex);
  const detail::PairingAdmissionState::Owner* const owner =
      state_->FindOwner(owner_generation_);
  return owner != nullptr && owner->window_enabled &&
         now_ms < owner->window_deadline_ms;
}

std::uint64_t PairingAdmissionController::window_generation(
    const std::uint64_t now_ms) const noexcept {
  if (state_ == nullptr) {
    return 0U;
  }
  const std::lock_guard lock(state_->mutex);
  const detail::PairingAdmissionState::Owner* const owner =
      state_->FindOwner(owner_generation_);
  return owner != nullptr && owner->window_enabled && now_ms < owner->window_deadline_ms
             ? owner->window_generation
             : 0U;
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
  const std::uint64_t owner_generation = owner_generation_;
  return static_cast<std::size_t>(std::count_if(
      state_->entries.begin(), state_->entries.end(),
      [owner_generation](const detail::PairingAdmissionState::Entry& entry) {
        return entry.owner_generation == owner_generation && entry.visible;
      }));
}

}  // namespace xnn_transfer::core::session
