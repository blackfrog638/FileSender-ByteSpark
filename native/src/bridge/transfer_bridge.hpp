#ifndef XNN_TRANSFER_BRIDGE_TRANSFER_BRIDGE_HPP_
#define XNN_TRANSFER_BRIDGE_TRANSFER_BRIDGE_HPP_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "event_channel.hpp"
#include "xnn_transfer/c_api.h"
#include "xnn_transfer/core/discovery/discovery.hpp"
#include "xnn_transfer/core/security/identity/types.hpp"
#include "xnn_transfer/core/storage/storage.hpp"
#include "xnn_transfer/core/transfer/transfer.hpp"

namespace xnn_transfer::bridge {

inline constexpr std::size_t kMaximumTransferRecords = 256;

struct TransferStartResult {
  xnn_transfer_status status{XNN_TRANSFER_STATUS_INTERNAL_ERROR};
  core::transfer::TransferId transfer_id{};
  std::uint64_t total_bytes{};
  std::string peer_label{};
};

class TransferBackend {
 public:
  virtual ~TransferBackend() = default;

  [[nodiscard]] virtual TransferStartResult Send(
      const core::security::identity::DeviceId& device_id,
      std::span<const std::uint8_t> path, std::uint64_t now_ms) = 0;
  [[nodiscard]] virtual xnn_transfer_status Accept(
      const core::transfer::TransferId& transfer_id, std::uint64_t now_ms) = 0;
  [[nodiscard]] virtual xnn_transfer_status Reject(
      const core::transfer::TransferId& transfer_id, std::uint64_t now_ms) = 0;
  [[nodiscard]] virtual xnn_transfer_status Cancel(
      const core::transfer::TransferId& transfer_id, std::uint64_t now_ms) = 0;
  virtual void Shutdown() = 0;
};

class TransferBridge final {
 public:
  explicit TransferBridge(TransferBackend& backend) : backend_(&backend) {}

  TransferBridge(const TransferBridge&) = delete;
  TransferBridge& operator=(const TransferBridge&) = delete;

  [[nodiscard]] xnn_transfer_status Send(
      const core::security::identity::DeviceId& device_id,
      const std::span<const std::uint8_t> path, const std::uint64_t now_ms,
      xnn_transfer_transfer_ref* const out_transfer, EventChannel& events) {
    if (out_transfer == nullptr || path.empty() ||
        path.size() > XNN_TRANSFER_TRANSFER_PATH_MAX_SIZE ||
        std::find(path.begin(), path.end(), std::uint8_t{0}) != path.end()) {
      return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
    }

    std::unique_lock backend_lock(backend_mutex_);
    {
      const std::scoped_lock lock(mutex_);
      if (!updates_open_) {
        return XNN_TRANSFER_STATUS_INVALID_STATE;
      }
      if (records_.size() + pending_starts_ >= kMaximumTransferRecords) {
        return XNN_TRANSFER_STATUS_UNAVAILABLE;
      }
      ++pending_starts_;
    }

    TransferStartResult result = backend_->Send(device_id, path, now_ms);
    const std::scoped_lock mutation_lock(mutation_mutex_);
    xnn_transfer_transfer_event_payload event{};
    bool invalid_result = false;
    {
      const std::scoped_lock lock(mutex_);
      --pending_starts_;
      if (!updates_open_) {
        return XNN_TRANSFER_STATUS_INVALID_STATE;
      }
      if (result.status != XNN_TRANSFER_STATUS_OK) {
        return result.status;
      }
      if (AllZero(result.transfer_id) ||
          result.total_bytes > core::storage::kMaxFileBytes ||
          !ValidPeerLabel(result.peer_label) ||
          Find(result.transfer_id) != records_.end() ||
          records_.size() >= kMaximumTransferRecords) {
        invalid_result = true;
      } else {
        Record record{
            .transfer_id = result.transfer_id,
            .direction = XNN_TRANSFER_TRANSFER_DIRECTION_OUTGOING,
            .state = XNN_TRANSFER_TRANSFER_STATE_QUEUED,
            .error = XNN_TRANSFER_TRANSFER_ERROR_NONE,
            .total_bytes = result.total_bytes,
            .transferred_bytes = 0,
            .peer_label = std::move(result.peer_label),
        };
        records_.push_back(std::move(record));
        AdvanceRevision();
        event = MakePayload(records_.back(), XNN_TRANSFER_TRANSFER_UPSERTED);
        *out_transfer = MakeRef(result.transfer_id);
      }
    }
    if (invalid_result) {
      static_cast<void>(backend_->Cancel(result.transfer_id, now_ms));
      return XNN_TRANSFER_STATUS_INTERNAL_ERROR;
    }
    backend_lock.unlock();

    if (!PublishIfOpen(event, events)) {
      return XNN_TRANSFER_STATUS_INVALID_STATE;
    }
    return XNN_TRANSFER_STATUS_OK;
  }

