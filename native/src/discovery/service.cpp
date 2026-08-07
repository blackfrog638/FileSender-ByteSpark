#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "xnn_transfer/core/discovery/discovery.hpp"

namespace xnn_transfer::core::discovery {
namespace {

thread_local const void* g_publishing_service = nullptr;

enum class ServiceState {
  kCreated,
  kStarting,
  kRunning,
  kStopping,
  kStopped,
};

struct Publisher {
  NetworkInterface interface{};
  InstanceToken token{};
  std::uint64_t sequence{1};
  bool announced{};
  std::uint64_t last_send_ms{};
  std::uint64_t announce_due_ms{};
  std::uint64_t rotation_due_ms{};
  std::optional<std::uint64_t> update_due_ms{};
};

[[nodiscard]] std::uint64_t SaturatingAdd(const std::uint64_t value,
                                          const std::uint64_t increment) noexcept {
  if (increment > std::numeric_limits<std::uint64_t>::max() - value) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return value + increment;
}

[[nodiscard]] std::span<const std::uint8_t> LabelBytes(
    const std::string& label) noexcept {
  return {reinterpret_cast<const std::uint8_t*>(label.data()), label.size()};
}

[[nodiscard]] bool IsValidInterface(const NetworkInterface& interface) noexcept {
  if (interface.scope.generation == 0 || interface.system_index == 0 ||
      interface.scope.family != interface.local_address.family) {
    return false;
  }
  const std::uint8_t maximum_prefix =
      interface.scope.family == AddressFamily::kIpv4 ? 32U : 128U;
  return interface.prefix_length <= maximum_prefix;
}

[[nodiscard]] std::vector<NetworkInterface> CanonicalInterfaces(
    const std::span<const NetworkInterface> interfaces) {
  std::vector<NetworkInterface> result;
  result.reserve(kMaxScopes);
  for (const NetworkInterface& interface : interfaces) {
    if (!IsValidInterface(interface)) {
      continue;
    }
    const auto position = std::lower_bound(result.begin(), result.end(), interface);
    const auto same_scope = std::find_if(result.begin(), result.end(),
                                         [&interface](const NetworkInterface& current) {
                                           return current.scope == interface.scope;
                                         });
    if (same_scope != result.end()) {
      if (interface < *same_scope) {
        *same_scope = interface;
        std::sort(result.begin(), result.end());
      }
      continue;
    }
    if (result.size() < kMaxScopes) {
      result.insert(position, interface);
    } else if (position != result.end()) {
      result.insert(position, interface);
      result.pop_back();
    }
  }
  return result;
}

[[nodiscard]] std::vector<InterfaceScope> Scopes(
    const std::span<const NetworkInterface> interfaces) {
  std::vector<InterfaceScope> result;
  result.reserve(interfaces.size());
  for (const NetworkInterface& interface : interfaces) {
    result.push_back(interface.scope);
  }
  return result;
}

[[nodiscard]] bool ContainsScope(const std::span<const NetworkInterface> interfaces,
                                 const InterfaceScope& scope) noexcept {
  return std::any_of(
      interfaces.begin(), interfaces.end(),
      [&scope](const NetworkInterface& interface) { return interface.scope == scope; });
}

}  // namespace

class DiscoveryService::Impl final {
 public:
  Impl(DiscoveryConfig config,
       std::shared_ptr<const DisplayLabelValidator> label_validator,
       std::unique_ptr<MonotonicClock> clock, std::unique_ptr<EntropySource> entropy,
       std::unique_ptr<DiscoveryTimer> timer,
       std::unique_ptr<DatagramTransport> transport,
       std::unique_ptr<InterfaceMonitor> interface_monitor, EventHandler event_handler)
      : config_(std::move(config)),
        label_validator_(std::move(label_validator)),
        cache_(label_validator_),
        clock_(std::move(clock)),
        entropy_(std::move(entropy)),
        timer_(std::move(timer)),
        transport_(std::move(transport)),
        interface_monitor_(std::move(interface_monitor)),
        event_handler_(std::move(event_handler)) {}

  ~Impl() { Stop(); }

