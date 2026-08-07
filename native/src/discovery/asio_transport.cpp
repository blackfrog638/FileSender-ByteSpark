#include <algorithm>
#include <array>
#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/error_code.hpp>
#include <asio/ip/multicast.hpp>
#include <asio/ip/udp.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/socket_base.hpp>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "runtime.hpp"

#if defined(_WIN32)
#include <MSWSock.h>
#include <WS2tcpip.h>
#include <WinSock2.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <net/if_dl.h>
#endif
#endif

namespace xnn_transfer::core::discovery {
namespace {

constexpr std::array<std::uint8_t, 4> kIpv4Group{239, 255, 88, 78};
constexpr std::array<std::uint8_t, 16> kIpv6Group{0xff, 0x12, 0x00, 0x00, 0x00, 0x00,
                                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                  0x58, 0x4e, 0x4e, 0x44};

enum class ReadStatus {
  kReceived,
  kWouldBlock,
  kError,
};

struct ScopeSocket {
  ScopeSocket(asio::any_io_executor executor, NetworkInterface value)
      : interface(std::move(value)), socket(std::move(executor)) {}

  NetworkInterface interface;
  asio::ip::udp::socket socket;
  std::array<std::uint8_t, kMaxDatagramSize> buffer{};
#if defined(_WIN32)
  LPFN_WSARECVMSG receive_message{};
#endif
};

struct TransportState {
  explicit TransportState(asio::any_io_executor value) : executor(std::move(value)) {}

  asio::any_io_executor executor;
  std::atomic<bool> active{false};
  std::mutex mutex;
  std::condition_variable callbacks_drained;
  DatagramTransport::ReceiveHandler handler{};
  std::size_t callbacks_in_flight{};
  std::vector<std::shared_ptr<ScopeSocket>> sockets;
};

thread_local const TransportState* g_delivering_transport = nullptr;

[[nodiscard]] IpAddress GroupAddress(const AddressFamily family) noexcept {
  return family == AddressFamily::kIpv4 ? IpAddress::V4(kIpv4Group)
                                        : IpAddress::V6(kIpv6Group);
}

[[nodiscard]] asio::ip::udp::endpoint GroupEndpoint(const AddressFamily family) {
  if (family == AddressFamily::kIpv4) {
    asio::ip::address_v4::bytes_type bytes{};
    std::copy(kIpv4Group.begin(), kIpv4Group.end(), bytes.begin());
    return {asio::ip::address_v4(bytes), kDiscoveryPort};
  }
  asio::ip::address_v6::bytes_type bytes{};
  std::copy(kIpv6Group.begin(), kIpv6Group.end(), bytes.begin());
  return {asio::ip::address_v6(bytes), kDiscoveryPort};
}

[[nodiscard]] bool IsDirectedBroadcast(const IpAddress& source,
                                       const NetworkInterface& interface) noexcept {
  if (source.family != AddressFamily::kIpv4 ||
      interface.scope.family != AddressFamily::kIpv4 || interface.prefix_length >= 32) {
    return false;
  }
  const auto to_integer = [](const IpAddress& address) {
    const std::span<const std::uint8_t> bytes = address.encoded();
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
  };
  const std::uint32_t source_value = to_integer(source);
  const std::uint32_t local_value = to_integer(interface.local_address);
  const std::uint32_t mask = interface.prefix_length == 0
                                 ? 0
                                 : std::numeric_limits<std::uint32_t>::max()
                                       << (32U - interface.prefix_length);
  return (source_value & mask) == (local_value & mask) &&
         (source_value | mask) == std::numeric_limits<std::uint32_t>::max();
}

[[nodiscard]] bool SourceAddress(const sockaddr_storage& storage,
                                 IpAddress& output) noexcept {
  if (storage.ss_family == AF_INET) {
    const auto* address = reinterpret_cast<const sockaddr_in*>(&storage);
    std::array<std::uint8_t, 4> bytes{};
    std::memcpy(bytes.data(), &address->sin_addr, bytes.size());
    output = IpAddress::V4(bytes);
    return true;
  }
  if (storage.ss_family == AF_INET6) {
    const auto* address = reinterpret_cast<const sockaddr_in6*>(&storage);
    std::array<std::uint8_t, 16> bytes{};
    std::memcpy(bytes.data(), &address->sin6_addr, bytes.size());
    output = IpAddress::V6(bytes);
    return true;
  }
  return false;
}

void Deliver(const std::shared_ptr<TransportState>& state,
             const DatagramMetadata& metadata,
             const std::span<const std::uint8_t> payload) {
  DatagramTransport::ReceiveHandler handler;
  {
    const std::scoped_lock lock(state->mutex);
    if (!state->active.load(std::memory_order_acquire) || !state->handler) {
      return;
    }
    handler = state->handler;
    ++state->callbacks_in_flight;
  }

  const TransportState* previous = g_delivering_transport;
  g_delivering_transport = state.get();
  try {
    handler(metadata, payload);
  } catch (...) {
  }
  g_delivering_transport = previous;

  {
    const std::scoped_lock lock(state->mutex);
    --state->callbacks_in_flight;
    state->callbacks_drained.notify_all();
  }
}

#if defined(_WIN32)

[[nodiscard]] bool SetOption(const SOCKET socket, const int level, const int option,
                             const void* value, const int size) noexcept {
  return setsockopt(socket, level, option, static_cast<const char*>(value), size) == 0;
}

[[nodiscard]] bool ConfigureNativeSocket(ScopeSocket& scope) {
  const SOCKET socket = scope.socket.native_handle();
  DWORD returned = 0;
  GUID receive_message_guid = WSAID_WSARECVMSG;
  if (WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &receive_message_guid,
               sizeof(receive_message_guid), &scope.receive_message,
               sizeof(scope.receive_message), &returned, nullptr, nullptr) != 0) {
    return false;
  }

