#include "runtime.hpp"

#include <array>
#include <asio/io_context.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "interface_registry.hpp"
#include "xnn_transfer/core/discovery/discovery.hpp"

namespace {

using xnn_transfer::core::discovery::AddressFamily;
using xnn_transfer::core::discovery::CandidateEvent;
using xnn_transfer::core::discovery::DatagramMetadata;
using xnn_transfer::core::discovery::DatagramTransport;
using xnn_transfer::core::discovery::DiscoveryConfig;
using xnn_transfer::core::discovery::DiscoveryService;
using xnn_transfer::core::discovery::DiscoveryTimer;
using xnn_transfer::core::discovery::DisplayLabelValidator;
using xnn_transfer::core::discovery::EncodeAdvertisement;
using xnn_transfer::core::discovery::EncodedDatagram;
using xnn_transfer::core::discovery::EntropySource;
using xnn_transfer::core::discovery::EventKind;
using xnn_transfer::core::discovery::ExpiryReason;
using xnn_transfer::core::discovery::InstanceToken;
using xnn_transfer::core::discovery::InterfaceGenerationRegistry;
using xnn_transfer::core::discovery::InterfaceMonitor;
using xnn_transfer::core::discovery::InterfaceScope;
using xnn_transfer::core::discovery::IpAddress;
using xnn_transfer::core::discovery::kAdvertisedTtlSeconds;
using xnn_transfer::core::discovery::kDiscoveryPort;
using xnn_transfer::core::discovery::kMaxScopes;
using xnn_transfer::core::discovery::kPublisherIntervalMs;
using xnn_transfer::core::discovery::kPublisherJitterMs;
using xnn_transfer::core::discovery::MakeAsioDatagramTransport;
using xnn_transfer::core::discovery::MakeAsioDiscoveryTimer;
using xnn_transfer::core::discovery::MakeOpenSslEntropySource;
using xnn_transfer::core::discovery::MakeSteadyMonotonicClock;
using xnn_transfer::core::discovery::MakeSystemInterfaceMonitor;
using xnn_transfer::core::discovery::MakeUtf8procDisplayLabelValidator;
using xnn_transfer::core::discovery::MessageType;
using xnn_transfer::core::discovery::MonotonicClock;
using xnn_transfer::core::discovery::NetworkInterface;
using xnn_transfer::core::discovery::ParseAdvertisement;
using xnn_transfer::core::discovery::RawNetworkInterface;

int failures = 0;

void Expect(const bool condition, const std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

[[nodiscard]] std::span<const std::uint8_t> Bytes(const std::string_view value) {
  return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

InstanceToken Token(const std::uint8_t seed) {
  InstanceToken token{};
  for (std::size_t index = 0; index < token.size(); ++index) {
    token[index] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(seed) +
                                             static_cast<std::uint16_t>(index));
  }
  return token;
}

NetworkInterface Interface(const std::uint64_t generation = 1,
                           const std::uint32_t system_index = 7,
                           const std::uint8_t address_suffix = 20) {
  return NetworkInterface{
      .scope = InterfaceScope{.generation = generation, .family = AddressFamily::kIpv4},
      .system_index = system_index,
      .local_address = IpAddress::V4({192, 0, 2, address_suffix}),
      .prefix_length = 24};
}

DatagramMetadata Metadata(const InterfaceScope& scope) {
  return DatagramMetadata{.observer = scope,
                          .source = IpAddress::V4({192, 0, 2, 90}),
                          .destination = IpAddress::V4({239, 255, 88, 78}),
                          .destination_port = kDiscoveryPort};
}

EncodedDatagram RemoteAnnouncement(const DisplayLabelValidator& validator,
                                   const std::uint64_t sequence = 1) {
  EncodedDatagram output;
  Expect(EncodeAdvertisement(MessageType::kAnnounce, sequence, Token(90), 45'879,
                             kAdvertisedTtlSeconds, Bytes("Remote"), validator, output),
         "remote announcement encodes");
  return output;
}

class FakeClock final : public MonotonicClock {
 public:
  explicit FakeClock(const std::uint64_t now_ms) : now_ms_(now_ms) {}

  [[nodiscard]] std::uint64_t NowMs() const noexcept override {
    return now_ms_.load(std::memory_order_acquire);
  }

  void Set(const std::uint64_t now_ms) noexcept {
    now_ms_.store(now_ms, std::memory_order_release);
  }

 private:
  std::atomic<std::uint64_t> now_ms_;
};

class FakeEntropy final : public EntropySource {
 public:
  [[nodiscard]] bool Fill(const std::span<std::uint8_t> output) noexcept override {
    for (std::uint8_t& byte : output) {
      byte = next_;
      ++next_;
      if (next_ == 0) {
        next_ = 1;
      }
    }
    return true;
  }

 private:
  std::uint8_t next_{1};
};

class FakeTimer final : public DiscoveryTimer {
 public:
  [[nodiscard]] bool ScheduleAt(const std::uint64_t deadline_ms,
                                Handler handler) override {
    const std::scoped_lock lock(mutex_);
    if (!active_ || !handler) {
      return false;
    }
    deadline_ms_ = deadline_ms;
    handler_ = std::move(handler);
    return true;
  }

  void Cancel() override {
    const std::scoped_lock lock(mutex_);
    handler_ = {};
  }

  void Stop() override {
    const std::scoped_lock lock(mutex_);
    active_ = false;
    handler_ = {};
  }

  [[nodiscard]] std::uint64_t deadline_ms() const {
    const std::scoped_lock lock(mutex_);
    return deadline_ms_;
  }

  [[nodiscard]] Handler Capture() const {
    const std::scoped_lock lock(mutex_);
    return handler_;
  }

  [[nodiscard]] bool Fire(FakeClock& clock) {
    Handler handler;
    std::uint64_t deadline = 0;
    {
      const std::scoped_lock lock(mutex_);
      if (!handler_) {
        return false;
      }
      handler = std::move(handler_);
      deadline = deadline_ms_;
    }
    clock.Set(deadline);
    handler();
    return true;
  }

 private:
  mutable std::mutex mutex_;
  bool active_{true};
  std::uint64_t deadline_ms_{};
  Handler handler_{};
};

struct SentDatagram {
  InterfaceScope scope{};
  std::vector<std::uint8_t> payload{};
};

class FakeTransport final : public DatagramTransport {
 public:
  [[nodiscard]] bool Start(const std::span<const NetworkInterface> interfaces,
                           ReceiveHandler receive_handler) override {
    const std::scoped_lock lock(mutex_);
    if (active_ || !receive_handler) {
      return false;
    }
    active_ = true;
    interfaces_.assign(interfaces.begin(), interfaces.end());
    handler_ = std::move(receive_handler);
    return true;
  }

  [[nodiscard]] bool Reconfigure(
      const std::span<const NetworkInterface> interfaces) override {
    const std::scoped_lock lock(mutex_);
    if (!active_) {
      return false;
    }
    interfaces_.assign(interfaces.begin(), interfaces.end());
    ++reconfigure_count_;
    return true;
  }

  [[nodiscard]] bool Send(const InterfaceScope& scope,
                          const std::span<const std::uint8_t> payload) override {
    const std::scoped_lock lock(mutex_);
    if (!active_) {
      return false;
    }
    sent_.push_back(SentDatagram{
        .scope = scope,
        .payload = std::vector<std::uint8_t>(payload.begin(), payload.end())});
    return true;
  }

  void Stop() override {
    const std::scoped_lock lock(mutex_);
    active_ = false;
    handler_ = {};
    interfaces_.clear();
  }

  void Deliver(const DatagramMetadata& metadata,
               const std::span<const std::uint8_t> payload) {
    ReceiveHandler handler = Capture();
    if (handler) {
      handler(metadata, payload);
    }
  }

  [[nodiscard]] ReceiveHandler Capture() const {
    const std::scoped_lock lock(mutex_);
    return handler_;
  }

  [[nodiscard]] std::vector<SentDatagram> sent() const {
    const std::scoped_lock lock(mutex_);
    return sent_;
  }

  [[nodiscard]] std::vector<NetworkInterface> interfaces() const {
    const std::scoped_lock lock(mutex_);
    return interfaces_;
  }

  [[nodiscard]] std::size_t reconfigure_count() const {
    const std::scoped_lock lock(mutex_);
    return reconfigure_count_;
  }

 private:
  mutable std::mutex mutex_;
  bool active_{};
  ReceiveHandler handler_{};
  std::vector<NetworkInterface> interfaces_{};
  std::vector<SentDatagram> sent_{};
  std::size_t reconfigure_count_{};
};

class FakeMonitor final : public InterfaceMonitor {
 public:
  explicit FakeMonitor(std::vector<NetworkInterface> interfaces)
      : interfaces_(std::move(interfaces)) {}

  [[nodiscard]] std::vector<NetworkInterface> Snapshot() override {
    const std::scoped_lock lock(mutex_);
    ++snapshot_count_;
    return interfaces_;
  }

  [[nodiscard]] bool Start(ChangeHandler change_handler) override {
    const std::scoped_lock lock(mutex_);
    if (active_ || !change_handler) {
      return false;
    }
    active_ = true;
    handler_ = std::move(change_handler);
    return true;
  }

  void Stop() override {
    const std::scoped_lock lock(mutex_);
    active_ = false;
    handler_ = {};
  }

  void SetInterfaces(std::vector<NetworkInterface> interfaces) {
    const std::scoped_lock lock(mutex_);
    interfaces_ = std::move(interfaces);
  }

  void EmitChange() {
    ChangeHandler handler = Capture();
    if (handler) {
      handler();
    }
  }

  [[nodiscard]] ChangeHandler Capture() const {
    const std::scoped_lock lock(mutex_);
    return handler_;
  }

  [[nodiscard]] std::size_t snapshot_count() const {
    const std::scoped_lock lock(mutex_);
    return snapshot_count_;
  }

 private:
  mutable std::mutex mutex_;
  bool active_{};
  ChangeHandler handler_{};
  std::vector<NetworkInterface> interfaces_{};
  std::size_t snapshot_count_{};
};

struct Harness {
  std::shared_ptr<const DisplayLabelValidator> validator;
  FakeClock* clock{};
  FakeTimer* timer{};
  FakeTransport* transport{};
  FakeMonitor* monitor{};
  std::vector<CandidateEvent> events;
  std::unique_ptr<DiscoveryService> service;
};

std::unique_ptr<Harness> MakeHarness(
    std::vector<NetworkInterface> interfaces = {Interface()},
    DiscoveryService::EventHandler event_handler = {}) {
  auto harness = std::make_unique<Harness>();
  harness->validator = MakeUtf8procDisplayLabelValidator();
  auto clock = std::make_unique<FakeClock>(1'000);
  auto timer = std::make_unique<FakeTimer>();
  auto transport = std::make_unique<FakeTransport>();
  auto monitor = std::make_unique<FakeMonitor>(std::move(interfaces));
  harness->clock = clock.get();
  harness->timer = timer.get();
  harness->transport = transport.get();
  harness->monitor = monitor.get();
  Harness* output = harness.get();
  if (!event_handler) {
    event_handler = [output](const CandidateEvent& event) {
      output->events.push_back(event);
    };
  }
  harness->service = std::make_unique<DiscoveryService>(
      DiscoveryConfig{.service_port = 45'880}, harness->validator, std::move(clock),
      std::make_unique<FakeEntropy>(), std::move(timer), std::move(transport),
      std::move(monitor), std::move(event_handler));
  return harness;
}

void TestUnicodeAndEncoding() {
  const auto validator = MakeUtf8procDisplayLabelValidator();
  Expect(validator != nullptr, "utf8proc validator is available");
  Expect(validator->IsCanonical(Bytes("Desk")), "ascii label is canonical");
  Expect(validator->IsCanonical(Bytes("Caf\xc3\xa9")),
         "precomposed nfc label is canonical");
  Expect(!validator->IsCanonical(Bytes("Cafe\xcc\x81")),
         "decomposed label is rejected");
  Expect(!validator->IsCanonical(std::array<std::uint8_t, 2>{0xc0, 0xaf}),
         "overlong utf8 is rejected");
  Expect(!validator->IsCanonical(Bytes("\xc2\xa0"
                                       "Desk")),
         "leading unicode whitespace is rejected");
  Expect(!validator->IsCanonical(Bytes("Desk\xe2\x80\x8b")),
         "format code point is rejected");
  Expect(!validator->IsCanonical(Bytes("Desk\xe2\x80\xa8")),
         "line separator is rejected");
  const std::string too_many_scalars(65, 'a');
  Expect(!validator->IsCanonical(Bytes(too_many_scalars)), "scalar limit is enforced");

  EncodedDatagram encoded;
  Expect(EncodeAdvertisement(MessageType::kAnnounce, 7, Token(1), 45'879,
                             kAdvertisedTtlSeconds, Bytes("Desk"), *validator, encoded),
         "canonical announcement encodes");
  const auto parsed = ParseAdvertisement(encoded.payload(), *validator);
  Expect(parsed.ok(), "encoded announcement round trips through parser");
  Expect(parsed.advertisement.sequence == 7 &&
             parsed.advertisement.service_port == 45'879 &&
             parsed.advertisement.display_label().size() == 4,
         "round trip preserves sender fields");

  EncodedDatagram withdrawal;
  Expect(EncodeAdvertisement(MessageType::kWithdraw, 8, Token(1), 0, 0, {}, *validator,
                             withdrawal),
         "canonical withdrawal encodes");
  Expect(ParseAdvertisement(withdrawal.payload(), *validator).ok(),
         "encoded withdrawal round trips through parser");
}

void TestInterfaceGenerationRegistry() {
  InterfaceGenerationRegistry registry;
  const RawNetworkInterface first{.system_index = 7,
                                  .family = AddressFamily::kIpv4,
                                  .local_address = IpAddress::V4({192, 0, 2, 20}),
                                  .prefix_length = 24};
  auto snapshot = registry.Update(std::span(&first, 1));
  Expect(snapshot.size() == 1 && snapshot[0].scope.generation != 0,
         "first interface receives a generation");
  const std::uint64_t generation = snapshot[0].scope.generation;
  snapshot = registry.Update(std::span(&first, 1));
  Expect(snapshot[0].scope.generation == generation,
         "unchanged interface preserves generation");
  Expect(registry.Update({}).empty(), "removed interface leaves active set");
  snapshot = registry.Update(std::span(&first, 1));
  Expect(snapshot[0].scope.generation != generation,
         "re-added interface receives a fresh generation");

  std::vector<RawNetworkInterface> hostile;
  for (std::uint32_t index = 40; index > 0; --index) {
    hostile.push_back(RawNetworkInterface{
        .system_index = index,
        .family = AddressFamily::kIpv4,
        .local_address = IpAddress::V4({192, 0, 2, static_cast<std::uint8_t>(index)}),
        .prefix_length = 24});
  }
  snapshot = registry.Update(hostile);
  Expect(snapshot.size() == kMaxScopes, "registry applies the hard interface limit");
  Expect(
      snapshot.front().system_index == 1 && snapshot.back().system_index == kMaxScopes,
      "registry keeps the stable lowest interface ordering");
}

void TestPublisherLifecycleAndUpdate() {
  auto harness = MakeHarness();
  Expect(harness->service->Start(), "service starts with deterministic fakes");
  Expect(harness->service->running(), "service reports running");
  Expect(harness->monitor->snapshot_count() == 2,
         "startup takes a final repair snapshot");
  auto sent = harness->transport->sent();
  Expect(sent.size() == 1, "startup sends one announcement per scope");
  auto parsed = ParseAdvertisement(sent.front().payload, *harness->validator);
  Expect(parsed.ok() && parsed.advertisement.type == MessageType::kAnnounce &&
             parsed.advertisement.sequence == 1 &&
             parsed.advertisement.ttl_seconds == kAdvertisedTtlSeconds,
         "startup announcement uses sequence one and ttl fifteen");
  const std::uint64_t first_deadline = harness->timer->deadline_ms();
  Expect(first_deadline >= 1'000 + kPublisherIntervalMs - kPublisherJitterMs &&
             first_deadline <= 1'000 + kPublisherIntervalMs + kPublisherJitterMs,
         "periodic announcement deadline stays inside jitter bounds");

  Expect(harness->service->UpdateAdvertisement(
             DiscoveryConfig{.service_port = 45'881, .display_label = "Desk"}),
         "visible advertisement update is accepted");
  Expect(harness->timer->deadline_ms() == 2'000,
         "visible update is rate limited to one second");
  Expect(harness->timer->Fire(*harness->clock), "visible update timer fires");
  sent = harness->transport->sent();
  parsed = ParseAdvertisement(sent.back().payload, *harness->validator);
  Expect(parsed.ok() && parsed.advertisement.sequence == 2 &&
             parsed.advertisement.service_port == 45'881 &&
             parsed.advertisement.display_label().size() == 4,
         "visible update sends a new sequence with changed fields");

  harness->service->Stop();
  sent = harness->transport->sent();
  parsed = ParseAdvertisement(sent.back().payload, *harness->validator);
  Expect(parsed.ok() && parsed.advertisement.type == MessageType::kWithdraw &&
             parsed.advertisement.sequence == 3,
         "graceful stop sends a final withdrawal");
  Expect(!harness->service->running(), "stop is terminal");
}

void TestReceiveExpiryInterfaceAndWake() {
  auto harness = MakeHarness();
  Expect(harness->service->Start(), "receive service starts");
  const EncodedDatagram remote = RemoteAnnouncement(*harness->validator);
  harness->transport->Deliver(Metadata(Interface().scope), remote.payload());
  Expect(harness->events.size() == 1 &&
             harness->events.back().kind == EventKind::kAppeared,
         "remote announcement publishes appeared");
  Expect(harness->service->Snapshot().size() == 1, "remote announcement enters cache");

  const std::uint64_t candidate_deadline =
      harness->service->Snapshot().front().deadline_ms;
  for (std::size_t step = 0; step < 8 && harness->clock->NowMs() < candidate_deadline;
       ++step) {
    Expect(harness->timer->Fire(*harness->clock), "next deterministic timer fires");
  }
  Expect(!harness->events.empty() &&
             harness->events.back().kind == EventKind::kExpired &&
             harness->events.back().expiry_reason == ExpiryReason::kTtl,
         "timer-driven cache expiry publishes ttl reason");

  harness->transport->Deliver(Metadata(Interface().scope),
                              RemoteAnnouncement(*harness->validator, 2).payload());
  Expect(harness->service->Snapshot().size() == 1, "newer remote sequence reappears");
  const auto before_wake = harness->transport->sent();
  const auto first_local =
      ParseAdvertisement(before_wake.front().payload, *harness->validator);
  harness->clock->Set(20'000);
  Expect(harness->service->Wake(), "wake rebuilds current interfaces");
  const auto after_wake = harness->transport->sent();
  const auto new_local =
      ParseAdvertisement(after_wake.back().payload, *harness->validator);
  Expect(first_local.ok() && new_local.ok() &&
             first_local.advertisement.token != new_local.advertisement.token &&
             new_local.advertisement.sequence == 1,
         "wake rotates publisher token and restarts sequence");
  Expect(harness->events.back().expiry_reason == ExpiryReason::kWake,
         "wake expires candidates with wake reason");

  harness->transport->Deliver(Metadata(Interface().scope),
                              RemoteAnnouncement(*harness->validator, 3).payload());
  harness->monitor->SetInterfaces({});
  harness->monitor->EmitChange();
  Expect(harness->transport->interfaces().empty(),
         "interface removal reconfigures transport");
  Expect(harness->service->Snapshot().empty(),
         "interface removal clears its candidates");
  Expect(harness->events.back().expiry_reason == ExpiryReason::kInterfaceRemoved,
         "interface removal publishes its specific reason");
  harness->service->Stop();
}

void TestWakeBeforeStartHasNoPlatformSideEffect() {
  auto harness = MakeHarness();
  Expect(harness->monitor->snapshot_count() == 0,
         "created service has not queried interfaces");
  Expect(!harness->service->Wake(), "wake before start is rejected");
  Expect(harness->monitor->snapshot_count() == 0,
         "rejected wake does not query platform interfaces");
}

void TestStopWaitsForCallbackAndRejectsLateCompletions() {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;
  std::atomic<std::size_t> callback_count{0};
  auto harness = MakeHarness({Interface()}, [&](const CandidateEvent&) {
    callback_count.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lock(mutex);
    entered = true;
    condition.notify_all();
    condition.wait(lock, [&release] { return release; });
  });
  Expect(harness->service->Start(), "barrier service starts");

  const auto late_receive = harness->transport->Capture();
  const auto late_timer = harness->timer->Capture();
  const auto late_monitor = harness->monitor->Capture();
  const EncodedDatagram remote = RemoteAnnouncement(*harness->validator);
  std::thread receiver(
      [&] { late_receive(Metadata(Interface().scope), remote.payload()); });
  {
    std::unique_lock lock(mutex);
    condition.wait(lock, [&entered] { return entered; });
  }

  std::atomic<bool> stop_started{false};
  std::atomic<bool> stop_returned{false};
  std::thread stopper([&] {
    stop_started.store(true, std::memory_order_release);
    harness->service->Stop();
    stop_returned.store(true, std::memory_order_release);
  });
  while (!stop_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  Expect(!stop_returned.load(std::memory_order_acquire),
         "stop waits for an event callback already in flight");
  {
    const std::scoped_lock lock(mutex);
    release = true;
  }
  condition.notify_all();
  receiver.join();
  stopper.join();
  Expect(stop_returned.load(std::memory_order_acquire),
         "stop returns after callback drains");

  const std::size_t callbacks_after_stop =
      callback_count.load(std::memory_order_relaxed);
  const std::size_t snapshots_after_stop = harness->monitor->snapshot_count();
  late_receive(Metadata(Interface().scope), remote.payload());
  if (late_timer) {
    late_timer();
  }
  if (late_monitor) {
    late_monitor();
  }
  Expect(callback_count.load(std::memory_order_relaxed) == callbacks_after_stop,
         "late transport timer and monitor completions publish nothing");
  Expect(harness->monitor->snapshot_count() == snapshots_after_stop,
         "late interface completion does not query platform state");
  Expect(harness->service->Snapshot().empty(),
         "late completions cannot recreate cache state");
}

void TestReentrantStopFromLifecycleEvents() {
  bool stop_on_event = false;
  DiscoveryService* service = nullptr;
  std::size_t callback_count = 0;
  auto wake_harness = MakeHarness({Interface()}, [&](const CandidateEvent&) {
    ++callback_count;
    if (stop_on_event) {
      service->Stop();
    }
  });
  service = wake_harness->service.get();
  Expect(service->Start(), "reentrant wake service starts");
  wake_harness->transport->Deliver(
      Metadata(Interface().scope),
      RemoteAnnouncement(*wake_harness->validator).payload());
  stop_on_event = true;
  Expect(service->Wake(), "wake completes when its event callback requests stop");
  Expect(!service->running() && callback_count == 2,
         "wake callback stop crosses no held lifecycle lock");

  stop_on_event = false;
  callback_count = 0;
  auto interface_harness = MakeHarness({Interface()}, [&](const CandidateEvent&) {
    ++callback_count;
    if (stop_on_event) {
      service->Stop();
    }
  });
  service = interface_harness->service.get();
  Expect(service->Start(), "reentrant interface service starts");
  interface_harness->transport->Deliver(
      Metadata(Interface().scope),
      RemoteAnnouncement(*interface_harness->validator).payload());
  interface_harness->monitor->SetInterfaces({});
  stop_on_event = true;
  interface_harness->monitor->EmitChange();
  Expect(!service->running() && callback_count == 2,
         "interface callback stop crosses no held lifecycle lock");
}

void TestProductionRuntimePrimitives() {
  auto clock = MakeSteadyMonotonicClock();
  auto entropy = MakeOpenSslEntropySource();
  Expect(clock != nullptr && entropy != nullptr,
         "production clock and entropy factories succeed");
  std::array<std::uint8_t, 32> random{};
  Expect(entropy->Fill(random), "production entropy fills a bounded request");

  asio::io_context context;
  auto timer = MakeAsioDiscoveryTimer(context.get_executor());
  std::size_t timer_callbacks = 0;
  Expect(
      timer != nullptr &&
          timer->ScheduleAt(clock->NowMs(), [&timer_callbacks] { ++timer_callbacks; }),
      "production timer accepts a monotonic deadline");
  context.run();
  Expect(timer_callbacks == 1, "production timer publishes one completion");

  context.restart();
  Expect(timer->ScheduleAt(clock->NowMs() + 60'000,
                           [&timer_callbacks] { ++timer_callbacks; }),
         "production timer accepts a cancellable deadline");
  timer->Stop();
  context.run();
  Expect(timer_callbacks == 1, "stopped production timer suppresses late completion");
}

void TestSystemMonitorSmoke() {
  asio::io_context context;
  auto monitor = MakeSystemInterfaceMonitor(context.get_executor());
  Expect(monitor != nullptr, "system interface monitor factory succeeds");
  const auto first = monitor->Snapshot();
  const auto second = monitor->Snapshot();
  Expect(first.size() <= kMaxScopes && second.size() <= kMaxScopes,
         "system interface snapshots are bounded");
  for (const NetworkInterface& interface : second) {
    Expect(interface.scope.generation != 0 && interface.system_index != 0 &&
               interface.scope.family == interface.local_address.family,
           "system snapshot contains canonical project interface values");
  }
  std::atomic<std::size_t> changes{0};
  Expect(
      monitor->Start([&changes] { changes.fetch_add(1, std::memory_order_relaxed); }),
      "system interface notification subscription starts");
  monitor->Stop();

  auto transport = MakeAsioDatagramTransport(context.get_executor());
  Expect(transport != nullptr, "asio datagram transport factory succeeds");
  Expect(transport->Start(
             {}, [](const DatagramMetadata&, const std::span<const std::uint8_t>) {}),
         "asio transport accepts an empty bounded snapshot");
  const std::vector<NetworkInterface> too_many_interfaces(kMaxScopes + 1, Interface());
  Expect(!transport->Reconfigure(too_many_interfaces),
         "asio transport rejects snapshots above the scope ceiling");
  transport->Stop();
}

}  // namespace

int main() {
  TestUnicodeAndEncoding();
  TestInterfaceGenerationRegistry();
  TestPublisherLifecycleAndUpdate();
  TestReceiveExpiryInterfaceAndWake();
  TestWakeBeforeStartHasNoPlatformSideEffect();
  TestStopWaitsForCallbackAndRejectsLateCompletions();
  TestReentrantStopFromLifecycleEvents();
  TestProductionRuntimePrimitives();
  TestSystemMonitorSmoke();

  if (failures != 0) {
    std::cerr << failures << " discovery runtime assertions failed\n";
    return 1;
  }
  std::cout << "Native discovery runtime tests passed.\n";
  return 0;
}
