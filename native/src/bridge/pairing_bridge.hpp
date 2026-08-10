#ifndef XNN_TRANSFER_BRIDGE_PAIRING_BRIDGE_HPP_
#define XNN_TRANSFER_BRIDGE_PAIRING_BRIDGE_HPP_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "event_channel.hpp"
#include "xnn_transfer/c_api.h"
#include "xnn_transfer/core/security/identity/types.hpp"
#include "xnn_transfer/core/security/tls/security_profile.hpp"
#include "xnn_transfer/core/session/session.hpp"

namespace xnn_transfer::bridge {

class PairingBackend {
 public:
  virtual ~PairingBackend() = default;

  [[nodiscard]] virtual xnn_transfer_status OpenWindow(std::uint64_t now_ms,
                                                       std::uint64_t duration_ms) = 0;
  virtual void CloseWindow() = 0;
  [[nodiscard]] virtual xnn_transfer_status Start(std::uint64_t peer_id,
                                                  std::uint64_t now_ms) = 0;
  [[nodiscard]] virtual core::session::PairingUpdate Decide(
      const core::session::AttemptHandle& attempt,
      core::security::tls::ConfirmationDecision decision, std::uint64_t now_ms) = 0;
  [[nodiscard]] virtual xnn_transfer_status Revoke(
      const core::security::identity::DeviceId& device_id) = 0;
  virtual void Shutdown() = 0;
};

class PairingBridge final {
 public:
  explicit PairingBridge(PairingBackend& backend) : backend_(&backend) {}

  PairingBridge(const PairingBridge&) = delete;
  PairingBridge& operator=(const PairingBridge&) = delete;

  [[nodiscard]] xnn_transfer_status OpenWindow(const std::uint64_t now_ms,
                                               const std::uint64_t duration_ms) {
    return backend_->OpenWindow(now_ms, duration_ms);
  }

  [[nodiscard]] xnn_transfer_status CloseWindow(EventChannel& events) {
    backend_->CloseWindow();
    const std::optional<xnn_transfer_pairing_attempt_event_payload> closed =
        ClearActive(XNN_TRANSFER_PAIRING_ERROR_CANCELLED);
    if (closed.has_value()) {
      events.EnqueuePairing(*closed);
    }
    return XNN_TRANSFER_STATUS_OK;
  }

  [[nodiscard]] xnn_transfer_status Start(const std::uint64_t peer_id,
                                          const std::uint64_t now_ms) {
    if (peer_id == 0) {
      return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
    }
    return backend_->Start(peer_id, now_ms);
  }

  [[nodiscard]] xnn_transfer_status Decide(
      const std::span<const std::uint8_t, XNN_TRANSFER_PAIRING_ATTEMPT_ID_SIZE>
          attempt_id,
      const core::security::tls::ConfirmationDecision decision,
      const std::uint64_t now_ms, EventChannel& events) {
    core::session::AttemptHandle attempt{};
    std::uint64_t peer_id = 0;
    {
      const std::scoped_lock lock(mutex_);
      if (!active_attempt_.has_value() ||
          !std::equal(attempt_id.begin(), attempt_id.end(),
                      std::begin(active_attempt_->attempt_id))) {
        return XNN_TRANSFER_STATUS_STALE_HANDLE;
      }
      std::copy(attempt_id.begin(), attempt_id.end(), attempt.begin());
      peer_id = active_attempt_->peer_id;
    }

    const core::session::PairingUpdate update =
        backend_->Decide(attempt, decision, now_ms);
    if (!update.terminal && update.error != core::session::PairingError::kNone) {
      return update.error == core::session::PairingError::kStateViolation
                 ? XNN_TRANSFER_STATUS_STALE_HANDLE
                 : XNN_TRANSFER_STATUS_INVALID_STATE;
    }
    return Apply(peer_id, attempt, update, events) ? XNN_TRANSFER_STATUS_OK
                                                   : XNN_TRANSFER_STATUS_STALE_HANDLE;
  }

  [[nodiscard]] xnn_transfer_status Revoke(const std::uint64_t trust_id,
                                           EventChannel& events) {
    core::security::identity::DeviceId device_id{};
    {
      const std::scoped_lock lock(mutex_);
      const TrustEntry* const entry = FindTrust(trust_id);
      if (entry == nullptr || entry->state == XNN_TRANSFER_TRUST_STATE_REVOKED) {
        return XNN_TRANSFER_STATUS_OK;
      }
      device_id = entry->device_id;
    }

    const xnn_transfer_status status = backend_->Revoke(device_id);
    if (status != XNN_TRANSFER_STATUS_OK) {
      return status;
    }

    std::optional<xnn_transfer_trust_event_payload> revoked;
    {
      const std::scoped_lock lock(mutex_);
      TrustEntry* const entry = FindTrust(trust_id);
      if (entry == nullptr || entry->state == XNN_TRANSFER_TRUST_STATE_REVOKED) {
        return XNN_TRANSFER_STATUS_OK;
      }
      entry->state = XNN_TRANSFER_TRUST_STATE_REVOKED;
      AdvanceRevision(trust_revision_);
      revoked = MakeTrustPayload(*entry);
    }
    events.EnqueueTrust(*revoked);
    return XNN_TRANSFER_STATUS_OK;
  }