  const DWORD one = 1;
  if (scope.interface.scope.family == AddressFamily::kIpv4) {
    in_addr group{};
    in_addr local{};
    std::memcpy(&group, kIpv4Group.data(), kIpv4Group.size());
    std::memcpy(&local, scope.interface.local_address.encoded().data(),
                kIpv4Group.size());
    ip_mreq membership{.imr_multiaddr = group, .imr_interface = local};
    const unsigned char hops = 1;
    return SetOption(socket, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one)) &&
           SetOption(socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
                     sizeof(membership)) &&
           SetOption(socket, IPPROTO_IP, IP_MULTICAST_IF, &local, sizeof(local)) &&
           SetOption(socket, IPPROTO_IP, IP_MULTICAST_TTL, &hops, sizeof(hops)) &&
           SetOption(socket, IPPROTO_IP, IP_MULTICAST_LOOP, &one, sizeof(one));
  }

  in6_addr group{};
  std::memcpy(&group, kIpv6Group.data(), kIpv6Group.size());
  ipv6_mreq membership{.ipv6mr_multiaddr = group,
                       .ipv6mr_interface = scope.interface.system_index};
  const int hops = 1;
  return SetOption(socket, IPPROTO_IPV6, IPV6_PKTINFO, &one, sizeof(one)) &&
         SetOption(socket, IPPROTO_IPV6, IPV6_JOIN_GROUP, &membership,
                   sizeof(membership)) &&
         SetOption(socket, IPPROTO_IPV6, IPV6_MULTICAST_IF,
                   &scope.interface.system_index,
                   sizeof(scope.interface.system_index)) &&
         SetOption(socket, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)) &&
         SetOption(socket, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &one, sizeof(one));
}

