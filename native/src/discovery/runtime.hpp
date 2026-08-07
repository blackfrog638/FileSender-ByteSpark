#ifndef XNN_TRANSFER_DISCOVERY_RUNTIME_HPP_
#define XNN_TRANSFER_DISCOVERY_RUNTIME_HPP_

#include <asio/any_io_executor.hpp>
#include <memory>

#include "xnn_transfer/core/discovery/discovery.hpp"

namespace xnn_transfer::core::discovery {

[[nodiscard]] std::unique_ptr<MonotonicClock> MakeSteadyMonotonicClock();
[[nodiscard]] std::unique_ptr<EntropySource> MakeOpenSslEntropySource();
[[nodiscard]] std::unique_ptr<DiscoveryTimer> MakeAsioDiscoveryTimer(
    asio::any_io_executor executor);
[[nodiscard]] std::unique_ptr<DatagramTransport> MakeAsioDatagramTransport(
    asio::any_io_executor executor);
[[nodiscard]] std::unique_ptr<InterfaceMonitor> MakeSystemInterfaceMonitor(
    asio::any_io_executor executor);

}  // namespace xnn_transfer::core::discovery

#endif  // XNN_TRANSFER_DISCOVERY_RUNTIME_HPP_