  [[nodiscard]] xnn_transfer_status ResolveActiveTrust(
      const std::uint64_t trust_id,
      core::security::identity::DeviceId* const out_device_id) const {
    if (trust_id == 0 || out_device_id == nullptr) {
      return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
    }

    const std::scoped_lock lock(mutex_);
    const TrustEntry* const entry = FindTrust(trust_id);
    if (entry == nullptr || entry->state != XNN_TRANSFER_TRUST_STATE_ACTIVE) {
      return XNN_TRANSFER_STATUS_STALE_HANDLE;
    }
    *out_device_id = entry->device_id;
    return XNN_TRANSFER_STATUS_OK;
  }

  /*
   * The native network/session owner calls Apply after serializing one XT-025
   * update. attempt is cached before XT-025 invalidates its terminal handle.
   */
  [[nodiscard]] bool Apply(const std::uint64_t peer_id,
                           const core::session::AttemptHandle& attempt,
                           const core::session::PairingUpdate& update,
                           EventChannel& events) {
    if (AllZero(attempt)) {
      return false;
    }

    std::optional<xnn_transfer_pairing_attempt_event_payload> attempt_event;
    std::optional<xnn_transfer_trust_event_payload> trust_event;
    {
      const std::scoped_lock lock(mutex_);
      if (!updates_open_ ||
          (active_attempt_.has_value() &&
           !std::equal(std::begin(active_attempt_->attempt_id),
                       std::end(active_attempt_->attempt_id), attempt.begin()))) {
        return false;
      }

      if (update.prompt.has_value()) {
        if (update.prompt->handle != attempt) {
          return false;
        }
        active_attempt_ = MakeAttemptPayload(
            XNN_TRANSFER_PAIRING_ATTEMPT_AWAITING_CONFIRMATION, peer_id,
            update.prompt->deadline_ms, attempt, update.prompt->sas_word_indices,
            XNN_TRANSFER_PAIRING_ERROR_NONE);
        AdvanceRevision(pairing_revision_);
        attempt_event = *active_attempt_;
      } else if (update.terminal) {
        const std::uint32_t state =
            update.state == core::session::PairingState::kPairedLocal
                ? XNN_TRANSFER_PAIRING_ATTEMPT_PAIRED
                : XNN_TRANSFER_PAIRING_ATTEMPT_CLOSED;
        attempt_event = MakeAttemptPayload(state, peer_id, 0, attempt, {},
                                           PublicError(update.error));
        active_attempt_.reset();
        AdvanceRevision(pairing_revision_);

        if (update.paired_peer.has_value()) {
          TrustEntry* entry = FindTrust(*update.paired_peer);
          if (entry == nullptr) {
            if (trust_entries_.size() >= core::security::identity::kMaxPeerRecords ||
                next_trust_id_ == 0) {
              return false;
            }
            trust_entries_.push_back(TrustEntry{
                .trust_id = next_trust_id_++,
                .peer_id = peer_id,
                .device_id = *update.paired_peer,
                .state = XNN_TRANSFER_TRUST_STATE_ACTIVE,
            });
            entry = &trust_entries_.back();
          } else {
            entry->peer_id = peer_id;
            entry->state = XNN_TRANSFER_TRUST_STATE_ACTIVE;
          }
          AdvanceRevision(trust_revision_);
          trust_event = MakeTrustPayload(*entry);
        }
      } else if (!active_attempt_.has_value()) {
        active_attempt_ =
            MakeAttemptPayload(XNN_TRANSFER_PAIRING_ATTEMPT_STARTING, peer_id, 0,
                               attempt, {}, XNN_TRANSFER_PAIRING_ERROR_NONE);
        AdvanceRevision(pairing_revision_);
        attempt_event = *active_attempt_;
      }
    }

    {
      const std::scoped_lock lock(mutex_);
      if (!updates_open_) {
        return false;
      }
    }
    if (attempt_event.has_value()) {
      events.EnqueuePairing(*attempt_event);
    }
    if (trust_event.has_value()) {
      events.EnqueueTrust(*trust_event);
    }
    return true;
  }

