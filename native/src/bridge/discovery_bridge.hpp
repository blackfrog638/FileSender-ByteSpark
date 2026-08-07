#ifndef XNN_TRANSFER_BRIDGE_DISCOVERY_BRIDGE_HPP_
#define XNN_TRANSFER_BRIDGE_DISCOVERY_BRIDGE_HPP_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>

#include "xnn_transfer/c_api.h"
#include "xnn_transfer/core/discovery/discovery.hpp"

namespace xnn_transfer::bridge {

class DiscoveryPeerRegistry final {
 public:
  DiscoveryPeerRegistry() = default;

  DiscoveryPeerRegistry(const DiscoveryPeerRegistry&) = delete;
  DiscoveryPeerRegistry& operator=(const DiscoveryPeerRegistry&) = delete;

  [[nodiscard]] std::optional<xnn_transfer_discovery_peer_event_payload> Apply(
      const core::discovery::CandidateEvent& event) {
    const std::scoped_lock lock(mutex_);
    auto existing = peers_.find(event.candidate.key);

    if (event.kind == core::discovery::EventKind::kExpired) {
      if (existing == peers_.end()) {
        return std::nullopt;
      }
      const std::uint64_t peer_id = existing->second.peer_id;
      const core::discovery::Candidate candidate = existing->second.candidate;
      peers_.erase(existing);
      AdvanceRevision();
      return MakeEvent(XNN_TRANSFER_DISCOVERY_PEER_EXPIRED,
                       ExpiryReason(event.expiry_reason), peer_id, candidate);
    }

    std::uint32_t change = XNN_TRANSFER_DISCOVERY_PEER_UPDATED;
    if (existing == peers_.end()) {
      if (peers_.size() >= core::discovery::kMaxCandidates || next_peer_id_ == 0) {
        return std::nullopt;
      }
      const std::uint64_t peer_id = next_peer_id_++;
      existing = peers_
                     .emplace(event.candidate.key,
                              Entry{
                                  .peer_id = peer_id,
                                  .candidate = event.candidate,
                              })
                     .first;
      change = XNN_TRANSFER_DISCOVERY_PEER_APPEARED;
    } else {
      existing->second.candidate = event.candidate;
    }

    AdvanceRevision();
    return MakeEvent(change, XNN_TRANSFER_DISCOVERY_EXPIRY_NONE,
                     existing->second.peer_id, existing->second.candidate);
  }

  template <typename Handler>
  void Clear(const std::uint32_t expiry_reason, Handler&& handler) {
    const std::scoped_lock lock(mutex_);
    for (const auto& [key, entry] : peers_) {
      static_cast<void>(key);
      AdvanceRevision();
      handler(MakeEvent(XNN_TRANSFER_DISCOVERY_PEER_EXPIRED, expiry_reason,
                        entry.peer_id, entry.candidate));
    }
    peers_.clear();
  }

  [[nodiscard]] xnn_transfer_status Snapshot(
      const std::uint64_t expected_revision, const std::uint32_t offset,
      xnn_transfer_discovery_snapshot_page* const out_page) const {
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
    if (offset > peers_.size()) {
      return XNN_TRANSFER_STATUS_INVALID_ARGUMENT;
    }

    xnn_transfer_discovery_snapshot_page output{
        .struct_size = sizeof(xnn_transfer_discovery_snapshot_page),
        .abi_version = XNN_TRANSFER_ABI_VERSION,
        .reserved = 0,
        .snapshot_revision = revision_,
        .offset = offset,
        .count = 0,
        .total_count = static_cast<std::uint32_t>(peers_.size()),
        .reserved2 = 0,
        .peers = {},
    };
    auto current = peers_.begin();
    std::advance(current, static_cast<std::ptrdiff_t>(offset));
    while (current != peers_.end() &&
           output.count < XNN_TRANSFER_DISCOVERY_SNAPSHOT_PAGE_CAPACITY) {
      output.peers[output.count] =
          MakePeer(current->second.peer_id, current->second.candidate);
      ++output.count;
      ++current;
    }
    *out_page = output;
    return XNN_TRANSFER_STATUS_OK;
  }

 private:
  struct Entry {
    std::uint64_t peer_id{};
    core::discovery::Candidate candidate{};
  };

  static xnn_transfer_discovery_peer MakePeer(
      const std::uint64_t peer_id, const core::discovery::Candidate& candidate) {
    xnn_transfer_discovery_peer peer{
        .struct_size = sizeof(xnn_transfer_discovery_peer),
        .abi_version = XNN_TRANSFER_ABI_VERSION,
        .reserved = 0,
        .peer_id = peer_id,
        .service_port = candidate.service_port,
        .address_family = static_cast<std::uint8_t>(
            candidate.key.source.family == core::discovery::AddressFamily::kIpv4
                ? XNN_TRANSFER_DISCOVERY_ADDRESS_FAMILY_IPV4
                : XNN_TRANSFER_DISCOVERY_ADDRESS_FAMILY_IPV6),
        .address_size =
            static_cast<std::uint8_t>(candidate.key.source.encoded().size()),
        .display_label_size = static_cast<std::uint32_t>(std::min(
            candidate.display_label.size(),
            static_cast<std::size_t>(XNN_TRANSFER_DISCOVERY_DISPLAY_LABEL_MAX_SIZE))),
        .address = {},
        .display_label = {},
    };
    const std::span<const std::uint8_t> address = candidate.key.source.encoded();
    std::copy(address.begin(), address.end(), peer.address);
    std::memcpy(peer.display_label, candidate.display_label.data(),
                peer.display_label_size);
    return peer;
  }

  xnn_transfer_discovery_peer_event_payload MakeEvent(
      const std::uint32_t change, const std::uint32_t expiry_reason,
      const std::uint64_t peer_id, const core::discovery::Candidate& candidate) const {
    return xnn_transfer_discovery_peer_event_payload{
        .struct_size = sizeof(xnn_transfer_discovery_peer_event_payload),
        .abi_version = XNN_TRANSFER_ABI_VERSION,
        .change = change,
        .snapshot_revision = revision_,
        .expiry_reason = expiry_reason,
        .reserved = 0,
        .peer = MakePeer(peer_id, candidate),
    };
  }

  static std::uint32_t ExpiryReason(
      const core::discovery::ExpiryReason reason) noexcept {
    switch (reason) {
      case core::discovery::ExpiryReason::kTtl:
        return XNN_TRANSFER_DISCOVERY_EXPIRY_TTL;
      case core::discovery::ExpiryReason::kWithdrawn:
        return XNN_TRANSFER_DISCOVERY_EXPIRY_WITHDRAWN;
      case core::discovery::ExpiryReason::kInterfaceRemoved:
        return XNN_TRANSFER_DISCOVERY_EXPIRY_INTERFACE_REMOVED;
      case core::discovery::ExpiryReason::kWake:
        return XNN_TRANSFER_DISCOVERY_EXPIRY_WAKE;
    }
    return XNN_TRANSFER_DISCOVERY_EXPIRY_NONE;
  }

  void AdvanceRevision() noexcept {
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
      revision_ = 1;
    } else {
      ++revision_;
    }
  }

  mutable std::mutex mutex_;
  std::map<core::discovery::CandidateKey, Entry> peers_;
  std::uint64_t revision_{1};
  std::uint64_t next_peer_id_{1};
};

}  // namespace xnn_transfer::bridge

#endif  // XNN_TRANSFER_BRIDGE_DISCOVERY_BRIDGE_HPP_