  [[nodiscard]] xnn_transfer_status Accept(
      const core::transfer::TransferId& transfer_id, const std::uint64_t now_ms,
      EventChannel& events) {
    std::unique_lock backend_lock(backend_mutex_);
    {
      const std::scoped_lock lock(mutex_);
      const auto record = Find(transfer_id);
      if (!updates_open_) {
        return XNN_TRANSFER_STATUS_INVALID_STATE;
      }
      if (record == records_.end() ||
          record->state != XNN_TRANSFER_TRANSFER_STATE_OFFERED) {
        return XNN_TRANSFER_STATUS_STALE_HANDLE;
      }
    }

    const xnn_transfer_status status = backend_->Accept(transfer_id, now_ms);
    if (status != XNN_TRANSFER_STATUS_OK) {
      return status;
    }

    const std::scoped_lock mutation_lock(mutation_mutex_);
    xnn_transfer_transfer_event_payload event{};
    {
      const std::scoped_lock lock(mutex_);
      const auto record = Find(transfer_id);
      if (!updates_open_) {
        return XNN_TRANSFER_STATUS_INVALID_STATE;
      }
      if (record == records_.end() ||
          record->state != XNN_TRANSFER_TRANSFER_STATE_OFFERED) {
        return XNN_TRANSFER_STATUS_STALE_HANDLE;
      }
      record->state = XNN_TRANSFER_TRANSFER_STATE_QUEUED;
      AdvanceRevision();
      event = MakePayload(*record, XNN_TRANSFER_TRANSFER_UPSERTED);
    }
    backend_lock.unlock();
    return PublishIfOpen(event, events) ? XNN_TRANSFER_STATUS_OK
                                        : XNN_TRANSFER_STATUS_INVALID_STATE;
  }

  [[nodiscard]] xnn_transfer_status Reject(
      const core::transfer::TransferId& transfer_id, const std::uint64_t now_ms,
      EventChannel& events) {
    std::unique_lock backend_lock(backend_mutex_);
    {
      const std::scoped_lock lock(mutex_);
      const auto record = Find(transfer_id);
      if (!updates_open_) {
        return XNN_TRANSFER_STATUS_INVALID_STATE;
      }
      if (record == records_.end() ||
          record->state != XNN_TRANSFER_TRANSFER_STATE_OFFERED) {
        return XNN_TRANSFER_STATUS_STALE_HANDLE;
      }
    }

    const xnn_transfer_status status = backend_->Reject(transfer_id, now_ms);
    if (status != XNN_TRANSFER_STATUS_OK) {
      return status;
    }

    const std::scoped_lock mutation_lock(mutation_mutex_);
    xnn_transfer_transfer_event_payload event{};
    {
      const std::scoped_lock lock(mutex_);
      const auto record = Find(transfer_id);
      if (!updates_open_) {
        return XNN_TRANSFER_STATUS_INVALID_STATE;
      }
      if (record == records_.end() ||
          record->state != XNN_TRANSFER_TRANSFER_STATE_OFFERED) {
        return XNN_TRANSFER_STATUS_STALE_HANDLE;
      }
      AdvanceRevision();
      event = MakePayload(*record, XNN_TRANSFER_TRANSFER_REMOVED);
      records_.erase(record);
    }
    backend_lock.unlock();
    return PublishIfOpen(event, events) ? XNN_TRANSFER_STATUS_OK
                                        : XNN_TRANSFER_STATUS_INVALID_STATE;
  }