  [[nodiscard]] xnn_transfer_status Snapshot(
      xnn_transfer_pairing_snapshot* const out_snapshot) const {
    if (out_snapshot == nullptr) {
      return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
    }

    const std::scoped_lock lock(mutex_);
    xnn_transfer_pairing_snapshot output{
        .struct_size = sizeof(xnn_transfer_pairing_snapshot),
        .abi_version = XNN_TRANSFER_ABI_VERSION,
        .reserved = 0,
        .snapshot_revision = pairing_revision_,
        .has_attempt = active_attempt_.has_value() ? 1U : 0U,
        .reserved2 = 0,
        .attempt = {},
    };
    if (active_attempt_.has_value()) {
      output.attempt = *active_attempt_;
    }
    *out_snapshot = output;
    return XNN_TRANSFER_STATUS_OK;
  }

  [[nodiscard]] xnn_transfer_status TrustSnapshot(
      const std::uint64_t expected_revision, const std::uint32_t offset,
      xnn_transfer_trust_snapshot_page* const out_page) const {
    if (out_page == nullptr) {
      return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
    }

    const std::scoped_lock lock(mutex_);
    if (expected_revision == 0 && offset != 0) {
      return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
    }
    if (expected_revision != 0 && expected_revision != trust_revision_) {
      return XNN_TRANSFER_STATUS_STALE_SNAPSHOT;
    }
    if (offset > trust_entries_.size()) {
      return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
    }

    xnn_transfer_trust_snapshot_page output{
        .struct_size = sizeof(xnn_transfer_trust_snapshot_page),
        .abi_version = XNN_TRANSFER_ABI_VERSION,
        .reserved = 0,
        .snapshot_revision = trust_revision_,
        .offset = offset,
        .count = 0,
        .total_count = static_cast<std::uint32_t>(trust_entries_.size()),
        .reserved2 = 0,
        .records = {},
    };
    std::size_t index = offset;
    while (index < trust_entries_.size() &&
           output.count < XNN_TRANSFER_TRUST_SNAPSHOT_PAGE_CAPACITY) {
      output.records[output.count] = MakeTrustPayload(trust_entries_[index]);
      ++output.count;
      ++index;
    }
    *out_page = output;
    return XNN_TRANSFER_STATUS_OK;
  }

  void Shutdown(EventChannel& events) {
    {
      const std::scoped_lock lock(mutex_);
      updates_open_ = false;
    }
    backend_->Shutdown();
    const std::optional<xnn_transfer_pairing_attempt_event_payload> closed =
        ClearActive(XNN_TRANSFER_PAIRING_ERROR_CANCELLED);
    if (closed.has_value()) {
      events.EnqueuePairing(*closed);
    }
  }

 private:
  struct TrustEntry {
    std::uint64_t trust_id{};
    std::uint64_t peer_id{};
    core::security::identity::DeviceId device_id{};
    std::uint32_t state{XNN_TRANSFER_TRUST_STATE_ACTIVE};
  };

  static bool AllZero(const core::session::AttemptHandle& attempt) noexcept {
    return std::all_of(attempt.begin(), attempt.end(),
                       [](const std::uint8_t value) { return value == 0; });
  }

  static std::uint16_t PublicError(const core::session::PairingError error) noexcept {
    switch (error) {
      case core::session::PairingError::kNone:
        return XNN_TRANSFER_PAIRING_ERROR_NONE;
      case core::session::PairingError::kAuthenticatedReject:
      case core::session::PairingError::kLocalReject:
        return XNN_TRANSFER_PAIRING_ERROR_REJECTED;
      case core::session::PairingError::kCancelled:
        return XNN_TRANSFER_PAIRING_ERROR_CANCELLED;
      case core::session::PairingError::kTimeout:
        return XNN_TRANSFER_PAIRING_ERROR_TIMED_OUT;
      case core::session::PairingError::kBusy:
        return XNN_TRANSFER_PAIRING_ERROR_BUSY;
      case core::session::PairingError::kMalformed:
      case core::session::PairingError::kLimitExceeded:
      case core::session::PairingError::kSequenceViolation:
      case core::session::PairingError::kUnsupportedVersion:
      case core::session::PairingError::kUnsupportedProfile:
      case core::session::PairingError::kDowngradeDetected:
      case core::session::PairingError::kRoleMismatch:
      case core::session::PairingError::kStateViolation:
      case core::session::PairingError::kReplayDetected:
      case core::session::PairingError::kConfirmationFailed:
      case core::session::PairingError::kAlreadyDecided:
      case core::session::PairingError::kCertificateRejected:
      case core::session::PairingError::kInternalFailure:
        return XNN_TRANSFER_PAIRING_ERROR_FAILED;
    }
    return XNN_TRANSFER_PAIRING_ERROR_FAILED;
  }