[[nodiscard]] ReadStatus ReadOne(const std::shared_ptr<ScopeSocket>& scope,
                                 const std::shared_ptr<TransportState>& state) {
  sockaddr_storage source{};
  WSABUF buffer{.len = static_cast<ULONG>(scope->buffer.size()),
                .buf = reinterpret_cast<char*>(scope->buffer.data())};
  std::array<char, 256> control{};
  WSAMSG message{};
  message.name = reinterpret_cast<sockaddr*>(&source);
  message.namelen = sizeof(source);
  message.lpBuffers = &buffer;
  message.dwBufferCount = 1;
  message.Control.buf = control.data();
  message.Control.len = static_cast<ULONG>(control.size());
  DWORD received = 0;
  const int result = scope->receive_message(scope->socket.native_handle(), &message,
                                            &received, nullptr, nullptr);
  bool truncated = (message.dwFlags & MSG_TRUNC) != 0;
  if (result == SOCKET_ERROR) {
    const int error = WSAGetLastError();
    if (error == WSAEWOULDBLOCK) {
      return ReadStatus::kWouldBlock;
    }
    if (error != WSAEMSGSIZE) {
      return ReadStatus::kError;
    }
    truncated = true;
  }

  DatagramMetadata metadata{
      .observer = scope->interface.scope,
      .destination = GroupAddress(scope->interface.scope.family),
      .destination_port = kDiscoveryPort,
      .observer_eligible = false,
      .truncated = truncated || received > static_cast<DWORD>(scope->buffer.size())};
  if (!SourceAddress(source, metadata.source) ||
      metadata.source.family != scope->interface.scope.family) {
    Deliver(state, metadata, {});
    return ReadStatus::kReceived;
  }

  std::uint32_t interface_index = 0;
  bool has_destination = false;
  for (WSACMSGHDR* header = WSA_CMSG_FIRSTHDR(&message); header != nullptr;
       header = WSA_CMSG_NXTHDR(&message, header)) {
    if (header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_PKTINFO) {
      const auto* info = reinterpret_cast<const IN_PKTINFO*>(WSA_CMSG_DATA(header));
      interface_index = info->ipi_ifindex;
      std::array<std::uint8_t, 4> bytes{};
      std::memcpy(bytes.data(), &info->ipi_addr, bytes.size());
      metadata.destination = IpAddress::V4(bytes);
      has_destination = true;
    } else if (header->cmsg_level == IPPROTO_IPV6 &&
               header->cmsg_type == IPV6_PKTINFO) {
      const auto* info = reinterpret_cast<const IN6_PKTINFO*>(WSA_CMSG_DATA(header));
      interface_index = info->ipi6_ifindex;
      std::array<std::uint8_t, 16> bytes{};
      std::memcpy(bytes.data(), &info->ipi6_addr, bytes.size());
      metadata.destination = IpAddress::V6(bytes);
      has_destination = true;
    }
  }
  metadata.observer_eligible =
      has_destination && interface_index == scope->interface.system_index;
  metadata.source_is_broadcast = IsDirectedBroadcast(metadata.source, scope->interface);
  const std::size_t payload_size =
      std::min<std::size_t>(received, scope->buffer.size());
  Deliver(state, metadata,
          std::span<const std::uint8_t>(scope->buffer).first(payload_size));
  return ReadStatus::kReceived;
}

#else

[[nodiscard]] bool SetOption(const int socket, const int level, const int option,
                             const void* value, const socklen_t size) noexcept {
  return setsockopt(socket, level, option, value, size) == 0;
}

