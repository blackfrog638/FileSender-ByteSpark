#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// The IP Helper notification API requires ws2ipdef.h from ws2tcpip.h first.
// clang-format off
#include <WinSock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <iphlpapi.h>
// clang-format on

#include <array>
#include <asio/post.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "interface_registry.hpp"
#include "runtime.hpp"

namespace xnn_transfer::core::discovery {
namespace {

constexpr ULONG kMaximumAdapterBuffer = 256U * 1024U;

struct MonitorState {
  explicit MonitorState(asio::any_io_executor value) : executor(std::move(value)) {}

  asio::any_io_executor executor;
  std::mutex mutex;
  bool active{};
  bool pending{};
  InterfaceMonitor::ChangeHandler handler{};
};

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

void CALLBACK InterfaceChanged(PVOID context, PMIB_IPINTERFACE_ROW,
                               MIB_NOTIFICATION_TYPE) {
  auto* state = static_cast<std::shared_ptr<MonitorState>*>(context);
  PostChange(*state);
}

[[nodiscard]] std::vector<RawNetworkInterface> EnumerateInterfaces() {
  std::vector<RawNetworkInterface> result;
  result.reserve(kMaxScopes);
  constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                          GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME;
  ULONG size = 0;
  const ULONG first = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, nullptr, &size);
  if (first != ERROR_BUFFER_OVERFLOW || size == 0 || size > kMaximumAdapterBuffer) {
    return result;
  }

  std::vector<std::uint8_t> buffer(size);
  auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &size) != NO_ERROR) {
    return result;
  }

  for (const IP_ADAPTER_ADDRESSES* adapter = adapters; adapter != nullptr;
       adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp ||
        adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
        (adapter->Flags & IP_ADAPTER_NO_MULTICAST) != 0) {
      continue;
    }
    for (const IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress;
         unicast != nullptr; unicast = unicast->Next) {
      if (unicast->Address.lpSockaddr == nullptr) {
        continue;
      }
      if (unicast->Address.lpSockaddr->sa_family == AF_INET && adapter->IfIndex != 0 &&
          unicast->OnLinkPrefixLength <= 32) {
        const auto* address =
            reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
        std::array<std::uint8_t, 4> encoded{};
        std::memcpy(encoded.data(), &address->sin_addr, encoded.size());
        if (encoded[0] == 0 || encoded[0] == 127 ||
            (encoded[0] >= 224 && encoded[0] <= 239)) {
          continue;
        }
        InsertBoundedRawInterface(
            result, RawNetworkInterface{.system_index = adapter->IfIndex,
                                        .family = AddressFamily::kIpv4,
                                        .local_address = IpAddress::V4(encoded),
                                        .prefix_length = unicast->OnLinkPrefixLength});
        continue;
      }
      if (unicast->Address.lpSockaddr->sa_family == AF_INET6 &&
          adapter->Ipv6IfIndex != 0 && unicast->OnLinkPrefixLength <= 128) {
        const auto* address =
            reinterpret_cast<const sockaddr_in6*>(unicast->Address.lpSockaddr);
        if (IN6_IS_ADDR_UNSPECIFIED(&address->sin6_addr) ||
            IN6_IS_ADDR_LOOPBACK(&address->sin6_addr) ||
            IN6_IS_ADDR_MULTICAST(&address->sin6_addr)) {
          continue;
        }
        std::array<std::uint8_t, 16> encoded{};
        std::memcpy(encoded.data(), &address->sin6_addr, encoded.size());
        InsertBoundedRawInterface(
            result, RawNetworkInterface{.system_index = adapter->Ipv6IfIndex,
                                        .family = AddressFamily::kIpv6,
                                        .local_address = IpAddress::V6(encoded),
                                        .prefix_length = unicast->OnLinkPrefixLength});
      }
    }
  }
  return result;
}

class WindowsInterfaceMonitor final : public InterfaceMonitor {
 public:
  explicit WindowsInterfaceMonitor(asio::any_io_executor executor)
      : state_(std::make_shared<MonitorState>(std::move(executor))),
        callback_state_(state_) {}

  ~WindowsInterfaceMonitor() override { Stop(); }

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
    if (NotifyIpInterfaceChange(AF_UNSPEC, &InterfaceChanged, &callback_state_, FALSE,
                                &notification_handle_) != NO_ERROR) {
      Deactivate();
      return false;
    }
    return true;
  }

  void Stop() override {
    Deactivate();
    if (notification_handle_ != nullptr) {
      (void)CancelMibChangeNotify2(notification_handle_);
      notification_handle_ = nullptr;
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
  HANDLE notification_handle_{};
};

}  // namespace

std::unique_ptr<InterfaceMonitor> MakeSystemInterfaceMonitor(
    asio::any_io_executor executor) {
  return std::make_unique<WindowsInterfaceMonitor>(std::move(executor));
}

}  // namespace xnn_transfer::core::discovery
