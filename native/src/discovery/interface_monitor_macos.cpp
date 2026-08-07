#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SCDynamicStore.h>
#include <dispatch/dispatch.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <array>
#include <asio/post.hpp>
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

struct MonitorState {
  explicit MonitorState(asio::any_io_executor value) : executor(std::move(value)) {}

  asio::any_io_executor executor;
  std::mutex mutex;
  bool active{};
  bool pending{};
  InterfaceMonitor::ChangeHandler handler{};
};

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

void PostChange(const std::shared_ptr<MonitorState>& state) {
  {
    const std::scoped_lock lock(state->mutex);
    if (!state->active || state->pending) {
      return;
    }
    state->pending = true;
  }

  const std::weak_ptr<MonitorState> weak_state = state;
  try {
    asio::post(state->executor, [weak_state] {
      const std::shared_ptr<MonitorState> locked = weak_state.lock();
      if (locked == nullptr) {
        return;
      }
      InterfaceMonitor::ChangeHandler handler;
      {
        const std::scoped_lock lock(locked->mutex);
        locked->pending = false;
        if (!locked->active) {
          return;
        }
        handler = locked->handler;
      }
      if (handler) {
        handler();
      }
    });
  } catch (...) {
    const std::scoped_lock lock(state->mutex);
    state->pending = false;
  }
}

void StoreChanged(SCDynamicStoreRef, CFArrayRef, void* context) {
  auto* state = static_cast<std::shared_ptr<MonitorState>*>(context);
  PostChange(*state);
}

void DispatchBarrier(void*) {}

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

class MacOsInterfaceMonitor final : public InterfaceMonitor {
 public:
  explicit MacOsInterfaceMonitor(asio::any_io_executor executor)
      : state_(std::make_shared<MonitorState>(std::move(executor))),
        callback_state_(state_) {}

  ~MacOsInterfaceMonitor() override { Stop(); }

  [[nodiscard]] std::vector<NetworkInterface> Snapshot() override {
    return registry_.Update(EnumerateInterfaces());
  }

  [[nodiscard]] bool Start(ChangeHandler change_handler) override {
    {
      const std::scoped_lock lock(state_->mutex);
      if (state_->active || !change_handler) {
        return false;
      }
      state_->active = true;
      state_->handler = std::move(change_handler);
    }

    SCDynamicStoreContext context{.version = 0,
                                  .info = &callback_state_,
                                  .retain = nullptr,
                                  .release = nullptr,
                                  .copyDescription = nullptr};
    store_ = SCDynamicStoreCreate(nullptr, CFSTR("XnnTransferDiscovery"), &StoreChanged,
                                  &context);
    if (store_ == nullptr) {
      Deactivate();
      return false;
    }

    const void* values[] = {CFSTR("State:/Network/Interface/.*/IPv4"),
                            CFSTR("State:/Network/Interface/.*/IPv6"),
                            CFSTR("State:/Network/Global/IPv4"),
                            CFSTR("State:/Network/Global/IPv6")};
    CFArrayRef patterns =
        CFArrayCreate(nullptr, values, static_cast<CFIndex>(std::size(values)),
                      &kCFTypeArrayCallBacks);
    const Boolean notifications_set =
        patterns != nullptr &&
        SCDynamicStoreSetNotificationKeys(store_, nullptr, patterns);
    if (patterns != nullptr) {
      CFRelease(patterns);
    }
    if (!notifications_set) {
      Stop();
      return false;
    }

    queue_ = dispatch_queue_create("com.xnntransfer.discovery.interfaces",
                                   DISPATCH_QUEUE_SERIAL);
    if (queue_ == nullptr || !SCDynamicStoreSetDispatchQueue(store_, queue_)) {
      Stop();
      return false;
    }
    return true;
  }

  void Stop() override {
    Deactivate();
    if (store_ != nullptr) {
      (void)SCDynamicStoreSetDispatchQueue(store_, nullptr);
    }
    if (queue_ != nullptr) {
      dispatch_sync_f(queue_, nullptr, &DispatchBarrier);
#if !OS_OBJECT_USE_OBJC
      dispatch_release(queue_);
#endif
      queue_ = nullptr;
    }
    if (store_ != nullptr) {
      CFRelease(store_);
      store_ = nullptr;
    }
  }

 private:
  void Deactivate() {
    const std::scoped_lock lock(state_->mutex);
    state_->active = false;
    state_->handler = {};
  }

  std::shared_ptr<MonitorState> state_;
  std::shared_ptr<MonitorState> callback_state_;
  InterfaceGenerationRegistry registry_;
  SCDynamicStoreRef store_{};
  dispatch_queue_t queue_{};
};

}  // namespace

std::unique_ptr<InterfaceMonitor> MakeSystemInterfaceMonitor(
    asio::any_io_executor executor) {
  return std::make_unique<MacOsInterfaceMonitor>(std::move(executor));
}

}  // namespace xnn_transfer::core::discovery