[[nodiscard]] bool ConfigureNativeSocket(ScopeSocket& scope) {
  const int socket = scope.socket.native_handle();
  const int one = 1;
  if (scope.interface.scope.family == AddressFamily::kIpv4) {
    in_addr group{};
    in_addr local{};
    std::memcpy(&group, kIpv4Group.data(), kIpv4Group.size());
    std::memcpy(&local, scope.interface.local_address.encoded().data(),
                kIpv4Group.size());
#if defined(__APPLE__)
    ip_mreq membership{.imr_multiaddr = group, .imr_interface = local};
    const unsigned char hops = 1;
    return SetOption(socket, IPPROTO_IP, IP_RECVDSTADDR, &one, sizeof(one)) &&
           SetOption(socket, IPPROTO_IP, IP_RECVIF, &one, sizeof(one)) &&
           SetOption(socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
                     sizeof(membership)) &&
           SetOption(socket, IPPROTO_IP, IP_MULTICAST_IF, &local, sizeof(local)) &&
           SetOption(socket, IPPROTO_IP, IP_MULTICAST_TTL, &hops, sizeof(hops)) &&
           SetOption(socket, IPPROTO_IP, IP_MULTICAST_LOOP, &one, sizeof(one));
#else
    ip_mreqn membership{.imr_multiaddr = group,
                        .imr_address = local,
                        .imr_ifindex = static_cast<int>(scope.interface.system_index)};
    const unsigned char hops = 1;
    bool configured =
        SetOption(socket, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one)) &&
        SetOption(socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
                  sizeof(membership)) &&
        SetOption(socket, IPPROTO_IP, IP_MULTICAST_IF, &membership,
                  sizeof(membership)) &&
        SetOption(socket, IPPROTO_IP, IP_MULTICAST_TTL, &hops, sizeof(hops)) &&
        SetOption(socket, IPPROTO_IP, IP_MULTICAST_LOOP, &one, sizeof(one));
#if defined(IP_MULTICAST_ALL)
    const int multicast_all = 0;
    configured = configured && SetOption(socket, IPPROTO_IP, IP_MULTICAST_ALL,
                                         &multicast_all, sizeof(multicast_all));
#endif
    return configured;
#endif
  }

  in6_addr group{};
  std::memcpy(&group, kIpv6Group.data(), kIpv6Group.size());
  ipv6_mreq membership{.ipv6mr_multiaddr = group,
                       .ipv6mr_interface = scope.interface.system_index};
  const int hops = 1;
  return SetOption(socket, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one, sizeof(one)) &&
         SetOption(socket, IPPROTO_IPV6, IPV6_JOIN_GROUP, &membership,
                   sizeof(membership)) &&
         SetOption(socket, IPPROTO_IPV6, IPV6_MULTICAST_IF,
                   &scope.interface.system_index,
                   sizeof(scope.interface.system_index)) &&
         SetOption(socket, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)) &&
         SetOption(socket, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &one, sizeof(one));
}

[[nodiscard]] ReadStatus ReadOne(const std::shared_ptr<ScopeSocket>& scope,
                                 const std::shared_ptr<TransportState>& state) {
  sockaddr_storage source{};
  iovec buffer{.iov_base = scope->buffer.data(), .iov_len = scope->buffer.size()};
  std::array<std::uint8_t, 256> control{};
  msghdr message{};
  message.msg_name = &source;
  message.msg_namelen = sizeof(source);
  message.msg_iov = &buffer;
  message.msg_iovlen = 1;
  message.msg_control = control.data();
  message.msg_controllen = control.size();
  const ssize_t received =
      recvmsg(scope->socket.native_handle(), &message, MSG_DONTWAIT | MSG_TRUNC);
  if (received < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return ReadStatus::kWouldBlock;
    }
    if (errno == EINTR) {
      return ReadStatus::kReceived;
    }
    return ReadStatus::kError;
  }

  DatagramMetadata metadata{
      .observer = scope->interface.scope,
      .destination = GroupAddress(scope->interface.scope.family),
      .destination_port = kDiscoveryPort,
      .observer_eligible = false,
      .truncated = (message.msg_flags & MSG_TRUNC) != 0 ||
                   received > static_cast<ssize_t>(scope->buffer.size())};
  if (!SourceAddress(source, metadata.source) ||
      metadata.source.family != scope->interface.scope.family) {
    Deliver(state, metadata, {});
    return ReadStatus::kReceived;
  }

  std::uint32_t interface_index = 0;
  bool has_destination = false;
  for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
       header = CMSG_NXTHDR(&message, header)) {
#if defined(__APPLE__)
    if (header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_RECVDSTADDR) {
      const auto* address = reinterpret_cast<const in_addr*>(CMSG_DATA(header));
      std::array<std::uint8_t, 4> bytes{};
      std::memcpy(bytes.data(), address, bytes.size());
      metadata.destination = IpAddress::V4(bytes);
      has_destination = true;
    } else if (header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_RECVIF) {
      const auto* link = reinterpret_cast<const sockaddr_dl*>(CMSG_DATA(header));
      interface_index = link->sdl_index;
    } else
#else
    if (header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_PKTINFO) {
      const auto* info = reinterpret_cast<const in_pktinfo*>(CMSG_DATA(header));
      interface_index = static_cast<std::uint32_t>(info->ipi_ifindex);
      std::array<std::uint8_t, 4> bytes{};
      std::memcpy(bytes.data(), &info->ipi_addr, bytes.size());
      metadata.destination = IpAddress::V4(bytes);
      has_destination = true;
    } else
#endif
        if (header->cmsg_level == IPPROTO_IPV6 && header->cmsg_type == IPV6_PKTINFO) {
      const auto* info = reinterpret_cast<const in6_pktinfo*>(CMSG_DATA(header));
      interface_index = info->ipi6_ifindex;
      std::array<std::uint8_t, 16> bytes{};
      std::memcpy(bytes.data(), &info->ipi6_addr, bytes.size());
      metadata.destination = IpAddress::V6(bytes);
      has_destination = true;
    }
  }
  metadata.observer_eligible =
      has_destination && interface_index == scope->interface.system_index;
  metadata.source_is_broadcast = IsDirectedBroadcast(metadata.source, scope->interface);
  const std::size_t payload_size =
      std::min<std::size_t>(static_cast<std::size_t>(received), scope->buffer.size());
  Deliver(state, metadata,
          std::span<const std::uint8_t>(scope->buffer).first(payload_size));
  return ReadStatus::kReceived;
}