  [[nodiscard]] xnn_transfer_status Cancel(
      const core::transfer::TransferId& transfer_id, const std::uint64_t now_ms,
      EventChannel& events) {
    std::unique_lock backend_lock(backend_mutex_);
    {
      const std::scoped_lock lock(mutex_);
      const auto record = Find(transfer_id);
      if (!updates_open_) {
        return XNN_TRANSFER_STATUS_INVALID_STATE;
      }
      if (record == records_.end() ||
          record->state == XNN_TRANSFER_TRANSFER_STATE_OFFERED ||
          Terminal(record->state)) {
        return XNN_TRANSFER_STATUS_STALE_HANDLE;
      }
      if (record->state == XNN_TRANSFER_TRANSFER_STATE_CANCELLING) {
        return XNN_TRANSFER_STATUS_OK;
      }
    }

    const xnn_transfer_status status = backend_->Cancel(transfer_id, now_ms);
    if (status != XNN_TRANSFER_STATUS_OK) {
      return status;
    }

    const std::scoped_lock mutation_lock(mutation_mutex_);
    xnn_transfer_transfer_event_payload event{};
    {
      const std::scoped_lock lock(mutex_);
      const auto record = Find(transfer_id);
      if (!updates_open_) {
        return XNN_TRANSFER_STATUS_INVALID_STATE;
      }
      if (record == records_.end() ||
          record->state == XNN_TRANSFER_TRANSFER_STATE_OFFERED ||
          Terminal(record->state)) {
        return XNN_TRANSFER_STATUS_STALE_HANDLE;
      }
      if (record->state == XNN_TRANSFER_TRANSFER_STATE_CANCELLING) {
        return XNN_TRANSFER_STATUS_OK;
      }
      record->state = XNN_TRANSFER_TRANSFER_STATE_CANCELLING;
      AdvanceRevision();
      event = MakePayload(*record, XNN_TRANSFER_TRANSFER_UPSERTED);
    }
    backend_lock.unlock();
    return PublishIfOpen(event, events) ? XNN_TRANSFER_STATUS_OK
                                        : XNN_TRANSFER_STATUS_INVALID_STATE;
  }

  /*
   * The native session/network owner calls ApplyIncomingOffer only after
   * authentication and the transfer core has validated the complete one-file
   * manifest. No peer path or protocol bytes cross the ABI.
   */
  [[nodiscard]] bool ApplyIncomingOffer(const std::string_view peer_label,
                                        const core::transfer::IncomingOffer& offer,
                                        EventChannel& events) {
    if (AllZero(offer.transfer_id) || offer.file_size > core::storage::kMaxFileBytes ||
        !ValidPeerLabel(peer_label)) {
      return false;
    }

    const std::scoped_lock mutation_lock(mutation_mutex_);
    xnn_transfer_transfer_event_payload event{};
    {
      const std::scoped_lock lock(mutex_);
      if (!updates_open_) {
        return false;
      }
      const auto existing = Find(offer.transfer_id);
      if (existing != records_.end()) {
        return existing->state == XNN_TRANSFER_TRANSFER_STATE_OFFERED &&
               existing->total_bytes == offer.file_size &&
               existing->peer_label == peer_label;
      }
      if (records_.size() + pending_starts_ >= kMaximumTransferRecords) {
        return false;
      }

      records_.push_back(Record{
          .transfer_id = offer.transfer_id,
          .direction = XNN_TRANSFER_TRANSFER_DIRECTION_INCOMING,
          .state = XNN_TRANSFER_TRANSFER_STATE_OFFERED,
          .error = XNN_TRANSFER_TRANSFER_ERROR_NONE,
          .total_bytes = offer.file_size,
          .transferred_bytes = 0,
          .peer_label = std::string(peer_label),
      });
      AdvanceRevision();
      event = MakePayload(records_.back(), XNN_TRANSFER_TRANSFER_UPSERTED);
    }
    return PublishIfOpen(event, events);
  }

