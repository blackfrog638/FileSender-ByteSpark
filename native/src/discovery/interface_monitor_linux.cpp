#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <asio/error_code.hpp>
#include <asio/posix/stream_descriptor.hpp>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "interface_registry.hpp"
#include "runtime.hpp"

namespace xnn_transfer::core::discovery {
namespace {

[[nodiscard]] std::optional<std::uint8_t> PrefixLength(
    const std::span<const std::uint8_t> mask) noexcept {
  std::uint16_t prefix = 0;
  bool saw_zero = false;
  for (const std::uint8_t byte : mask) {
    for (int bit = 7; bit >= 0; --bit) {
      const bool set = (byte & (1U << static_cast<unsigned>(bit))) != 0;
      if (saw_zero && set) {
        return std::nullopt;
      }
      saw_zero = saw_zero || !set;
      prefix += static_cast<std::uint16_t>(set);
    }
  }
  return static_cast<std::uint8_t>(prefix);
}

[[nodiscard]] std::vector<RawNetworkInterface> EnumerateInterfaces() {
  std::vector<RawNetworkInterface> result;
  result.reserve(kMaxScopes);
  ifaddrs* addresses = nullptr;
  if (getifaddrs(&addresses) != 0) {
    return result;
  }
  for (const ifaddrs* current = addresses; current != nullptr;
       current = current->ifa_next) {
    if (current->ifa_addr == nullptr || current->ifa_netmask == nullptr ||
        current->ifa_name == nullptr || (current->ifa_flags & IFF_UP) == 0 ||
        (current->ifa_flags & IFF_MULTICAST) == 0 ||
        (current->ifa_flags & IFF_LOOPBACK) != 0) {
      continue;
    }
    const unsigned index = if_nametoindex(current->ifa_name);
    if (index == 0) {
      continue;
    }

    if (current->ifa_addr->sa_family == AF_INET) {
      const auto* address = reinterpret_cast<const sockaddr_in*>(current->ifa_addr);
      const auto* netmask = reinterpret_cast<const sockaddr_in*>(current->ifa_netmask);
      std::array<std::uint8_t, 4> encoded{};
      std::array<std::uint8_t, 4> mask{};
      std::memcpy(encoded.data(), &address->sin_addr, encoded.size());
      std::memcpy(mask.data(), &netmask->sin_addr, mask.size());
      const std::optional<std::uint8_t> prefix = PrefixLength(mask);
      if (!prefix.has_value() || encoded[0] == 0 || encoded[0] == 127 ||
          (encoded[0] >= 224 && encoded[0] <= 239)) {
        continue;
      }
      InsertBoundedRawInterface(
          result, RawNetworkInterface{.system_index = static_cast<std::uint32_t>(index),
                                      .family = AddressFamily::kIpv4,
                                      .local_address = IpAddress::V4(encoded),
                                      .prefix_length = *prefix});
      continue;
    }

    if (current->ifa_addr->sa_family == AF_INET6) {
      const auto* address = reinterpret_cast<const sockaddr_in6*>(current->ifa_addr);
      const auto* netmask = reinterpret_cast<const sockaddr_in6*>(current->ifa_netmask);
      if (IN6_IS_ADDR_UNSPECIFIED(&address->sin6_addr) ||
          IN6_IS_ADDR_LOOPBACK(&address->sin6_addr) ||
          IN6_IS_ADDR_MULTICAST(&address->sin6_addr)) {
        continue;
      }
      std::array<std::uint8_t, 16> encoded{};
      std::array<std::uint8_t, 16> mask{};
      std::memcpy(encoded.data(), &address->sin6_addr, encoded.size());
      std::memcpy(mask.data(), &netmask->sin6_addr, mask.size());
      const std::optional<std::uint8_t> prefix = PrefixLength(mask);
      if (!prefix.has_value()) {
        continue;
      }
      InsertBoundedRawInterface(
          result, RawNetworkInterface{.system_index = static_cast<std::uint32_t>(index),
                                      .family = AddressFamily::kIpv6,
                                      .local_address = IpAddress::V6(encoded),
                                      .prefix_length = *prefix});
    }
  }
  freeifaddrs(addresses);
  return result;
}

struct LinuxMonitorState {
  explicit LinuxMonitorState(asio::any_io_executor executor)
      : descriptor(std::move(executor)) {}

  asio::posix::stream_descriptor descriptor;
  std::atomic<bool> active{false};
  std::mutex mutex;
  InterfaceMonitor::ChangeHandler handler{};
};

void ArmMonitor(const std::shared_ptr<LinuxMonitorState>& state) {
  const std::weak_ptr<LinuxMonitorState> weak_state = state;
  state->descriptor.async_wait(
      asio::posix::stream_descriptor::wait_read,
      [weak_state](const asio::error_code& error) {
        const std::shared_ptr<LinuxMonitorState> locked = weak_state.lock();
        if (locked == nullptr || error ||
            !locked->active.load(std::memory_order_acquire)) {
          return;
        }

        std::array<std::uint8_t, 8'192> buffer{};
        for (std::size_t reads = 0; reads < 64; ++reads) {
          const ssize_t received = recv(locked->descriptor.native_handle(),
                                        buffer.data(), buffer.size(), MSG_DONTWAIT);
          if (received >= 0) {
            continue;
          }
          if (errno == EINTR) {
            continue;
          }
          break;
        }

        InterfaceMonitor::ChangeHandler handler;
        {
          const std::scoped_lock lock(locked->mutex);
          if (locked->active.load(std::memory_order_acquire)) {
            handler = locked->handler;
          }
        }
        if (handler) {
          handler();
        }
        if (locked->active.load(std::memory_order_acquire)) {
          ArmMonitor(locked);
        }
      });
}

class LinuxInterfaceMonitor final : public InterfaceMonitor {
 public:
  explicit LinuxInterfaceMonitor(asio::any_io_executor executor)
      : state_(std::make_shared<LinuxMonitorState>(std::move(executor))) {}

  ~LinuxInterfaceMonitor() override { Stop(); }

  [[nodiscard]] std::vector<NetworkInterface> Snapshot() override {
    return registry_.Update(EnumerateInterfaces());
  }

  [[nodiscard]] bool Start(ChangeHandler change_handler) override {
    if (!change_handler || state_->active.exchange(true, std::memory_order_acq_rel)) {
      return false;
    }
    {
      const std::scoped_lock lock(state_->mutex);
      state_->handler = std::move(change_handler);
    }

    const int descriptor =
        socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (descriptor < 0) {
      Stop();
      return false;
    }
    sockaddr_nl address{};
    address.nl_family = AF_NETLINK;
    address.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
    if (bind(descriptor, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0) {
      close(descriptor);
      Stop();
      return false;
    }

    asio::error_code error;
    state_->descriptor.assign(descriptor, error);
    if (error) {
      close(descriptor);
      Stop();
      return false;
    }
    ArmMonitor(state_);
    return true;
  }

  void Stop() override {
    if (!state_->active.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    {
      const std::scoped_lock lock(state_->mutex);
      state_->handler = {};
    }
    asio::error_code error;
    state_->descriptor.cancel(error);
    state_->descriptor.close(error);
  }

 private:
  std::shared_ptr<LinuxMonitorState> state_;
  InterfaceGenerationRegistry registry_;
};

}  // namespace

std::unique_ptr<InterfaceMonitor> MakeSystemInterfaceMonitor(
    asio::any_io_executor executor) {
  return std::make_unique<LinuxInterfaceMonitor>(std::move(executor));
}

}  // namespace xnn_transfer::core::discovery