  static xnn_transfer_pairing_attempt_event_payload MakeAttemptPayload(
      const std::uint32_t state, const std::uint64_t peer_id,
      const std::uint64_t deadline_ms, const core::session::AttemptHandle& attempt,
      const std::array<std::uint16_t, XNN_TRANSFER_PAIRING_SAS_WORD_COUNT>& sas_words,
      const std::uint16_t error) {
    xnn_transfer_pairing_attempt_event_payload payload{
        .struct_size = sizeof(xnn_transfer_pairing_attempt_event_payload),
        .abi_version = XNN_TRANSFER_ABI_VERSION,
        .state = state,
        .peer_id = peer_id,
        .deadline_ms = deadline_ms,
        .attempt_id = {},
        .sas_word_indices = {},
        .sas_word_count = static_cast<std::uint16_t>(
            state == XNN_TRANSFER_PAIRING_ATTEMPT_AWAITING_CONFIRMATION
                ? XNN_TRANSFER_PAIRING_SAS_WORD_COUNT
                : 0U),
        .error = error,
        .reserved = 0,
    };
    std::copy(attempt.begin(), attempt.end(), payload.attempt_id);
    std::copy(sas_words.begin(), sas_words.end(), payload.sas_word_indices);
    return payload;
  }

  static xnn_transfer_trust_event_payload MakeTrustPayload(const TrustEntry& entry) {
    return xnn_transfer_trust_event_payload{
        .struct_size = sizeof(xnn_transfer_trust_event_payload),
        .abi_version = XNN_TRANSFER_ABI_VERSION,
        .state = entry.state,
        .trust_id = entry.trust_id,
        .peer_id = entry.peer_id,
        .reserved = 0,
        .reserved2 = 0,
    };
  }

  static void AdvanceRevision(std::uint64_t& revision) noexcept {
    revision = revision == std::numeric_limits<std::uint64_t>::max() ? 1 : revision + 1;
  }

  [[nodiscard]] std::optional<xnn_transfer_pairing_attempt_event_payload> ClearActive(
      const std::uint16_t error) {
    const std::scoped_lock lock(mutex_);
    if (!active_attempt_.has_value()) {
      return std::nullopt;
    }
    xnn_transfer_pairing_attempt_event_payload closed = *active_attempt_;
    closed.state = XNN_TRANSFER_PAIRING_ATTEMPT_CLOSED;
    closed.deadline_ms = 0;
    std::fill(std::begin(closed.sas_word_indices), std::end(closed.sas_word_indices),
              0);
    closed.sas_word_count = 0;
    closed.error = error;
    active_attempt_.reset();
    AdvanceRevision(pairing_revision_);
    return closed;
  }

  [[nodiscard]] TrustEntry* FindTrust(const std::uint64_t trust_id) noexcept {
    const auto entry = std::find_if(trust_entries_.begin(), trust_entries_.end(),
                                    [trust_id](const TrustEntry& candidate) {
                                      return candidate.trust_id == trust_id;
                                    });
    return entry == trust_entries_.end() ? nullptr : &*entry;
  }

  [[nodiscard]] const TrustEntry* FindTrust(
      const std::uint64_t trust_id) const noexcept {
    const auto entry = std::find_if(trust_entries_.cbegin(), trust_entries_.cend(),
                                    [trust_id](const TrustEntry& candidate) {
                                      return candidate.trust_id == trust_id;
                                    });
    return entry == trust_entries_.cend() ? nullptr : &*entry;
  }

  [[nodiscard]] TrustEntry* FindTrust(
      const core::security::identity::DeviceId& device_id) noexcept {
    const auto entry = std::find_if(trust_entries_.begin(), trust_entries_.end(),
                                    [&device_id](const TrustEntry& candidate) {
                                      return candidate.device_id == device_id;
                                    });
    return entry == trust_entries_.end() ? nullptr : &*entry;
  }

  PairingBackend* backend_;
  mutable std::mutex mutex_;
  std::optional<xnn_transfer_pairing_attempt_event_payload> active_attempt_;
  std::vector<TrustEntry> trust_entries_;
  std::uint64_t pairing_revision_{1};
  std::uint64_t trust_revision_{1};
  std::uint64_t next_trust_id_{1};
  bool updates_open_{true};
};

}  // namespace xnn_transfer::bridge

#endif  // XNN_TRANSFER_BRIDGE_PAIRING_BRIDGE_HPP_
