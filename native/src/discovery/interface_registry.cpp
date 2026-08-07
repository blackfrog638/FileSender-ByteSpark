#include "interface_registry.hpp"

#include <algorithm>
#include <limits>
#include <span>
#include <vector>

namespace xnn_transfer::core::discovery {

bool operator<(const RawNetworkInterface& left,
               const RawNetworkInterface& right) noexcept {
  if (left.system_index != right.system_index) {
    return left.system_index < right.system_index;
  }
  if (left.family != right.family) {
    return left.family < right.family;
  }
  if (left.local_address != right.local_address) {
    return left.local_address < right.local_address;
  }
  return left.prefix_length < right.prefix_length;
}

void InsertBoundedRawInterface(std::vector<RawNetworkInterface>& interfaces,
                               const RawNetworkInterface& candidate) {
  const auto same_scope =
      std::find_if(interfaces.begin(), interfaces.end(),
                   [&candidate](const RawNetworkInterface& current) {
                     return current.system_index == candidate.system_index &&
                            current.family == candidate.family;
                   });
  if (same_scope != interfaces.end()) {
    if (candidate < *same_scope) {
      *same_scope = candidate;
      std::sort(interfaces.begin(), interfaces.end());
    }
    return;
  }

  const auto position =
      std::lower_bound(interfaces.begin(), interfaces.end(), candidate);
  if (interfaces.size() < kMaxScopes) {
    interfaces.insert(position, candidate);
  } else if (position != interfaces.end()) {
    interfaces.insert(position, candidate);
    interfaces.pop_back();
  }
}

std::vector<NetworkInterface> InterfaceGenerationRegistry::Update(
    const std::span<const RawNetworkInterface> interfaces) {
  std::vector<RawNetworkInterface> canonical;
  canonical.reserve(kMaxScopes);
  for (const RawNetworkInterface& interface : interfaces) {
    InsertBoundedRawInterface(canonical, interface);
  }

  std::vector<NetworkInterface> replacement;
  replacement.reserve(canonical.size());
  for (const RawNetworkInterface& interface : canonical) {
    const auto existing = std::find_if(
        active_.begin(), active_.end(), [&interface](const NetworkInterface& current) {
          return current.system_index == interface.system_index &&
                 current.scope.family == interface.family &&
                 current.local_address == interface.local_address &&
                 current.prefix_length == interface.prefix_length;
        });
    std::uint64_t generation = 0;
    if (existing != active_.end()) {
      generation = existing->scope.generation;
    } else {
      if (next_generation_ == 0 ||
          next_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        continue;
      }
      generation = next_generation_;
      ++next_generation_;
    }
    replacement.push_back(NetworkInterface{
        .scope = InterfaceScope{.generation = generation, .family = interface.family},
        .system_index = interface.system_index,
        .local_address = interface.local_address,
        .prefix_length = interface.prefix_length});
  }
  active_ = replacement;
  return replacement;
}

}  // namespace xnn_transfer::core::discovery