  /*
   * transferred_bytes is measured by the native transport adapter after the
   * corresponding frame or durable write has completed. It is never accepted
   * from presentation.
   */
  [[nodiscard]] bool Apply(const core::transfer::TransferId& transfer_id,
                           const core::transfer::TransferUpdate& update,
                           const std::uint64_t transferred_bytes,
                           EventChannel& events) {
    const std::scoped_lock mutation_lock(mutation_mutex_);
    xnn_transfer_transfer_event_payload event{};
    {
      const std::scoped_lock lock(mutex_);
      if (!updates_open_) {
        return false;
      }
      const auto record = Find(transfer_id);
      if (record == records_.end() ||
          record->state == XNN_TRANSFER_TRANSFER_STATE_OFFERED ||
          Terminal(record->state) || transferred_bytes < record->transferred_bytes ||
          transferred_bytes > record->total_bytes) {
        return false;
      }

      const std::uint32_t state = PublicState(update.state);
      if (state == XNN_TRANSFER_TRANSFER_STATE_OFFERED ||
          (state == XNN_TRANSFER_TRANSFER_STATE_COMPLETED &&
           transferred_bytes != record->total_bytes) ||
          (!Terminal(state) && update.terminal) ||
          (Terminal(state) && !update.terminal)) {
        return false;
      }
      const std::uint32_t error = PublicError(update.error, state);
      const bool failure_state = state == XNN_TRANSFER_TRANSFER_STATE_FAILED ||
                                 state == XNN_TRANSFER_TRANSFER_STATE_REJECTED;
      if ((failure_state && error == XNN_TRANSFER_TRANSFER_ERROR_NONE) ||
          (!failure_state && state != XNN_TRANSFER_TRANSFER_STATE_CANCELLED &&
           error != XNN_TRANSFER_TRANSFER_ERROR_NONE)) {
        return false;
      }

      record->state = state;
      record->error = error;
      record->transferred_bytes = transferred_bytes;
      AdvanceRevision();
      event = MakePayload(*record, XNN_TRANSFER_TRANSFER_UPSERTED);
    }
    return PublishIfOpen(event, events);
  }

  [[nodiscard]] xnn_transfer_status Snapshot(
      const std::uint64_t expected_revision, const std::uint32_t offset,
      xnn_transfer_transfer_snapshot_page* const out_page) const {
    if (out_page == nullptr) {
      return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
    }

    const std::scoped_lock lock(mutex_);
    if (expected_revision == 0 && offset != 0) {
      return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
    }
    if (expected_revision != 0 && expected_revision != revision_) {
      return XNN_TRANSFER_STATUS_STALE_SNAPSHOT;
    }
    if (offset > records_.size()) {
      return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
    }

    xnn_transfer_transfer_snapshot_page output{
        .struct_size = sizeof(xnn_transfer_transfer_snapshot_page),
        .abi_version = XNN_TRANSFER_ABI_VERSION,
        .reserved = 0,
        .snapshot_revision = revision_,
        .offset = offset,
        .count = 0,
        .total_count = static_cast<std::uint32_t>(records_.size()),
        .reserved2 = 0,
        .records = {},
    };
    std::size_t index = offset;
    while (index < records_.size() &&
           output.count < XNN_TRANSFER_TRANSFER_SNAPSHOT_PAGE_CAPACITY) {
      output.records[output.count] =
          MakePayload(records_[index], XNN_TRANSFER_TRANSFER_UPSERTED);
      ++output.count;
      ++index;
    }
    *out_page = output;
    return XNN_TRANSFER_STATUS_OK;
  }

  void Shutdown(EventChannel& events) {
    {
      const std::scoped_lock backend_lock(backend_mutex_);
      {
        const std::scoped_lock mutation_lock(mutation_mutex_);
        const std::scoped_lock lock(mutex_);
        if (!updates_open_) {
          return;
        }
        updates_open_ = false;
      }
      backend_->Shutdown();
    }

    const std::scoped_lock mutation_lock(mutation_mutex_);
    std::vector<xnn_transfer_transfer_event_payload> terminal_events;
    {
      const std::scoped_lock lock(mutex_);
      auto record = records_.begin();
      while (record != records_.end()) {
        if (record->state == XNN_TRANSFER_TRANSFER_STATE_OFFERED) {
          AdvanceRevision();
          terminal_events.push_back(
              MakePayload(*record, XNN_TRANSFER_TRANSFER_REMOVED));
          record = records_.erase(record);
          continue;
        }
        if (!Terminal(record->state)) {
          record->state = XNN_TRANSFER_TRANSFER_STATE_CANCELLED;
          record->error = XNN_TRANSFER_TRANSFER_ERROR_CANCELLED;
          AdvanceRevision();
          terminal_events.push_back(
              MakePayload(*record, XNN_TRANSFER_TRANSFER_UPSERTED));
        }
        ++record;
      }
    }
    for (const xnn_transfer_transfer_event_payload& event : terminal_events) {
      events.EnqueueTransfer(event);
    }
  }

 private:
  struct Record {
    core::transfer::TransferId transfer_id{};
    std::uint32_t direction{};
    std::uint32_t state{};
    std::uint32_t error{};
    std::uint64_t total_bytes{};
    std::uint64_t transferred_bytes{};
    std::string peer_label{};
  };