  [[nodiscard]] bool Start() {
    const std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    {
      const std::scoped_lock lock(mutex_);
      if (state_ != ServiceState::kCreated || !DependenciesValid() ||
          !ConfigValid(config_)) {
        return false;
      }
      state_ = ServiceState::kStarting;
    }

    if (!interface_monitor_->Start([this] { OnInterfaceChanged(); })) {
      FailStart();
      return false;
    }
    std::vector<NetworkInterface> interfaces =
        CanonicalInterfaces(interface_monitor_->Snapshot());
    const std::vector<InterfaceScope> scopes = Scopes(interfaces);
    const std::uint64_t now_ms = clock_->NowMs();
    if (!cache_.Start(scopes, now_ms) ||
        !transport_->Start(interfaces,
                           [this](const DatagramMetadata& metadata,
                                  const std::span<const std::uint8_t> payload) {
                             OnDatagram(metadata, payload);
                           })) {
      FailStart();
      return false;
    }

    interfaces = CanonicalInterfaces(interface_monitor_->Snapshot());
    const std::vector<InterfaceScope> refreshed_scopes = Scopes(interfaces);
    (void)cache_.ApplyInterfaceSnapshot(refreshed_scopes, now_ms);
    if (!transport_->Reconfigure(interfaces)) {
      FailStart();
      return false;
    }

    bool initialized = true;
    {
      const std::scoped_lock lock(mutex_);
      if (state_ != ServiceState::kStarting) {
        initialized = false;
      } else {
        interfaces_ = interfaces;
        publishers_.reserve(interfaces_.size());
        for (const NetworkInterface& interface : interfaces_) {
          std::optional<Publisher> publisher = CreatePublisherLocked(interface, now_ms);
          if (!publisher.has_value()) {
            initialized = false;
            break;
          }
          publishers_.push_back(std::move(*publisher));
        }
        state_ = initialized ? ServiceState::kRunning : ServiceState::kStopping;
        if (initialized) {
          for (Publisher& publisher : publishers_) {
            initialized = SendAnnounceLocked(publisher, now_ms) && initialized;
          }
          initialized = ScheduleNextLocked() && initialized;
        }
      }
    }
    if (!initialized) {
      FailStart();
      return false;
    }
    return true;
  }

  void Stop() {
    const std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    StopWithLifecycleLock();
  }

  [[nodiscard]] bool Wake() {
    std::vector<CandidateEvent> events;
    bool success = true;
    {
      const std::scoped_lock lifecycle_lock(lifecycle_mutex_);
      {
        const std::scoped_lock lock(mutex_);
        if (state_ != ServiceState::kRunning) {
          return false;
        }
      }
      const std::vector<NetworkInterface> interfaces =
          CanonicalInterfaces(interface_monitor_->Snapshot());
      {
        const std::scoped_lock lock(mutex_);
        const std::uint64_t now_ms = clock_->NowMs();
        const std::vector<InterfaceScope> scopes = Scopes(interfaces);
        events = cache_.Wake(scopes, now_ms);
        success = transport_->Reconfigure(interfaces);
        interfaces_ = interfaces;
        publishers_.clear();
        publishers_.reserve(interfaces_.size());
        for (const NetworkInterface& interface : interfaces_) {
          std::optional<Publisher> publisher = CreatePublisherLocked(interface, now_ms);
          if (!publisher.has_value()) {
            success = false;
            break;
          }
          publishers_.push_back(std::move(*publisher));
        }
        if (success) {
          for (Publisher& publisher : publishers_) {
            success = SendAnnounceLocked(publisher, now_ms) && success;
          }
          success = ScheduleNextLocked() && success;
        }
      }
      if (!success) {
        StopWithLifecycleLock();
      }
    }
    Publish(events);
    return success;
  }

  [[nodiscard]] bool UpdateAdvertisement(DiscoveryConfig config) {
    const std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    if (!ConfigValid(config)) {
      return false;
    }
    const std::scoped_lock lock(mutex_);
    if (state_ != ServiceState::kRunning) {
      return false;
    }
    if (config.service_port == config_.service_port &&
        config.display_label == config_.display_label) {
      return true;
    }
    config_ = std::move(config);
    const std::uint64_t now_ms = clock_->NowMs();
    for (Publisher& publisher : publishers_) {
      const std::uint64_t earliest =
          SaturatingAdd(publisher.last_send_ms, kImmediateUpdateIntervalMs);
      const std::uint64_t due = std::max(now_ms, earliest);
      if (!publisher.update_due_ms.has_value() || due < *publisher.update_due_ms) {
        publisher.update_due_ms = due;
      }
    }
    return ScheduleNextLocked();
  }

  [[nodiscard]] std::vector<Candidate> Snapshot() const { return cache_.Snapshot(); }

  [[nodiscard]] bool running() const {
    const std::scoped_lock lock(mutex_);
    return state_ == ServiceState::kRunning;
  }

