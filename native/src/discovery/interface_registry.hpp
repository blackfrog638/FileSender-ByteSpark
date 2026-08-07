#ifndef XNN_TRANSFER_DISCOVERY_INTERFACE_REGISTRY_HPP_
#define XNN_TRANSFER_DISCOVERY_INTERFACE_REGISTRY_HPP_

#include <cstdint>
#include <span>
#include <vector>

#include "xnn_transfer/core/discovery/discovery.hpp"

namespace xnn_transfer::core::discovery {

struct RawNetworkInterface {
  std::uint32_t system_index{};
  AddressFamily family{AddressFamily::kIpv4};
  IpAddress local_address{};
  std::uint8_t prefix_length{};

  friend bool operator==(const RawNetworkInterface&,
                         const RawNetworkInterface&) = default;
  friend bool operator<(const RawNetworkInterface& left,
                        const RawNetworkInterface& right) noexcept;
};

void InsertBoundedRawInterface(std::vector<RawNetworkInterface>& interfaces,
                               const RawNetworkInterface& candidate);

class InterfaceGenerationRegistry final {
 public:
  [[nodiscard]] std::vector<NetworkInterface> Update(
      std::span<const RawNetworkInterface> interfaces);

 private:
  std::uint64_t next_generation_{1};
  std::vector<NetworkInterface> active_;
};

}  // namespace xnn_transfer::core::discovery

#endif  // XNN_TRANSFER_DISCOVERY_INTERFACE_REGISTRY_HPP_