  using RecordIterator = std::vector<Record>::iterator;
  using ConstRecordIterator = std::vector<Record>::const_iterator;

  [[nodiscard]] RecordIterator Find(const core::transfer::TransferId& transfer_id) {
    return std::find_if(records_.begin(), records_.end(),
                        [&transfer_id](const Record& record) {
                          return record.transfer_id == transfer_id;
                        });
  }

  [[nodiscard]] ConstRecordIterator Find(
      const core::transfer::TransferId& transfer_id) const {
    return std::find_if(records_.cbegin(), records_.cend(),
                        [&transfer_id](const Record& record) {
                          return record.transfer_id == transfer_id;
                        });
  }

  static bool AllZero(const core::transfer::TransferId& transfer_id) noexcept {
    return std::all_of(transfer_id.begin(), transfer_id.end(),
                       [](const std::uint8_t value) { return value == 0; });
  }

  static bool ValidPeerLabel(const std::string_view label) {
    if (label.empty()) {
      return true;
    }
    if (label.size() > XNN_TRANSFER_TRANSFER_PEER_LABEL_MAX_SIZE) {
      return false;
    }
    const std::shared_ptr<const core::discovery::DisplayLabelValidator> validator =
        core::discovery::MakeUtf8procDisplayLabelValidator();
    return validator != nullptr &&
           validator->IsCanonical(std::span<const std::uint8_t>(
               reinterpret_cast<const std::uint8_t*>(label.data()), label.size()));
  }

  static bool Terminal(const std::uint32_t state) noexcept {
    return state == XNN_TRANSFER_TRANSFER_STATE_COMPLETED ||
           state == XNN_TRANSFER_TRANSFER_STATE_CANCELLED ||
           state == XNN_TRANSFER_TRANSFER_STATE_REJECTED ||
           state == XNN_TRANSFER_TRANSFER_STATE_FAILED;
  }

  static std::uint32_t PublicState(const core::transfer::TransferState state) noexcept {
    switch (state) {
      case core::transfer::TransferState::kCreated:
        return XNN_TRANSFER_TRANSFER_STATE_QUEUED;
      case core::transfer::TransferState::kSendingManifest:
      case core::transfer::TransferState::kReceivingManifest:
      case core::transfer::TransferState::kAwaitingDecision:
      case core::transfer::TransferState::kSendingFile:
      case core::transfer::TransferState::kReceivingFile:
      case core::transfer::TransferState::kRejecting:
      case core::transfer::TransferState::kAwaitingFileCommit:
      case core::transfer::TransferState::kAwaitingCompletion:
      case core::transfer::TransferState::kCompleting:
        return XNN_TRANSFER_TRANSFER_STATE_RUNNING;
      case core::transfer::TransferState::kCancelling:
        return XNN_TRANSFER_TRANSFER_STATE_CANCELLING;
      case core::transfer::TransferState::kCommitted:
      case core::transfer::TransferState::kCompleted:
        return XNN_TRANSFER_TRANSFER_STATE_COMPLETED;
      case core::transfer::TransferState::kCancelled:
        return XNN_TRANSFER_TRANSFER_STATE_CANCELLED;
      case core::transfer::TransferState::kRejected:
        return XNN_TRANSFER_TRANSFER_STATE_REJECTED;
      case core::transfer::TransferState::kFailed:
        return XNN_TRANSFER_TRANSFER_STATE_FAILED;
    }
    return XNN_TRANSFER_TRANSFER_STATE_FAILED;
  }