#endif

void CloseSocket(const std::shared_ptr<ScopeSocket>& scope) {
  asio::error_code error;
  scope->socket.cancel(error);
  scope->socket.close(error);
}

void ArmReceive(const std::shared_ptr<ScopeSocket>& scope,
                const std::shared_ptr<TransportState>& state) {
  const std::weak_ptr<TransportState> weak_state = state;
  scope->socket.async_wait(
      asio::ip::udp::socket::wait_read,
      [scope, weak_state](const asio::error_code& error) {
        const std::shared_ptr<TransportState> locked = weak_state.lock();
        if (locked == nullptr || error ||
            !locked->active.load(std::memory_order_acquire)) {
          return;
        }
        for (std::size_t count = 0; count < 64; ++count) {
          const ReadStatus status = ReadOne(scope, locked);
          if (status == ReadStatus::kWouldBlock || status == ReadStatus::kError ||
              !locked->active.load(std::memory_order_acquire)) {
            break;
          }
        }
        if (locked->active.load(std::memory_order_acquire) && scope->socket.is_open()) {
          ArmReceive(scope, locked);
        }
      });
}

[[nodiscard]] std::shared_ptr<ScopeSocket> OpenSocket(
    const asio::any_io_executor& executor, const NetworkInterface& interface,
    const std::shared_ptr<TransportState>& state) {
  auto scope = std::make_shared<ScopeSocket>(executor, interface);
  asio::error_code error;
  const asio::ip::udp protocol = interface.scope.family == AddressFamily::kIpv4
                                     ? asio::ip::udp::v4()
                                     : asio::ip::udp::v6();
  scope->socket.open(protocol, error);
  if (error) {
    return nullptr;
  }
  scope->socket.set_option(asio::socket_base::reuse_address(true), error);
  if (error) {
    CloseSocket(scope);
    return nullptr;
  }
  if (interface.scope.family == AddressFamily::kIpv6) {
    scope->socket.set_option(asio::ip::v6_only(true), error);
    if (error) {
      CloseSocket(scope);
      return nullptr;
    }
  }
  scope->socket.bind(asio::ip::udp::endpoint(protocol, kDiscoveryPort), error);
  if (error || !ConfigureNativeSocket(*scope)) {
    CloseSocket(scope);
    return nullptr;
  }
  scope->socket.non_blocking(true, error);
  if (error) {
    CloseSocket(scope);
    return nullptr;
  }
  ArmReceive(scope, state);
  return scope;
}

