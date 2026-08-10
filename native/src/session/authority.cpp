#include <algorithm>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "xnn_transfer/core/session/session.hpp"

namespace xnn_transfer::core::session {
namespace {

constexpr std::size_t kMaxActiveSessions = security::identity::kMaxPeerRecords;
constexpr std::size_t kHandleGenerationAttempts = 4;

[[nodiscard]] bool AllZero(const SessionHandle& handle) noexcept {
  std::uint8_t accumulator = 0;
  for (const std::uint8_t byte : handle) {
    accumulator = static_cast<std::uint8_t>(accumulator | byte);
  }
  return accumulator == 0;
}

}  // namespace

struct SessionAuthority::Implementation {
  struct Entry {
    SessionHandle handle{};
    std::unique_ptr<EstablishedTlsChannel> channel{};
  };

  [[nodiscard]] bool Current(const Entry& entry) const noexcept {
    if (shutdown || !repository->ready()) {
      return false;
    }
    const security::identity::PeerRecord* const peer =
        repository->FindPeer(entry.channel->peer_device_id());
    return entry.channel->transport_bound() &&
           repository->revision() == entry.channel->identity_revision() &&
           peer != nullptr &&
           peer->trust_state == security::identity::TrustState::kActive &&
           peer->public_key == entry.channel->peer_public_key() &&
           peer->record_revision == entry.channel->peer_record_revision() &&
           peer->security_profile <= entry.channel->security_profile() &&
           entry.channel->current_transport();
  }

  [[nodiscard]] const Entry* Find(const SessionHandle& handle) const noexcept {
    const auto iterator =
        std::find_if(entries.begin(), entries.end(),
                     [&handle](const Entry& entry) { return entry.handle == handle; });
    return iterator == entries.end() ? nullptr : &*iterator;
  }

  security::identity::IdentityRepository* repository{};
  SessionEntropy* entropy{};
  mutable std::mutex mutex{};
  std::vector<Entry> entries{};
  bool shutdown{};
};

SessionAuthority::SessionAuthority(security::identity::IdentityRepository& repository,
                                   SessionEntropy& entropy)
    : implementation_(std::make_unique<Implementation>()) {
  implementation_->repository = &repository;
  implementation_->entropy = &entropy;
}

SessionAuthority::~SessionAuthority() = default;

AuthorizationResult SessionAuthority::Activate(
    std::unique_ptr<EstablishedTlsChannel> channel) {
  const std::lock_guard lock(implementation_->mutex);
  if (implementation_->shutdown || channel == nullptr) {
    return {.error = AuthorizationError::kInvalidArgument};
  }
  if (!channel->transport_bound()) {
    return {.error = AuthorizationError::kUnauthenticated};
  }
  if (!implementation_->repository->ready()) {
    return {.error = AuthorizationError::kUnavailable};
  }
  if (implementation_->entries.size() == kMaxActiveSessions) {
    return {.error = AuthorizationError::kUnavailable};
  }

  const security::identity::PeerRecord* const peer =
      implementation_->repository->FindPeer(channel->peer_device_id());
  if (peer == nullptr || peer->trust_state != security::identity::TrustState::kActive ||
      peer->public_key != channel->peer_public_key() ||
      peer->record_revision != channel->peer_record_revision() ||
      implementation_->repository->revision() != channel->identity_revision() ||
      peer->security_profile > channel->security_profile() ||
      !channel->current_transport()) {
    return {.error = AuthorizationError::kUnauthenticated};
  }

  SessionHandle handle{};
  bool generated = false;
  for (std::size_t attempt = 0; attempt < kHandleGenerationAttempts; ++attempt) {
    if (!implementation_->entropy->Fill(handle)) {
      return {.error = AuthorizationError::kEntropyFailure};
    }
    const bool duplicate =
        std::any_of(implementation_->entries.begin(), implementation_->entries.end(),
                    [&handle](const Implementation::Entry& entry) {
                      return entry.handle == handle;
                    });
    if (!AllZero(handle) && !duplicate) {
      generated = true;
      break;
    }
  }
  if (!generated) {
    return {.error = AuthorizationError::kEntropyFailure};
  }

  try {
    implementation_->entries.push_back(Implementation::Entry{
        .handle = handle,
        .channel = std::move(channel),
    });
    return {
        .error = AuthorizationError::kNone,
        .handle = handle,
    };
  } catch (const std::bad_alloc&) {
    return {.error = AuthorizationError::kUnavailable};
  }
}

bool SessionAuthority::IsAuthorized(const SessionHandle& handle) const noexcept {
  const std::lock_guard lock(implementation_->mutex);
  const Implementation::Entry* const entry = implementation_->Find(handle);
  return entry != nullptr && implementation_->Current(*entry);
}

AuthorizationError SessionAuthority::Deactivate(const SessionHandle& handle) noexcept {
  const std::lock_guard lock(implementation_->mutex);
  const std::size_t previous_size = implementation_->entries.size();
  implementation_->entries.erase(
      std::remove_if(implementation_->entries.begin(), implementation_->entries.end(),
                     [&handle](const Implementation::Entry& entry) {
                       return entry.handle == handle;
                     }),
      implementation_->entries.end());
  return implementation_->entries.size() == previous_size
             ? AuthorizationError::kUnauthenticated
             : AuthorizationError::kNone;
}

AuthorizationError SessionAuthority::Revoke(const SessionHandle& handle) {
  const std::lock_guard lock(implementation_->mutex);
  const Implementation::Entry* const entry = implementation_->Find(handle);
  if (entry == nullptr || !implementation_->Current(*entry)) {
    return AuthorizationError::kUnauthenticated;
  }
  const DeviceId peer_device_id = entry->channel->peer_device_id();
  const auto revoked = implementation_->repository->RevokePeer(peer_device_id);
  if (!revoked.ok()) {
    return AuthorizationError::kStorageFailure;
  }
  implementation_->entries.erase(
      std::remove_if(implementation_->entries.begin(), implementation_->entries.end(),
                     [&peer_device_id](const Implementation::Entry& current) {
                       return current.channel->peer_device_id() == peer_device_id;
                     }),
      implementation_->entries.end());
  return AuthorizationError::kNone;
}

void SessionAuthority::InvalidateStale() noexcept {
  const std::lock_guard lock(implementation_->mutex);
  implementation_->entries.erase(
      std::remove_if(implementation_->entries.begin(), implementation_->entries.end(),
                     [this](const Implementation::Entry& entry) {
                       return !implementation_->Current(entry);
                     }),
      implementation_->entries.end());
}

void SessionAuthority::Shutdown() noexcept {
  const std::lock_guard lock(implementation_->mutex);
  implementation_->shutdown = true;
  implementation_->entries.clear();
}

std::size_t SessionAuthority::active_sessions() const noexcept {
  const std::lock_guard lock(implementation_->mutex);
  return implementation_->entries.size();
}

}  // namespace xnn_transfer::core::session