  static std::uint32_t PublicError(const core::transfer::TransferError error,
                                   const std::uint32_t public_state) noexcept {
    if (public_state == XNN_TRANSFER_TRANSFER_STATE_COMPLETED) {
      return XNN_TRANSFER_TRANSFER_ERROR_NONE;
    }
    if (public_state == XNN_TRANSFER_TRANSFER_STATE_CANCELLED) {
      return XNN_TRANSFER_TRANSFER_ERROR_CANCELLED;
    }
    if (public_state == XNN_TRANSFER_TRANSFER_STATE_REJECTED) {
      return XNN_TRANSFER_TRANSFER_ERROR_REJECTED;
    }

    switch (error) {
      case core::transfer::TransferError::kNone:
        return public_state == XNN_TRANSFER_TRANSFER_STATE_FAILED
                   ? XNN_TRANSFER_TRANSFER_ERROR_FAILED
                   : XNN_TRANSFER_TRANSFER_ERROR_NONE;
      case core::transfer::TransferError::kPolicyRejected:
        return XNN_TRANSFER_TRANSFER_ERROR_POLICY_REJECTED;
      case core::transfer::TransferError::kNoSpace:
        return XNN_TRANSFER_TRANSFER_ERROR_NO_SPACE;
      case core::transfer::TransferError::kBusy:
        return XNN_TRANSFER_TRANSFER_ERROR_BUSY;
      case core::transfer::TransferError::kIoFailure:
      case core::transfer::TransferError::kSourceFailure:
        return XNN_TRANSFER_TRANSFER_ERROR_IO_FAILURE;
      case core::transfer::TransferError::kIntegrityFailed:
        return XNN_TRANSFER_TRANSFER_ERROR_INTEGRITY_FAILED;
      case core::transfer::TransferError::kTimeout:
        return XNN_TRANSFER_TRANSFER_ERROR_TIMED_OUT;
      case core::transfer::TransferError::kCancelled:
        return XNN_TRANSFER_TRANSFER_ERROR_CANCELLED;
      case core::transfer::TransferError::kInvalidArgument:
      case core::transfer::TransferError::kUnauthenticated:
      case core::transfer::TransferError::kMalformedFrame:
      case core::transfer::TransferError::kMalformedMessage:
      case core::transfer::TransferError::kStateViolation:
      case core::transfer::TransferError::kMessageIdViolation:
      case core::transfer::TransferError::kLimitExceeded:
      case core::transfer::TransferError::kInvalidOffer:
      case core::transfer::TransferError::kInvalidManifest:
      case core::transfer::TransferError::kIdempotencyConflict:
      case core::transfer::TransferError::kInternalFailure:
        return XNN_TRANSFER_TRANSFER_ERROR_FAILED;
    }
    return XNN_TRANSFER_TRANSFER_ERROR_FAILED;
  }

  [[nodiscard]] static xnn_transfer_transfer_ref MakeRef(
      const core::transfer::TransferId& transfer_id) {
    xnn_transfer_transfer_ref output{
        .struct_size = sizeof(xnn_transfer_transfer_ref),
        .abi_version = XNN_TRANSFER_ABI_VERSION,
        .reserved = 0,
        .transfer_id = {},
    };
    std::copy(transfer_id.begin(), transfer_id.end(), output.transfer_id);
    return output;
  }

  [[nodiscard]] xnn_transfer_transfer_event_payload MakePayload(
      const Record& record, const std::uint32_t change) const {
    xnn_transfer_transfer_event_payload output{
        .struct_size = sizeof(xnn_transfer_transfer_event_payload),
        .abi_version = XNN_TRANSFER_ABI_VERSION,
        .change = change,
        .snapshot_revision = revision_,
        .direction = record.direction,
        .state = record.state,
        .error = record.error,
        .peer_label_size = static_cast<std::uint32_t>(record.peer_label.size()),
        .reserved = 0,
        .reserved2 = 0,
        .total_bytes = record.total_bytes,
        .transferred_bytes = record.transferred_bytes,
        .transfer_id = {},
        .peer_label = {},
    };
    std::copy(record.transfer_id.begin(), record.transfer_id.end(), output.transfer_id);
    std::memcpy(output.peer_label, record.peer_label.data(), record.peer_label.size());
    return output;
  }

  [[nodiscard]] bool PublishIfOpen(const xnn_transfer_transfer_event_payload& event,
                                   EventChannel& events) const {
    {
      const std::scoped_lock lock(mutex_);
      if (!updates_open_) {
        return false;
      }
    }
    events.EnqueueTransfer(event);
    return true;
  }

  void AdvanceRevision() noexcept {
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
      revision_ = 1;
    } else {
      ++revision_;
    }
  }

  TransferBackend* backend_{};
  std::mutex backend_mutex_;
  std::mutex mutation_mutex_;
  mutable std::mutex mutex_;
  std::vector<Record> records_;
  std::uint64_t revision_{1};
  std::size_t pending_starts_{};
  bool updates_open_{true};
};

}  // namespace xnn_transfer::bridge

#endif  // XNN_TRANSFER_BRIDGE_TRANSFER_BRIDGE_HPP_