class AsioDatagramTransport final : public DatagramTransport {
 public:
  explicit AsioDatagramTransport(asio::any_io_executor executor)
      : state_(std::make_shared<TransportState>(std::move(executor))) {}

  ~AsioDatagramTransport() override { Stop(); }

  [[nodiscard]] bool Start(const std::span<const NetworkInterface> interfaces,
                           ReceiveHandler receive_handler) override {
    if (!receive_handler || state_->active.exchange(true, std::memory_order_acq_rel)) {
      return false;
    }
    {
      const std::scoped_lock lock(state_->mutex);
      state_->handler = std::move(receive_handler);
    }
    if (!Reconfigure(interfaces)) {
      Stop();
      return false;
    }
    return true;
  }

  [[nodiscard]] bool Reconfigure(
      const std::span<const NetworkInterface> interfaces) override {
    if (!state_->active.load(std::memory_order_acquire) ||
        interfaces.size() > kMaxScopes) {
      return false;
    }
    std::vector<std::shared_ptr<ScopeSocket>> replacement;
    replacement.reserve(interfaces.size());
    for (const NetworkInterface& interface : interfaces) {
      const auto existing =
          std::find_if(state_->sockets.begin(), state_->sockets.end(),
                       [&interface](const std::shared_ptr<ScopeSocket>& scope) {
                         return scope->interface == interface;
                       });
      if (existing != state_->sockets.end()) {
        replacement.push_back(*existing);
        continue;
      }
      std::shared_ptr<ScopeSocket> opened =
          OpenSocket(state_->executor, interface, state_);
      if (opened == nullptr) {
        for (const std::shared_ptr<ScopeSocket>& scope : replacement) {
          if (std::find(state_->sockets.begin(), state_->sockets.end(), scope) ==
              state_->sockets.end()) {
            CloseSocket(scope);
          }
        }
        return false;
      }
      replacement.push_back(std::move(opened));
    }
    for (const std::shared_ptr<ScopeSocket>& scope : state_->sockets) {
      if (std::find(replacement.begin(), replacement.end(), scope) ==
          replacement.end()) {
        CloseSocket(scope);
      }
    }
    state_->sockets = std::move(replacement);
    return true;
  }

  [[nodiscard]] bool Send(const InterfaceScope& scope,
                          const std::span<const std::uint8_t> payload) override {
    if (!state_->active.load(std::memory_order_acquire) || payload.empty() ||
        payload.size() > kMaxDatagramSize) {
      return false;
    }
    const auto socket =
        std::find_if(state_->sockets.begin(), state_->sockets.end(),
                     [&scope](const std::shared_ptr<ScopeSocket>& current) {
                       return current->interface.scope == scope;
                     });
    if (socket == state_->sockets.end()) {
      return false;
    }
    asio::error_code error;
    const std::size_t sent = (*socket)->socket.send_to(
        asio::buffer(payload), GroupEndpoint(scope.family), 0, error);
    return !error && sent == payload.size();
  }

  void Stop() override {
    if (!state_->active.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    {
      const std::scoped_lock lock(state_->mutex);
      state_->handler = {};
    }
    for (const std::shared_ptr<ScopeSocket>& scope : state_->sockets) {
      CloseSocket(scope);
    }
    state_->sockets.clear();

    if (g_delivering_transport != state_.get()) {
      std::unique_lock lock(state_->mutex);
      state_->callbacks_drained.wait(
          lock, [this] { return state_->callbacks_in_flight == 0; });
    }
  }

 private:
  std::shared_ptr<TransportState> state_;
};

}  // namespace

std::unique_ptr<DatagramTransport> MakeAsioDatagramTransport(
    asio::any_io_executor executor) {
  return std::make_unique<AsioDatagramTransport>(std::move(executor));
}

}  // namespace xnn_transfer::core::discovery