 private:
  [[nodiscard]] bool DependenciesValid() const noexcept {
    return label_validator_ != nullptr && clock_ != nullptr && entropy_ != nullptr &&
           timer_ != nullptr && transport_ != nullptr && interface_monitor_ != nullptr;
  }

  [[nodiscard]] bool ConfigValid(const DiscoveryConfig& config) const noexcept {
    if (config.service_port == 0) {
      return false;
    }
    return config.display_label.empty() ||
           (label_validator_ != nullptr &&
            label_validator_->IsCanonical(LabelBytes(config.display_label)));
  }

  [[nodiscard]] bool GenerateToken(InstanceToken& token) noexcept {
    for (std::size_t attempt = 0; attempt < 4; ++attempt) {
      if (!entropy_->Fill(token)) {
        return false;
      }
      if (std::any_of(token.begin(), token.end(),
                      [](const std::uint8_t byte) { return byte != 0; })) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] std::optional<std::int64_t> Jitter() noexcept {
    constexpr std::uint64_t kRange = 2 * kPublisherJitterMs + 1;
    constexpr std::uint64_t kValues =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
    constexpr std::uint64_t kLimit = kValues - (kValues % kRange);
    std::array<std::uint8_t, 4> bytes{};
    for (std::size_t attempt = 0; attempt < 4; ++attempt) {
      if (!entropy_->Fill(bytes)) {
        return std::nullopt;
      }
      const std::uint32_t value = (static_cast<std::uint32_t>(bytes[0]) << 24U) |
                                  (static_cast<std::uint32_t>(bytes[1]) << 16U) |
                                  (static_cast<std::uint32_t>(bytes[2]) << 8U) |
                                  static_cast<std::uint32_t>(bytes[3]);
      if (value < kLimit) {
        return static_cast<std::int64_t>(static_cast<std::uint64_t>(value) % kRange) -
               static_cast<std::int64_t>(kPublisherJitterMs);
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<Publisher> CreatePublisherLocked(
      const NetworkInterface& interface, const std::uint64_t now_ms) noexcept {
    Publisher publisher{.interface = interface};
    if (!GenerateToken(publisher.token) ||
        !cache_.SetLocalToken(interface.scope, publisher.token)) {
      return std::nullopt;
    }
    publisher.rotation_due_ms = SaturatingAdd(now_ms, kPublisherRotationMs);
    return publisher;
  }

  [[nodiscard]] bool SchedulePeriodicLocked(Publisher& publisher,
                                            const std::uint64_t now_ms) noexcept {
    const std::optional<std::int64_t> jitter = Jitter();
    if (!jitter.has_value()) {
      return false;
    }
    const std::int64_t interval =
        static_cast<std::int64_t>(kPublisherIntervalMs) + *jitter;
    publisher.announce_due_ms =
        SaturatingAdd(now_ms, static_cast<std::uint64_t>(interval));
    return true;
  }

  [[nodiscard]] bool SendAnnounceLocked(Publisher& publisher,
                                        const std::uint64_t now_ms) {
    if (publisher.announced) {
      if (publisher.sequence == std::numeric_limits<std::uint64_t>::max()) {
        return RotateLocked(publisher, now_ms);
      }
      ++publisher.sequence;
    }
    EncodedDatagram datagram;
    if (!EncodeAdvertisement(MessageType::kAnnounce, publisher.sequence,
                             publisher.token, config_.service_port,
                             kAdvertisedTtlSeconds, LabelBytes(config_.display_label),
                             *label_validator_, datagram)) {
      return false;
    }
    (void)transport_->Send(publisher.interface.scope, datagram.payload());
    publisher.announced = true;
    publisher.last_send_ms = now_ms;
    publisher.update_due_ms.reset();
    return SchedulePeriodicLocked(publisher, now_ms);
  }

  void SendWithdrawalLocked(Publisher& publisher) {
    if (!publisher.announced ||
        publisher.sequence == std::numeric_limits<std::uint64_t>::max()) {
      return;
    }
    ++publisher.sequence;
    EncodedDatagram datagram;
    if (EncodeAdvertisement(MessageType::kWithdraw, publisher.sequence, publisher.token,
                            0, 0, {}, *label_validator_, datagram)) {
      (void)transport_->Send(publisher.interface.scope, datagram.payload());
    }
    publisher.announced = false;
  }

  [[nodiscard]] bool RotateLocked(Publisher& publisher, const std::uint64_t now_ms) {
    InstanceToken replacement{};
    if (!GenerateToken(replacement)) {
      return false;
    }
    SendWithdrawalLocked(publisher);
    publisher.token = replacement;
    publisher.sequence = 1;
    publisher.announced = false;
    publisher.rotation_due_ms = SaturatingAdd(now_ms, kPublisherRotationMs);
    if (!cache_.SetLocalToken(publisher.interface.scope, publisher.token)) {
      return false;
    }
    return SendAnnounceLocked(publisher, now_ms);
  }

  [[nodiscard]] bool ScheduleNextLocked() {
    if (state_ != ServiceState::kRunning) {
      return false;
    }
    std::optional<std::uint64_t> deadline = cache_.NextDeadlineMs();
    const auto consider = [&deadline](const std::uint64_t candidate) {
      if (!deadline.has_value() || candidate < *deadline) {
        deadline = candidate;
      }
    };
    for (const Publisher& publisher : publishers_) {
      consider(publisher.announce_due_ms);
      consider(publisher.rotation_due_ms);
      if (publisher.update_due_ms.has_value()) {
        consider(*publisher.update_due_ms);
      }
    }
    timer_->Cancel();
    return !deadline.has_value() ||
           timer_->ScheduleAt(*deadline, [this] { OnTimer(); });
  }

  void OnTimer() {
    std::vector<CandidateEvent> events;
    bool success = true;
    {
      const std::scoped_lock lock(mutex_);
      if (state_ != ServiceState::kRunning) {
        return;
      }
      const std::uint64_t now_ms = clock_->NowMs();
      events = cache_.Advance(now_ms);
      for (Publisher& publisher : publishers_) {
        if (now_ms >= publisher.rotation_due_ms) {
          success = RotateLocked(publisher, now_ms) && success;
          continue;
        }
        if (publisher.update_due_ms.has_value() && now_ms >= *publisher.update_due_ms) {
          success = SendAnnounceLocked(publisher, now_ms) && success;
        }
        if (now_ms >= publisher.announce_due_ms) {
          success = SendAnnounceLocked(publisher, now_ms) && success;
        }
      }
      success = ScheduleNextLocked() && success;
    }
    Publish(events);
    if (!success) {
      Stop();
    }
  }

  void OnDatagram(const DatagramMetadata& metadata,
                  const std::span<const std::uint8_t> payload) {
    std::vector<CandidateEvent> events;
    bool scheduled = true;
    {
      const std::scoped_lock lock(mutex_);
      if (state_ != ServiceState::kRunning) {
        return;
      }
      ReceiveResult result = cache_.Receive(metadata, payload, clock_->NowMs());
      events = std::move(result.events);
      scheduled = ScheduleNextLocked();
    }
    Publish(events);
    if (!scheduled) {
      Stop();
    }
  }

  void OnInterfaceChanged() {
    std::vector<CandidateEvent> events;
    {
      const std::scoped_lock lifecycle_lock(lifecycle_mutex_);
      {
        const std::scoped_lock lock(mutex_);
        if (state_ != ServiceState::kRunning) {
          return;
        }
      }
      if (!RefreshInterfacesLockedLifecycle(events)) {
        StopWithLifecycleLock();
      }
    }
    Publish(events);
  }

  [[nodiscard]] bool RefreshInterfacesLockedLifecycle(
      std::vector<CandidateEvent>& events) {
    const std::vector<NetworkInterface> interfaces =
        CanonicalInterfaces(interface_monitor_->Snapshot());
    bool success = true;
    {
      const std::scoped_lock lock(mutex_);
      if (state_ != ServiceState::kRunning) {
        return true;
      }
      const std::uint64_t now_ms = clock_->NowMs();
      for (Publisher& publisher : publishers_) {
        if (!ContainsScope(interfaces, publisher.interface.scope)) {
          SendWithdrawalLocked(publisher);
        }
      }

      const std::vector<InterfaceScope> scopes = Scopes(interfaces);
      events = cache_.ApplyInterfaceSnapshot(scopes, now_ms);
      success = transport_->Reconfigure(interfaces);

      std::vector<Publisher> replacement;
      replacement.reserve(interfaces.size());
      for (const NetworkInterface& interface : interfaces) {
        const auto existing =
            std::find_if(publishers_.begin(), publishers_.end(),
                         [&interface](const Publisher& publisher) {
                           return publisher.interface.scope == interface.scope;
                         });
        if (existing != publishers_.end()) {
          Publisher preserved = *existing;
          preserved.interface = interface;
          replacement.push_back(std::move(preserved));
          continue;
        }
        std::optional<Publisher> publisher = CreatePublisherLocked(interface, now_ms);
        if (!publisher.has_value()) {
          success = false;
          break;
        }
        replacement.push_back(std::move(*publisher));
        success = SendAnnounceLocked(replacement.back(), now_ms) && success;
      }
      interfaces_ = interfaces;
      publishers_ = std::move(replacement);
      success = ScheduleNextLocked() && success;
    }
    return success;
  }

  void Publish(const std::span<const CandidateEvent> events) {
    for (const CandidateEvent& event : events) {
      EventHandler handler;
      {
        const std::scoped_lock lock(mutex_);
        if (state_ != ServiceState::kRunning || !event_handler_) {
          return;
        }
        handler = event_handler_;
        ++callbacks_in_flight_;
      }

      const void* previous = g_publishing_service;
      g_publishing_service = this;
      try {
        handler(event);
      } catch (...) {
      }
      g_publishing_service = previous;

      {
        const std::scoped_lock lock(mutex_);
        --callbacks_in_flight_;
        callbacks_drained_.notify_all();
      }
    }
  }

  void FailStart() {
    {
      const std::scoped_lock lock(mutex_);
      state_ = ServiceState::kStopping;
    }
    timer_->Stop();
    interface_monitor_->Stop();
    transport_->Stop();
    cache_.Stop();
    const std::scoped_lock lock(mutex_);
    publishers_.clear();
    interfaces_.clear();
    state_ = ServiceState::kStopped;
  }

  void StopWithLifecycleLock() {
    {
      const std::scoped_lock lock(mutex_);
      if (state_ == ServiceState::kCreated) {
        state_ = ServiceState::kStopped;
        return;
      }
      if (state_ == ServiceState::kStopped) {
        return;
      }
      state_ = ServiceState::kStopping;
      for (Publisher& publisher : publishers_) {
        SendWithdrawalLocked(publisher);
      }
    }

    timer_->Stop();
    interface_monitor_->Stop();
    transport_->Stop();
    cache_.Stop();

    std::unique_lock lock(mutex_);
    publishers_.clear();
    interfaces_.clear();
    state_ = ServiceState::kStopped;
    if (g_publishing_service != this) {
      callbacks_drained_.wait(lock, [this] { return callbacks_in_flight_ == 0; });
    }
  }

  mutable std::mutex lifecycle_mutex_;
  mutable std::mutex mutex_;
  std::condition_variable callbacks_drained_;
  ServiceState state_{ServiceState::kCreated};
  DiscoveryConfig config_;
  std::shared_ptr<const DisplayLabelValidator> label_validator_;
  DiscoveryCache cache_;
  std::unique_ptr<MonotonicClock> clock_;
  std::unique_ptr<EntropySource> entropy_;
  std::unique_ptr<DiscoveryTimer> timer_;
  std::unique_ptr<DatagramTransport> transport_;
  std::unique_ptr<InterfaceMonitor> interface_monitor_;
  EventHandler event_handler_;
  std::vector<NetworkInterface> interfaces_;
  std::vector<Publisher> publishers_;
  std::size_t callbacks_in_flight_{};
};

DiscoveryService::DiscoveryService(
    DiscoveryConfig config,
    std::shared_ptr<const DisplayLabelValidator> label_validator,
    std::unique_ptr<MonotonicClock> clock, std::unique_ptr<EntropySource> entropy,
    std::unique_ptr<DiscoveryTimer> timer, std::unique_ptr<DatagramTransport> transport,
    std::unique_ptr<InterfaceMonitor> interface_monitor, EventHandler event_handler)
    : impl_(std::make_unique<Impl>(
          std::move(config), std::move(label_validator), std::move(clock),
          std::move(entropy), std::move(timer), std::move(transport),
          std::move(interface_monitor), std::move(event_handler))) {}

DiscoveryService::~DiscoveryService() = default;

bool DiscoveryService::Start() { return impl_->Start(); }

void DiscoveryService::Stop() { impl_->Stop(); }

bool DiscoveryService::Wake() { return impl_->Wake(); }

bool DiscoveryService::UpdateAdvertisement(DiscoveryConfig config) {
  return impl_->UpdateAdvertisement(std::move(config));
}

std::vector<Candidate> DiscoveryService::Snapshot() const { return impl_->Snapshot(); }

bool DiscoveryService::running() const { return impl_->running(); }

}  // namespace xnn_transfer::core::discovery
