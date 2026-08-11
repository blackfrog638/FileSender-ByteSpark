#include <openssl/evp.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/post.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/steady_timer.hpp>
#include <asio/write.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "connection_runtime_internal.hpp"
#include "internal.hpp"

namespace xnn_transfer::core::session {
namespace {

using Tcp = asio::ip::tcp;
using TlsStream = asio::ssl::stream<Tcp::socket>;
using runtime_internal::CallbackDispatchResult;
using security::tls::AcceptedEstablishedTlsConnection;
using security::tls::AcceptedPairingTlsConnection;
using security::tls::OpenSslTlsContext;
using security::tls::Result;
using security::tls::SecurityError;
using security::tls::TlsEndpointRole;

constexpr std::chrono::seconds kRuntimeCommandTimeout(5);
constexpr std::size_t kEmergencyCompletionCapacity =
    kMaxAuthenticatedConnections * (kMaxEstablishedPendingWrites + 1U);
constexpr std::size_t kEmergencyEventCapacity = kEmergencyCompletionCapacity;
constexpr std::size_t kEmergencyNetworkCapacity = kMaxAuthenticatedConnections;
struct EmergencyNetworkEntry {
  const void* key{};
  std::optional<ConnectionId> pairing_key{};
  std::function<void()> callback{};
};
enum class RuntimeCommandState : std::uint8_t {
  kQueued,
  kRunning,
  kCancelled,
  kCompleted,
};

class RuntimeCommand final {
 public:
  [[nodiscard]] std::future<bool> Future() { return completion_.get_future(); }

  [[nodiscard]] bool Start() noexcept {
    RuntimeCommandState expected = RuntimeCommandState::kQueued;
    return state_.compare_exchange_strong(expected, RuntimeCommandState::kRunning,
                                          std::memory_order_acq_rel);
  }

  void Complete(const bool value) noexcept {
    state_.store(RuntimeCommandState::kCompleted, std::memory_order_release);
    Resolve(value);
  }

  void Cancel() noexcept {
    RuntimeCommandState expected = RuntimeCommandState::kQueued;
    if (state_.compare_exchange_strong(expected, RuntimeCommandState::kCancelled,
                                       std::memory_order_acq_rel)) {
      Resolve(false);
    }
  }

  [[nodiscard]] bool CancelIfQueued() noexcept {
    RuntimeCommandState expected = RuntimeCommandState::kQueued;
    if (!state_.compare_exchange_strong(expected, RuntimeCommandState::kCancelled,
                                        std::memory_order_acq_rel)) {
      return false;
    }
    Resolve(false);
    return true;
  }

 private:
  void Resolve(const bool value) noexcept {
    if (resolved_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    try {
      completion_.set_value(value);
    } catch (...) {
    }
  }

  std::atomic<RuntimeCommandState> state_{RuntimeCommandState::kQueued};
  std::atomic_bool resolved_{};
  std::promise<bool> completion_{};
};

thread_local const void* kActiveCallbackRuntime = nullptr;

[[nodiscard]] std::uint64_t NowMs() noexcept {
  const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  return value < 0 ? 0U : static_cast<std::uint64_t>(value);
}

[[nodiscard]] bool AllZero(const std::span<const std::uint8_t> bytes) noexcept {
  return std::all_of(bytes.begin(), bytes.end(),
                     [](const std::uint8_t value) { return value == 0U; });
}

[[nodiscard]] std::optional<Tcp::endpoint> ToAsioEndpoint(
    const NetworkEndpoint& endpoint) {
  if (endpoint.port == 0U) {
    return std::nullopt;
  }
  if (endpoint.family == NetworkAddressFamily::kIpv4) {
    if (endpoint.scope_id != 0U ||
        std::any_of(endpoint.address.begin() + 4U, endpoint.address.end(),
                    [](const std::uint8_t value) { return value != 0U; })) {
      return std::nullopt;
    }
    asio::ip::address_v4::bytes_type bytes{};
    std::copy_n(endpoint.address.begin(), bytes.size(), bytes.begin());
    const asio::ip::address_v4 address(bytes);
    if (address.is_unspecified() || address.is_multicast() ||
        address == asio::ip::address_v4::broadcast()) {
      return std::nullopt;
    }
    return Tcp::endpoint(address, endpoint.port);
  }
  if (endpoint.family == NetworkAddressFamily::kIpv6) {
    asio::ip::address_v6::bytes_type bytes{};
    std::copy(endpoint.address.begin(), endpoint.address.end(), bytes.begin());
    const asio::ip::address_v6 address(bytes, endpoint.scope_id);
    if (address.is_unspecified() || address.is_multicast() ||
        (address.is_link_local() && endpoint.scope_id == 0U) ||
        (!address.is_link_local() && endpoint.scope_id != 0U)) {
      return std::nullopt;
    }
    return Tcp::endpoint(address, endpoint.port);
  }
  return std::nullopt;
}

[[nodiscard]] SourceToken MakeSourceToken(const Tcp::endpoint& endpoint) noexcept {
  std::array<std::uint8_t, 21> encoded{};
  std::size_t encoded_size = 0;
  if (endpoint.address().is_v4()) {
    encoded[encoded_size++] = 4U;
    const auto bytes = endpoint.address().to_v4().to_bytes();
    std::copy(bytes.begin(), bytes.end(), encoded.begin() + encoded_size);
    encoded_size += bytes.size();
  } else {
    encoded[encoded_size++] = 6U;
    const auto address = endpoint.address().to_v6();
    const auto bytes = address.to_bytes();
    std::copy(bytes.begin(), bytes.end(), encoded.begin() + encoded_size);
    encoded_size += bytes.size();
    const std::uint32_t scope = address.scope_id();
    for (int shift = 24; shift >= 0; shift -= 8) {
      encoded[encoded_size++] =
          static_cast<std::uint8_t>(scope >> static_cast<unsigned int>(shift));
    }
  }

  std::array<std::uint8_t, 32> digest{};
  std::size_t digest_size = digest.size();
  SourceToken output{};
  if (EVP_Q_digest(nullptr, "SHA256", nullptr, encoded.data(), encoded_size,
                   digest.data(), &digest_size) == 1 &&
      digest_size == digest.size()) {
    std::copy_n(digest.begin(), output.size(), output.begin());
  }
  OPENSSL_cleanse(digest.data(), digest.size());
  return output;
}

[[nodiscard]] PairingUpdate ClosedUpdate(const PairingError error) {
  return PairingUpdate{
      .state = PairingState::kClosed,
      .error = error,
      .terminal = true,
  };
}

}  // namespace

NetworkEndpoint NetworkEndpoint::V4(const std::array<std::uint8_t, 4>& value,
                                    const std::uint16_t port) noexcept {
  NetworkEndpoint endpoint{.family = NetworkAddressFamily::kIpv4, .port = port};
  std::copy(value.begin(), value.end(), endpoint.address.begin());
  return endpoint;
}

NetworkEndpoint NetworkEndpoint::V6(const std::array<std::uint8_t, 16>& value,
                                    const std::uint32_t scope_id,
                                    const std::uint16_t port) noexcept {
  return NetworkEndpoint{
      .family = NetworkAddressFamily::kIpv6,
      .address = value,
      .scope_id = scope_id,
      .port = port,
  };
}

class AuthenticatedConnectionRuntime::Impl final
    : public std::enable_shared_from_this<AuthenticatedConnectionRuntime::Impl> {
 public:
  using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;

  struct TrackedEstablishedConnection {
    std::shared_ptr<AuthenticatedEstablishedConnection::RuntimeLease> lease{};
  };

  using PairingConnection = runtime_internal::PairingSocket;
  using HandshakeConnection = runtime_internal::TlsHandshake;
  using AdmissionBridge = runtime_internal::PairingAdmissionBridge;

  Impl(security::identity::IdentityRepository& repository,
       AuthenticatedConnectionRuntimeConfig config, PairingHandler pairing_handler,
       EstablishedHandler established_handler)
      : repository_(&repository),
        config_(std::move(config)),
        pairing_handler_(std::move(pairing_handler)),
        established_handler_(std::move(established_handler)) {}

  ~Impl() {
    Stop();
    if (thread_.joinable()) {
      if (thread_.get_id() == std::this_thread::get_id()) {
        thread_.detach();
      } else {
        thread_.join();
      }
    }
    if (callback_thread_.joinable()) {
      if (callback_thread_.get_id() == std::this_thread::get_id()) {
        callback_thread_.detach();
      } else {
        callback_thread_.join();
      }
    }
  }

  [[nodiscard]] bool Start() {
    std::unique_lock lifecycle_lock(lifecycle_mutex_);
    if (start_attempted_ || repository_ == nullptr || !repository_->ready() ||
        config_.tls_handshake_timeout_ms == 0U ||
        config_.tls_handshake_timeout_ms > kDefaultTlsHandshakeTimeoutMs ||
        config_.pairing_write_fragment_bytes == 0U ||
        config_.pairing_write_fragment_bytes > kMaxPairingFrameSize ||
        internal::ValidateOffer(config_.pairing_offer) != PairingError::kNone) {
      start_attempted_ = true;
      return false;
    }
    start_attempted_ = true;

    try {
      admission_ = AdmissionBridge::ProcessScoped();
    } catch (...) {
      return false;
    }
    const std::weak_ptr<Impl> weak_owner = weak_from_this();
    Result<OpenSslTlsContext> server;
    Result<OpenSslTlsContext> pairing_client;
    Result<OpenSslTlsContext> established_client;
    try {
      server = OpenSslTlsContext::CreateServerDispatcher(*repository_, [weak_owner] {
        const std::shared_ptr<Impl> owner = weak_owner.lock();
        return owner == nullptr || !owner->running()
                   ? 0U
                   : AdmissionBridge::WindowGeneration(owner->admission_, NowMs());
      });
      pairing_client = OpenSslTlsContext::Create(TlsEndpointRole::kClient, *repository_,
                                                 security::tls::kPairingAlpn);
      established_client = OpenSslTlsContext::Create(
          TlsEndpointRole::kClient, *repository_, security::tls::kEstablishedAlpn);
    } catch (...) {
      return RollbackStart(lifecycle_lock);
    }
    if (!server.ok() || !pairing_client.ok() || !established_client.ok()) {
      return RollbackStart(lifecycle_lock);
    }
    try {
      server_context_ = std::make_shared<OpenSslTlsContext>(std::move(*server.value));
      pairing_client_context_ =
          std::make_shared<OpenSslTlsContext>(std::move(*pairing_client.value));
      established_client_context_ =
          std::make_shared<OpenSslTlsContext>(std::move(*established_client.value));
      server_asio_context_ = MakeAsioContext(server_context_);
      pairing_client_asio_context_ = MakeAsioContext(pairing_client_context_);
      established_client_asio_context_ = MakeAsioContext(established_client_context_);
      acceptor_ = std::make_unique<Tcp::acceptor>(context_);
    } catch (...) {
      return RollbackStart(lifecycle_lock);
    }
    if (server_asio_context_ == nullptr || pairing_client_asio_context_ == nullptr ||
        established_client_asio_context_ == nullptr) {
      return RollbackStart(lifecycle_lock);
    }

    asio::error_code error;
    acceptor_->open(Tcp::v6(), error);
    if (error) {
      return RollbackStart(lifecycle_lock);
    }
#if defined(_WIN32)
    const int exclusive_address_use = 1;
    if (::setsockopt(acceptor_->native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive_address_use),
                     sizeof(exclusive_address_use)) != 0) {
      return RollbackStart(lifecycle_lock);
    }
#else
    acceptor_->set_option(Tcp::acceptor::reuse_address(true), error);
    if (error) {
      return RollbackStart(lifecycle_lock);
    }
#endif
    acceptor_->set_option(asio::ip::v6_only(false), error);
    if (error) {
      return RollbackStart(lifecycle_lock);
    }
    acceptor_->bind(Tcp::endpoint(Tcp::v6(), config_.listen_port), error);
    if (error) {
      return RollbackStart(lifecycle_lock);
    }
    acceptor_->listen(static_cast<int>(kMaxPendingTlsHandshakes), error);
    if (error) {
      return RollbackStart(lifecycle_lock);
    }
    listen_port_.store(acceptor_->local_endpoint(error).port(),
                       std::memory_order_release);
    if (error || listen_port_.load(std::memory_order_acquire) == 0U) {
      return RollbackStart(lifecycle_lock);
    }

    try {
      work_.emplace(asio::make_work_guard(context_));
      callback_work_.emplace(asio::make_work_guard(callback_context_));
      commands_open_.store(true, std::memory_order_release);
      callbacks_open_.store(true, std::memory_order_release);
      running_.store(true, std::memory_order_release);
      stopping_.store(false, std::memory_order_release);
      DoAccept();
      const std::shared_ptr<Impl> self = shared_from_this();
      callback_thread_ = std::thread([self] {
        kActiveCallbackRuntime = self.get();
        while (self->callbacks_open_.load(std::memory_order_acquire)) {
          try {
            self->callback_context_.run_for(std::chrono::milliseconds(10));
          } catch (...) {
            self->callbacks_open_.store(false, std::memory_order_release);
            self->callback_context_.stop();
          }
          self->DrainEmergencyCompletions(true);
          self->DrainEmergencyEvents(true);
          if (self->callbacks_open_.load(std::memory_order_acquire)) {
            self->callback_context_.restart();
          }
        }
        self->DrainEmergencyCompletions(false);
        self->DrainEmergencyEvents(false);
        DrainStoppedContext(self->callback_context_);
        kActiveCallbackRuntime = nullptr;
        {
          const std::scoped_lock lock(self->lifecycle_mutex_);
          self->callback_thread_exited_ = true;
        }
        self->stopped_cv_.notify_all();
      });
      thread_ = std::thread([self] {
        while (!self->stopping_.load(std::memory_order_acquire)) {
          try {
            self->context_.run_for(std::chrono::milliseconds(10));
          } catch (...) {
            self->stopping_.store(true, std::memory_order_release);
            self->ShutdownOwnedState();
            self->context_.stop();
          }
          self->DrainEmergencyNetwork(true);
          if (!self->stopping_.load(std::memory_order_acquire)) {
            self->context_.restart();
          }
        }
        self->DrainEmergencyNetwork(false);
        if (self->shutdown_started_) {
          DrainStoppedContext(self->context_);
        }
        self->running_.store(false, std::memory_order_release);
        {
          const std::scoped_lock lock(self->lifecycle_mutex_);
          self->thread_exited_ = true;
        }
        self->stopped_cv_.notify_all();
      });
      worker_id_ = thread_.get_id();
    } catch (...) {
      return RollbackStart(lifecycle_lock);
    }
    return true;
  }

  void Stop() {
    const bool in_callback = kActiveCallbackRuntime == this;
    if (in_callback && stopping_.load(std::memory_order_acquire)) {
      return;
    }
    std::unique_lock lifecycle_lock(lifecycle_mutex_);
    if (in_callback && stopping_.load(std::memory_order_acquire)) {
      return;
    }
    const bool on_callback =
        in_callback || (callback_thread_.joinable() &&
                        callback_thread_.get_id() == std::this_thread::get_id());
    if (!start_attempted_) {
      return;
    }
    if (rollback_in_progress_) {
      stopped_cv_.wait(lifecycle_lock, [this] { return !rollback_in_progress_; });
      return;
    }
    const bool on_worker = worker_id_ == std::this_thread::get_id();
    CancelRuntimeCommands();
    BeginCallbackShutdown();
    if (thread_exited_) {
      ShutdownOwnedState();
      if (thread_.joinable() && !on_worker) {
        std::thread joining = std::move(thread_);
        joining.join();
      }
      if (!on_worker) {
        DrainStoppedContext(context_);
      }
      FinishCallbackShutdown(lifecycle_lock, on_callback);
      if (!on_worker) {
        DrainStoppedContext(context_);
      }
      return;
    }
    if (!running_.load(std::memory_order_acquire)) {
      if (thread_.joinable() && !on_worker) {
        context_.stop();
        stopped_cv_.wait(lifecycle_lock, [this] { return thread_exited_; });
        ShutdownOwnedState();
        if (thread_.joinable()) {
          std::thread joining = std::move(thread_);
          joining.join();
        }
        DrainStoppedContext(context_);
      } else {
        ShutdownOwnedState();
      }
      FinishCallbackShutdown(lifecycle_lock, on_callback);
      if (!on_worker) {
        DrainStoppedContext(context_);
      }
      return;
    }

    if (on_callback) {
      callback_stop_waiting_for_worker_ = true;
    }
    if (!stopping_.exchange(true, std::memory_order_acq_rel)) {
      if (on_worker) {
        ShutdownOwnedState();
      } else {
        context_.stop();
      }
    } else if (!on_worker) {
      context_.stop();
    }
    if (on_worker) {
      if (callback_stop_waiting_for_worker_) {
        return;
      }
      FinishCallbackShutdown(lifecycle_lock, false);
      return;
    }
    stopped_cv_.wait(lifecycle_lock, [this] { return thread_exited_; });
    if (on_callback) {
      callback_stop_waiting_for_worker_ = false;
    }
    ShutdownOwnedState();
    if (thread_.joinable()) {
      std::thread joining = std::move(thread_);
      joining.join();
    }
    DrainStoppedContext(context_);
    FinishCallbackShutdown(lifecycle_lock, on_callback);
    DrainStoppedContext(context_);
  }

  [[nodiscard]] bool running() const noexcept {
    return running_.load(std::memory_order_acquire) &&
           !stopping_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::uint16_t listen_port() const noexcept {
    return listen_port_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool OpenPairingWindow(const std::uint64_t duration_ms) {
    return RunBool(
        [this, duration_ms] { return admission_.OpenWindow(NowMs(), duration_ms); });
  }

  void ClosePairingWindow() {
    static_cast<void>(RunBool([this] {
      admission_.CloseWindow();
      return true;
    }));
  }

  [[nodiscard]] bool StartPairing(const NetworkEndpoint& endpoint,
                                  const std::uint64_t request_id,
                                  std::string peer_display_label) {
    const std::optional<Tcp::endpoint> target = ToAsioEndpoint(endpoint);
    if (!target.has_value() ||
        peer_display_label.size() > security::identity::kMaxDisplayLabelBytes) {
      return false;
    }
    return RunBool([this, target = *target, request_id,
                    peer_display_label = std::move(peer_display_label)]() mutable {
      return StartClientHandshake(target, HandshakeConnection::Mode::kClientPairing,
                                  request_id, std::move(peer_display_label),
                                  std::nullopt);
    });
  }

  [[nodiscard]] bool Decide(const AttemptHandle& attempt,
                            const security::tls::ConfirmationDecision decision) {
    if (AllZero(attempt)) {
      return false;
    }
    return RunBool([this, attempt, decision] {
      const auto found = std::find_if(
          pairing_connections_.begin(), pairing_connections_.end(),
          [&attempt](const std::shared_ptr<PairingConnection>& connection) {
            return connection->attempt_handle() == attempt;
          });
      if (found == pairing_connections_.end()) {
        return false;
      }
      (*found)->Decide(decision);
      return true;
    });
  }

  [[nodiscard]] bool Cancel(const AttemptHandle& attempt) {
    if (AllZero(attempt)) {
      return false;
    }
    return RunBool([this, attempt] {
      const auto found = std::find_if(
          pairing_connections_.begin(), pairing_connections_.end(),
          [&attempt](const std::shared_ptr<PairingConnection>& connection) {
            return connection->attempt_handle() == attempt;
          });
      if (found == pairing_connections_.end()) {
        return false;
      }
      (*found)->Cancel();
      return true;
    });
  }

  [[nodiscard]] bool OpenEstablished(const NetworkEndpoint& endpoint,
                                     const DeviceId& peer_device_id,
                                     const std::uint64_t request_id) {
    const std::optional<Tcp::endpoint> target = ToAsioEndpoint(endpoint);
    if (!target.has_value() || AllZero(peer_device_id)) {
      return false;
    }
    return RunBool([this, target = *target, peer_device_id, request_id] {
      return StartClientHandshake(target, HandshakeConnection::Mode::kClientEstablished,
                                  request_id, {}, peer_device_id);
    });
  }

 private:
  static void DrainStoppedContext(asio::io_context& context) noexcept {
    context.restart();
    while (true) {
      try {
        if (context.poll() == 0U) {
          break;
        }
      } catch (...) {
      }
    }
    context.stop();
  }

  [[nodiscard]] CallbackDispatchResult QueueEmergencyCompletion(
      std::function<void()>& callback) noexcept {
    const std::scoped_lock lock(emergency_completion_mutex_);
    if (!callbacks_open_.load(std::memory_order_acquire)) {
      return CallbackDispatchResult::kClosed;
    }
    if (emergency_completion_count_ == emergency_completions_.size()) {
      return CallbackDispatchResult::kFailed;
    }
    emergency_completions_[emergency_completion_tail_] = std::move(callback);
    emergency_completion_tail_ =
        (emergency_completion_tail_ + 1U) % emergency_completions_.size();
    ++emergency_completion_count_;
    return CallbackDispatchResult::kQueued;
  }

  [[nodiscard]] CallbackDispatchResult DispatchCompletion(
      std::function<void()>& callback) noexcept {
    if (!callback || !callbacks_open_.load(std::memory_order_acquire)) {
      return CallbackDispatchResult::kClosed;
    }
    const std::weak_ptr<Impl> weak_owner = weak_from_this();
    try {
      asio::post(callback_context_, [weak_owner, callback] {
        const std::shared_ptr<Impl> owner = weak_owner.lock();
        if (owner != nullptr &&
            owner->callbacks_open_.load(std::memory_order_acquire)) {
          callback();
        }
      });
      return CallbackDispatchResult::kQueued;
    } catch (...) {
      return QueueEmergencyCompletion(callback);
    }
  }

  void DrainEmergencyCompletions(const bool invoke) noexcept {
    while (true) {
      std::function<void()> callback;
      {
        const std::scoped_lock lock(emergency_completion_mutex_);
        if (emergency_completion_count_ == 0U) {
          return;
        }
        callback = std::move(emergency_completions_[emergency_completion_head_]);
        emergency_completion_head_ =
            (emergency_completion_head_ + 1U) % emergency_completions_.size();
        --emergency_completion_count_;
      }
      if (invoke && callbacks_open_.load(std::memory_order_acquire) && callback) {
        try {
          callback();
        } catch (...) {
        }
      }
    }
  }

  [[nodiscard]] CallbackDispatchResult QueueEmergencyEvent(
      std::function<void()>& callback) noexcept {
    const std::scoped_lock lock(emergency_event_mutex_);
    if (!callbacks_open_.load(std::memory_order_acquire)) {
      return CallbackDispatchResult::kClosed;
    }
    if (emergency_event_count_ == emergency_events_.size()) {
      return CallbackDispatchResult::kFailed;
    }
    emergency_events_[emergency_event_tail_] = std::move(callback);
    emergency_event_tail_ = (emergency_event_tail_ + 1U) % emergency_events_.size();
    ++emergency_event_count_;
    return CallbackDispatchResult::kQueued;
  }

  [[nodiscard]] CallbackDispatchResult DispatchEvent(
      std::function<void()>& callback) noexcept {
    if (!callback || !callbacks_open_.load(std::memory_order_acquire)) {
      return CallbackDispatchResult::kClosed;
    }
    const std::weak_ptr<Impl> weak_owner = weak_from_this();
    try {
      asio::post(callback_context_, [weak_owner, callback] {
        const std::shared_ptr<Impl> owner = weak_owner.lock();
        if (owner != nullptr &&
            owner->callbacks_open_.load(std::memory_order_acquire)) {
          callback();
        }
      });
      return CallbackDispatchResult::kQueued;
    } catch (...) {
      return QueueEmergencyEvent(callback);
    }
  }

  void DrainEmergencyEvents(const bool invoke) noexcept {
    while (true) {
      std::function<void()> callback;
      {
        const std::scoped_lock lock(emergency_event_mutex_);
        if (emergency_event_count_ == 0U) {
          return;
        }
        callback = std::move(emergency_events_[emergency_event_head_]);
        emergency_event_head_ = (emergency_event_head_ + 1U) % emergency_events_.size();
        --emergency_event_count_;
      }
      if (invoke && callbacks_open_.load(std::memory_order_acquire) && callback) {
        try {
          callback();
        } catch (...) {
        }
      }
    }
  }

  [[nodiscard]] CallbackDispatchResult QueueEmergencyNetwork(
      const void* key, std::function<void()>& callback) noexcept {
    const std::scoped_lock lock(emergency_network_mutex_);
    if (key == nullptr || stopping_.load(std::memory_order_acquire) ||
        !running_.load()) {
      return CallbackDispatchResult::kClosed;
    }
    const auto existing =
        std::find_if(emergency_network_.begin(), emergency_network_.end(),
                     [key](const EmergencyNetworkEntry& entry) {
                       return entry.key == key && static_cast<bool>(entry.callback);
                     });
    if (existing != emergency_network_.end()) {
      return CallbackDispatchResult::kQueued;
    }
    const auto slot = std::find_if(
        emergency_network_.begin(), emergency_network_.end(),
        [](const EmergencyNetworkEntry& entry) { return !entry.callback; });
    if (slot == emergency_network_.end()) {
      return CallbackDispatchResult::kFailed;
    }
    slot->key = key;
    slot->pairing_key.reset();
    slot->callback = std::move(callback);
    return CallbackDispatchResult::kQueued;
  }

  [[nodiscard]] CallbackDispatchResult DispatchNetwork(
      const void* key, std::function<void()>& callback) noexcept {
    if (key == nullptr || !callback || !running()) {
      return CallbackDispatchResult::kClosed;
    }
    try {
      asio::post(context_, callback);
      return CallbackDispatchResult::kQueued;
    } catch (...) {
      return QueueEmergencyNetwork(key, callback);
    }
  }

  void CancelEmergencyNetwork(const void* key) noexcept {
    const std::scoped_lock lock(emergency_network_mutex_);
    for (EmergencyNetworkEntry& entry : emergency_network_) {
      if (entry.key == key) {
        entry.callback = {};
        entry.key = nullptr;
        entry.pairing_key.reset();
      }
    }
  }

  [[nodiscard]] CallbackDispatchResult QueueEmergencyPairingNetwork(
      const ConnectionId& key, std::function<void()>& callback) noexcept {
    const std::scoped_lock lock(emergency_network_mutex_);
    if (stopping_.load(std::memory_order_acquire) || !running_.load()) {
      return CallbackDispatchResult::kClosed;
    }
    const auto existing = std::find_if(
        emergency_network_.begin(), emergency_network_.end(),
        [&key](const EmergencyNetworkEntry& entry) {
          return entry.pairing_key == key && static_cast<bool>(entry.callback);
        });
    if (existing != emergency_network_.end()) {
      return CallbackDispatchResult::kQueued;
    }
    const auto slot = std::find_if(
        emergency_network_.begin(), emergency_network_.end(),
        [](const EmergencyNetworkEntry& entry) { return !entry.callback; });
    if (slot == emergency_network_.end()) {
      return CallbackDispatchResult::kFailed;
    }
    slot->key = nullptr;
    slot->pairing_key = key;
    slot->callback = std::move(callback);
    return CallbackDispatchResult::kQueued;
  }

  [[nodiscard]] CallbackDispatchResult DispatchPairingNetwork(
      const ConnectionId& key, std::function<void()>& callback) noexcept {
    if (!callback || !running()) {
      return CallbackDispatchResult::kClosed;
    }
    try {
      asio::post(context_, callback);
      return CallbackDispatchResult::kQueued;
    } catch (...) {
      return QueueEmergencyPairingNetwork(key, callback);
    }
  }

  void CancelEmergencyPairingNetwork(const ConnectionId& key) noexcept {
    const std::scoped_lock lock(emergency_network_mutex_);
    for (EmergencyNetworkEntry& entry : emergency_network_) {
      if (entry.pairing_key == key) {
        entry.callback = {};
        entry.key = nullptr;
        entry.pairing_key.reset();
      }
    }
  }

  void DrainEmergencyNetwork(const bool invoke) noexcept {
    while (true) {
      std::function<void()> callback;
      {
        const std::scoped_lock lock(emergency_network_mutex_);
        const auto slot =
            std::find_if(emergency_network_.begin(), emergency_network_.end(),
                         [](const EmergencyNetworkEntry& entry) {
                           return static_cast<bool>(entry.callback);
                         });
        if (slot == emergency_network_.end()) {
          return;
        }
        callback = std::move(slot->callback);
        slot->key = nullptr;
        slot->pairing_key.reset();
      }
      if (invoke && !stopping_.load(std::memory_order_acquire) && callback) {
        try {
          callback();
        } catch (...) {
        }
      }
    }
  }

  [[nodiscard]] bool ReserveCompletion() noexcept {
    if (!callbacks_open_.load(std::memory_order_acquire)) {
      return false;
    }
    std::size_t current = completion_reservations_.load(std::memory_order_acquire);
    while (current < kEmergencyCompletionCapacity) {
      if (completion_reservations_.compare_exchange_weak(current, current + 1U,
                                                         std::memory_order_acq_rel)) {
        if (callbacks_open_.load(std::memory_order_acquire)) {
          return true;
        }
        ReleaseCompletion();
        return false;
      }
    }
    return false;
  }

  void ReleaseCompletion() noexcept {
    completion_reservations_.fetch_sub(1U, std::memory_order_acq_rel);
  }

  [[nodiscard]] bool RollbackStart(std::unique_lock<std::mutex>& lifecycle_lock) {
    rollback_in_progress_ = true;
    running_.store(false, std::memory_order_release);
    stopping_.store(true, std::memory_order_release);
    CancelRuntimeCommands();
    callbacks_open_.store(false, std::memory_order_release);
    work_.reset();
    callback_work_.reset();
    asio::error_code ignored;
    if (acceptor_ != nullptr) {
      acceptor_->cancel(ignored);
      acceptor_->close(ignored);
    }
    context_.stop();
    callback_context_.stop();
    AdmissionBridge::Retire(admission_);

    std::thread joining_thread = std::move(thread_);
    std::thread joining_callback_thread = std::move(callback_thread_);
    lifecycle_lock.unlock();
    if (joining_thread.joinable()) {
      joining_thread.join();
    }
    if (joining_callback_thread.joinable()) {
      joining_callback_thread.join();
    }
    DrainStoppedContext(context_);
    DrainStoppedContext(callback_context_);
    lifecycle_lock.lock();

    acceptor_.reset();
    server_asio_context_.reset();
    pairing_client_asio_context_.reset();
    established_client_asio_context_.reset();
    server_context_.reset();
    pairing_client_context_.reset();
    established_client_context_.reset();
    listen_port_.store(0U, std::memory_order_release);
    worker_id_ = {};
    shutdown_started_ = true;
    thread_exited_ = true;
    callback_thread_exited_ = true;
    rollback_in_progress_ = false;
    stopped_cv_.notify_all();
    return false;
  }

  void BeginCallbackShutdown() noexcept {
    callbacks_open_.store(false, std::memory_order_release);
    callback_work_.reset();
    callback_context_.stop();
  }

  void FinishCallbackShutdown(std::unique_lock<std::mutex>& lifecycle_lock,
                              const bool on_callback) {
    if (on_callback) {
      return;
    }
    if (callback_thread_.joinable()) {
      stopped_cv_.wait(lifecycle_lock, [this] { return callback_thread_exited_; });
      if (callback_thread_.joinable()) {
        std::thread joining = std::move(callback_thread_);
        joining.join();
      }
    }
    DrainStoppedContext(callback_context_);
  }

  [[nodiscard]] bool RegisterRuntimeCommand(
      const std::shared_ptr<RuntimeCommand>& command) {
    const std::scoped_lock lock(command_mutex_);
    if (!commands_open_.load(std::memory_order_acquire)) {
      return false;
    }
    try {
      runtime_commands_.push_back(command);
      return true;
    } catch (...) {
      return false;
    }
  }

  void RemoveRuntimeCommand(const std::shared_ptr<RuntimeCommand>& command) noexcept {
    const std::scoped_lock lock(command_mutex_);
    runtime_commands_.erase(
        std::remove(runtime_commands_.begin(), runtime_commands_.end(), command),
        runtime_commands_.end());
  }

  void CancelRuntimeCommands() noexcept {
    const std::scoped_lock lock(command_mutex_);
    commands_open_.store(false, std::memory_order_release);
    for (const std::shared_ptr<RuntimeCommand>& command : runtime_commands_) {
      command->Cancel();
    }
  }

  template <typename Function>
  [[nodiscard]] bool RunBool(Function function) {
    if (!running()) {
      return false;
    }
    if (worker_id_ == std::this_thread::get_id()) {
      try {
        return function();
      } catch (...) {
        return false;
      }
    }

    std::shared_ptr<RuntimeCommand> command;
    std::future<bool> result;
    try {
      command = std::make_shared<RuntimeCommand>();
      result = command->Future();
      if (!RegisterRuntimeCommand(command)) {
        return false;
      }
      const std::weak_ptr<Impl> weak_owner = shared_from_this();
      asio::post(context_,
                 [weak_owner, command, function = std::move(function)]() mutable {
                   bool value = false;
                   if (command->Start()) {
                     const std::shared_ptr<Impl> self = weak_owner.lock();
                     try {
                       if (self != nullptr && self->running()) {
                         value = function();
                       }
                     } catch (...) {
                     }
                     command->Complete(value);
                   }
                 });
    } catch (...) {
      if (command != nullptr) {
        command->Cancel();
        RemoveRuntimeCommand(command);
      }
      return false;
    }
    if (result.wait_for(kRuntimeCommandTimeout) != std::future_status::ready) {
      if (command->CancelIfQueued()) {
        RemoveRuntimeCommand(command);
        return false;
      }
      result.wait();
    }
    const bool value = result.get();
    RemoveRuntimeCommand(command);
    return value;
  }

  [[nodiscard]] bool GenerateConnectionId(ConnectionId& output) {
    for (std::size_t attempt = 0; attempt < 8U; ++attempt) {
      if (!entropy_.Fill(output) || AllZero(output)) {
        continue;
      }
      const bool pending = std::any_of(
          pending_handshakes_.begin(), pending_handshakes_.end(),
          [&output](const std::shared_ptr<HandshakeConnection>& connection) {
            return connection->connection_id() == output;
          });
      const bool pairing =
          std::any_of(pairing_connections_.begin(), pairing_connections_.end(),
                      [&output](const std::shared_ptr<PairingConnection>& connection) {
                        return connection->connection_id() == output;
                      });
      const bool established =
          std::any_of(established_connections_.begin(), established_connections_.end(),
                      [&output](const TrackedEstablishedConnection& connection) {
                        return connection.lease->id() == output;
                      });
      if (!pending && !pairing && !established) {
        return true;
      }
    }
    std::fill(output.begin(), output.end(), 0U);
    return false;
  }

  [[nodiscard]] static std::shared_ptr<asio::ssl::context> MakeAsioContext(
      const std::shared_ptr<OpenSslTlsContext>& context) {
    SSL_CTX* const native = context->native_handle();
    if (native == nullptr || SSL_CTX_up_ref(native) != 1) {
      return nullptr;
    }
    try {
      return std::make_shared<asio::ssl::context>(native);
    } catch (...) {
      SSL_CTX_free(native);
      return nullptr;
    }
  }

  [[nodiscard]] static std::unique_ptr<TlsStream> MakeTlsStream(
      Tcp::socket socket, const std::shared_ptr<asio::ssl::context>& context) {
    try {
      return std::make_unique<TlsStream>(std::move(socket), *context);
    } catch (...) {
      return nullptr;
    }
  }

  [[nodiscard]] bool AtConnectionCapacity() const noexcept {
    return pending_handshakes_.size() + pairing_connections_.size() +
               established_connections_.size() >=
           kMaxAuthenticatedConnections;
  }

  void DoAccept() {
    if (stopping_.load(std::memory_order_acquire) || acceptor_ == nullptr ||
        !acceptor_->is_open()) {
      return;
    }
    const std::weak_ptr<Impl> weak_owner = shared_from_this();
    acceptor_->async_accept([weak_owner](const asio::error_code& error,
                                         Tcp::socket socket) mutable {
      const std::shared_ptr<Impl> self = weak_owner.lock();
      if (self == nullptr) {
        asio::error_code ignored;
        socket.close(ignored);
        return;
      }
      if (!error && self->running() && !self->AtConnectionCapacity() &&
          self->pending_handshakes_.size() < kMaxPendingTlsHandshakes) {
        asio::error_code endpoint_error;
        const Tcp::endpoint peer = socket.remote_endpoint(endpoint_error);
        ConnectionId connection_id{};
        std::unique_ptr<PairingAdmissionLease> admission;
        std::unique_ptr<TlsStream> stream;
        if (!endpoint_error && self->GenerateConnectionId(connection_id)) {
          const SourceToken source = MakeSourceToken(peer);
          if (!AllZero(source)) {
            admission = AdmissionBridge::Reserve(self->admission_, connection_id,
                                                 source, false, NowMs());
          }
          if (admission != nullptr) {
            stream = self->MakeTlsStream(std::move(socket), self->server_asio_context_);
          }
        }
        if (stream != nullptr) {
          try {
            auto connection = std::make_shared<HandshakeConnection>(
                self->server_context_, std::move(stream), connection_id, peer,
                HandshakeConnection::Mode::kServerUnknown, 0U, "", std::nullopt,
                std::move(admission), 0U, self->config_.tls_handshake_timeout_ms,
                [weak_owner](const std::shared_ptr<HandshakeConnection>& handshake) {
                  if (const auto owner = weak_owner.lock()) {
                    owner->CompleteHandshake(handshake);
                  }
                },
                [weak_owner](const std::shared_ptr<HandshakeConnection>& handshake,
                             const ConnectionIoError io_error,
                             const SecurityError security_error, const bool timed_out) {
                  if (const auto owner = weak_owner.lock()) {
                    owner->CompleteHandshakeFailure(handshake, io_error, security_error,
                                                    timed_out);
                  }
                });
            self->pending_handshakes_.push_back(connection);
            connection->StartServer();
          } catch (...) {
            CloseStream(stream);
          }
        } else {
          asio::error_code ignored;
          socket.close(ignored);
        }
      } else {
        asio::error_code ignored;
        socket.close(ignored);
      }
      self->DoAccept();
    });
  }

  [[nodiscard]] bool StartClientHandshake(const Tcp::endpoint& endpoint,
                                          const HandshakeConnection::Mode mode,
                                          const std::uint64_t request_id,
                                          std::string peer_display_label,
                                          std::optional<DeviceId> expected_peer) {
    const std::uint64_t pairing_window_generation =
        mode == HandshakeConnection::Mode::kClientPairing
            ? AdmissionBridge::WindowGeneration(admission_, NowMs())
            : 0U;
    if (AtConnectionCapacity() ||
        pending_handshakes_.size() >= kMaxPendingTlsHandshakes ||
        (mode == HandshakeConnection::Mode::kClientPairing &&
         pairing_window_generation == 0U)) {
      return false;
    }
    ConnectionId connection_id{};
    if (!GenerateConnectionId(connection_id)) {
      return false;
    }
    const SourceToken source = MakeSourceToken(endpoint);
    if (AllZero(source)) {
      return false;
    }
    std::unique_ptr<PairingAdmissionLease> admission =
        AdmissionBridge::Reserve(admission_, connection_id, source, true, NowMs());
    if (admission == nullptr) {
      return false;
    }
    std::shared_ptr<OpenSslTlsContext> context =
        mode == HandshakeConnection::Mode::kClientPairing ? pairing_client_context_
                                                          : established_client_context_;
    std::shared_ptr<asio::ssl::context> asio_context =
        mode == HandshakeConnection::Mode::kClientPairing
            ? pairing_client_asio_context_
            : established_client_asio_context_;
    std::unique_ptr<TlsStream> stream =
        MakeTlsStream(Tcp::socket(context_), asio_context);
    if (stream == nullptr) {
      return false;
    }
    try {
      const std::weak_ptr<Impl> weak_owner = shared_from_this();
      auto connection = std::make_shared<HandshakeConnection>(
          std::move(context), std::move(stream), connection_id, endpoint, mode,
          request_id, std::move(peer_display_label), std::move(expected_peer),
          std::move(admission), pairing_window_generation,
          config_.tls_handshake_timeout_ms,
          [weak_owner](const std::shared_ptr<HandshakeConnection>& handshake) {
            if (const auto owner = weak_owner.lock()) {
              owner->CompleteHandshake(handshake);
            }
          },
          [weak_owner](const std::shared_ptr<HandshakeConnection>& handshake,
                       const ConnectionIoError io_error,
                       const SecurityError security_error, const bool timed_out) {
            if (const auto owner = weak_owner.lock()) {
              owner->CompleteHandshakeFailure(handshake, io_error, security_error,
                                              timed_out);
            }
          });
      pending_handshakes_.push_back(connection);
      connection->StartClient(endpoint);
      return true;
    } catch (...) {
      return false;
    }
  }

  void CompleteHandshake(const std::shared_ptr<HandshakeConnection>& handshake) {
    std::unique_ptr<TlsStream> stream = handshake->TakeStream();
    RemovePending(handshake->connection_id());
    if (!running() || stream == nullptr) {
      CloseStream(stream);
      return;
    }

    if (handshake->mode() == HandshakeConnection::Mode::kServerUnknown) {
      auto accepted =
          handshake->context()->AcceptServerPeer(stream->native_handle(), *repository_);
      if (!accepted.ok()) {
        CloseStream(stream);
        return;
      }
      if (auto* pairing = std::get_if<AcceptedPairingTlsConnection>(&*accepted.value);
          pairing != nullptr) {
        const std::uint64_t pairing_window_generation =
            pairing->pairing_window_generation();
        auto channel = OpenSslPairingChannel::Create(
            *handshake->context(), stream->native_handle(), std::move(*pairing));
        if (!channel.ok()) {
          CloseStream(stream);
          return;
        }
        BeginPairing(handshake, std::move(stream),
                     std::unique_ptr<PairingChannel>(std::move(*channel.value)), true,
                     pairing_window_generation);
        return;
      }
      auto* established =
          std::get_if<AcceptedEstablishedTlsConnection>(&*accepted.value);
      if (established == nullptr) {
        CloseStream(stream);
        return;
      }
      auto channel = EstablishedTlsChannel::Create(
          *handshake->context(), stream->native_handle(), std::move(*established));
      if (!channel.ok()) {
        CloseStream(stream);
        return;
      }
      PublishEstablished(handshake, std::move(stream), std::move(*channel.value), true);
      return;
    }

    if (handshake->mode() == HandshakeConnection::Mode::kClientPairing) {
      auto channel =
          OpenSslPairingChannel::Create(*handshake->context(), stream->native_handle());
      if (!channel.ok()) {
        PublishPairingFailure(handshake, PairingError::kCertificateRejected);
        CloseStream(stream);
        return;
      }
      BeginPairing(handshake, std::move(stream),
                   std::unique_ptr<PairingChannel>(std::move(*channel.value)), false,
                   handshake->pairing_window_generation());
      return;
    }

    if (!handshake->expected_peer().has_value()) {
      PublishEstablishedFailure(handshake, ConnectionIoError::kInvalidArgument,
                                SecurityError::kPinMismatch);
      CloseStream(stream);
      return;
    }
    auto channel =
        EstablishedTlsChannel::Create(*handshake->context(), stream->native_handle(),
                                      *repository_, *handshake->expected_peer());
    if (!channel.ok()) {
      PublishEstablishedFailure(handshake, ConnectionIoError::kNone, channel.error);
      CloseStream(stream);
      return;
    }
    PublishEstablished(handshake, std::move(stream), std::move(*channel.value), false);
  }

  void CompleteHandshakeFailure(const std::shared_ptr<HandshakeConnection>& handshake,
                                const ConnectionIoError io_error,
                                const SecurityError security_error,
                                const bool timed_out) {
    RemovePending(handshake->connection_id());
    if (handshake->mode() == HandshakeConnection::Mode::kClientPairing) {
      PublishPairingFailure(handshake, timed_out ? PairingError::kTimeout
                                                 : PairingError::kCertificateRejected);
    } else if (handshake->mode() == HandshakeConnection::Mode::kClientEstablished) {
      PublishEstablishedFailure(handshake, io_error, security_error);
    }
  }

  void BeginPairing(const std::shared_ptr<HandshakeConnection>& handshake,
                    std::unique_ptr<TlsStream> stream,
                    std::unique_ptr<PairingChannel> channel, const bool inbound,
                    const std::uint64_t pairing_window_generation) {
    const PublicKey* const local_key = repository_->root_public_key();
    if (local_key == nullptr || channel == nullptr) {
      PublishPairingFailure(handshake, PairingError::kInternalFailure);
      CloseStream(stream);
      return;
    }
    const SourceToken source = MakeSourceToken(handshake->peer_endpoint());
    if (AllZero(source)) {
      PublishPairingFailure(handshake, PairingError::kInternalFailure);
      CloseStream(stream);
      return;
    }
    PairingAdmissionResult admission = AdmissionBridge::Bind(
        admission_, handshake->TakeAdmission(),
        PairingAdmissionRequest{
            .connection_id = handshake->connection_id(),
            .source = source,
            .local_key = *local_key,
            .peer_key = channel->peer_public_key().bytes(),
            .local_role = inbound ? security::tls::Role::kResponder
                                  : security::tls::Role::kInitiator,
            .user_initiated = !inbound,
            .now_ms = NowMs(),
        },
        pairing_window_generation);
    if (admission.displaced_connection.has_value()) {
      const auto displaced = std::find_if(
          pairing_connections_.begin(), pairing_connections_.end(),
          [&admission](const std::shared_ptr<PairingConnection>& connection) {
            return connection->connection_id() == *admission.displaced_connection;
          });
      if (displaced != pairing_connections_.end()) {
        (*displaced)->Stop(PairingError::kBusy, true);
      }
    }
    if (!admission.accepted()) {
      PublishPairingFailure(handshake, admission.error);
      CloseStream(stream);
      return;
    }

    std::unique_ptr<PairingAttempt> attempt;
    PairingUpdate created = PairingAttempt::Create(
        *repository_, std::move(channel), entropy_, std::move(admission.lease),
        replay_cache_,
        PairingAttemptOptions{
            .offer = config_.pairing_offer,
            .peer_display_label = handshake->peer_display_label(),
        },
        attempt);
    if (created.terminal || attempt == nullptr) {
      PublishPairingFailure(handshake, created.error);
      CloseStream(stream);
      return;
    }

    const AttemptHandle attempt_handle = attempt->handle();
    try {
      const std::weak_ptr<Impl> weak_owner = shared_from_this();
      auto connection = std::make_shared<PairingConnection>(
          handshake->context(), std::move(stream), std::move(attempt),
          handshake->connection_id(), attempt_handle, handshake->request_id(), inbound,
          config_.pairing_write_fragment_bytes,
          [weak_owner](PairingConnection& pairing, PairingUpdate update) {
            if (const auto owner = weak_owner.lock()) {
              owner->PublishPairing(pairing, std::move(update));
            }
          },
          [weak_owner](const ConnectionId& connection_id) {
            if (const auto owner = weak_owner.lock()) {
              owner->RemovePairing(connection_id);
            }
          });
      pairing_connections_.push_back(connection);
      connection->Start();
    } catch (...) {
      PublishPairingFailure(handshake, PairingError::kLimitExceeded);
      CloseStream(stream);
    }
  }

  void PublishEstablished(const std::shared_ptr<HandshakeConnection>& handshake,
                          std::unique_ptr<TlsStream> stream,
                          std::unique_ptr<EstablishedTlsChannel> channel,
                          const bool inbound) {
    try {
      const std::shared_ptr<Impl> executor_owner = shared_from_this();
      const std::weak_ptr<Impl> weak_owner = executor_owner;
      auto construction =
          std::make_unique<AuthenticatedEstablishedConnection::Construction>();
      construction->executor_owner = executor_owner;
      construction->context = handshake->context();
      construction->stream = std::move(stream);
      construction->channel = std::move(channel);
      construction->connection_id = handshake->connection_id();
      construction->inbound = inbound;
      construction->close_handler = [weak_owner](const ConnectionId& connection_id) {
        if (const std::shared_ptr<Impl> owner = weak_owner.lock()) {
          owner->RemoveEstablished(connection_id);
        }
      };
      construction->completion_reserver = [weak_owner] {
        const std::shared_ptr<Impl> owner = weak_owner.lock();
        return owner != nullptr && owner->ReserveCompletion();
      };
      construction->completion_releaser = [weak_owner] {
        if (const std::shared_ptr<Impl> owner = weak_owner.lock()) {
          owner->ReleaseCompletion();
        }
      };
      construction->callback_dispatcher =
          [weak_owner](std::function<void()>& callback) {
            const std::shared_ptr<Impl> owner = weak_owner.lock();
            if (owner == nullptr) {
              return runtime_internal::CallbackDispatchResult::kClosed;
            }
            return owner->DispatchCompletion(callback);
          };
      construction->network_dispatcher = [weak_owner](const void* key,
                                                      std::function<void()>& callback) {
        const std::shared_ptr<Impl> owner = weak_owner.lock();
        if (owner == nullptr) {
          return runtime_internal::CallbackDispatchResult::kClosed;
        }
        return owner->DispatchNetwork(key, callback);
      };
      construction->network_canceller = [weak_owner](const void* key) {
        if (const std::shared_ptr<Impl> owner = weak_owner.lock()) {
          owner->CancelEmergencyNetwork(key);
        }
      };
      std::shared_ptr<AuthenticatedEstablishedConnection::RuntimeLease> runtime_lease;
      auto connection = AuthenticatedEstablishedConnection::Create(
          std::move(construction), runtime_lease);
      if (connection == nullptr || runtime_lease == nullptr) {
        PublishEstablishedFailure(handshake, ConnectionIoError::kBusy,
                                  SecurityError::kTlsConfigurationFailure);
        return;
      }
      established_connections_.push_back(TrackedEstablishedConnection{
          .lease = runtime_lease,
      });
      QueueEstablishedEvent(
          EstablishedRuntimeEvent{
              .request_id = handshake->request_id(),
              .inbound = inbound,
              .connection = connection,
          },
          std::move(runtime_lease));
    } catch (...) {
      CloseStream(stream);
      PublishEstablishedFailure(handshake, ConnectionIoError::kBusy,
                                SecurityError::kTlsConfigurationFailure);
    }
  }

  void PublishPairing(PairingConnection& connection, PairingUpdate update) {
    QueuePairingEvent(
        PairingRuntimeEvent{
            .connection_id = connection.connection_id(),
            .attempt = connection.attempt_handle(),
            .request_id = connection.request_id(),
            .update = std::move(update),
            .inbound = connection.inbound(),
        },
        true);
  }

  void PublishPairingFailure(const std::shared_ptr<HandshakeConnection>& handshake,
                             const PairingError error) {
    QueuePairingEvent(
        PairingRuntimeEvent{
            .connection_id = handshake->connection_id(),
            .request_id = handshake->request_id(),
            .update = ClosedUpdate(error),
            .inbound = handshake->mode() == HandshakeConnection::Mode::kServerUnknown,
        },
        false);
  }

  void PublishEstablishedFailure(const std::shared_ptr<HandshakeConnection>& handshake,
                                 const ConnectionIoError io_error,
                                 const SecurityError security_error) {
    QueueEstablishedEvent(
        EstablishedRuntimeEvent{
            .request_id = handshake->request_id(),
            .inbound = false,
            .io_error = io_error,
            .security_error = security_error,
        },
        nullptr);
  }

  void QueuePairingEvent(PairingRuntimeEvent event, const bool has_live_connection) {
    if (!pairing_handler_ || !callbacks_open_.load(std::memory_order_acquire)) {
      return;
    }
    const std::weak_ptr<Impl> weak_owner = shared_from_this();
    const ConnectionId connection_id = event.connection_id;
    try {
      PairingHandler handler = pairing_handler_;
      std::function<void()> recovery;
      if (has_live_connection) {
        recovery = [weak_owner, connection_id] {
          const std::shared_ptr<Impl> owner = weak_owner.lock();
          if (owner == nullptr || !owner->running()) {
            return;
          }
          owner->FailPairing(connection_id);
        };
      }
      std::function<void()> callback = [weak_owner, handler = std::move(handler),
                                        event = std::move(event),
                                        recovery = std::move(recovery)]() mutable {
        const std::shared_ptr<Impl> owner = weak_owner.lock();
        if (owner == nullptr ||
            !owner->callbacks_open_.load(std::memory_order_acquire)) {
          return;
        }
        try {
          handler(event);
        } catch (...) {
          if (recovery) {
            const CallbackDispatchResult result =
                owner->DispatchPairingNetwork(event.connection_id, recovery);
            if (result == CallbackDispatchResult::kFailed) {
              owner->Stop();
            }
          }
        }
      };
      const CallbackDispatchResult result = DispatchEvent(callback);
      if (result == CallbackDispatchResult::kFailed && has_live_connection) {
        FailPairing(connection_id);
      }
    } catch (...) {
      if (has_live_connection) {
        FailPairing(connection_id);
      }
    }
  }

  void FailPairing(const ConnectionId& connection_id) {
    const auto found = std::find_if(
        pairing_connections_.begin(), pairing_connections_.end(),
        [&connection_id](const std::shared_ptr<PairingConnection>& connection) {
          return connection->connection_id() == connection_id;
        });
    if (found != pairing_connections_.end()) {
      (*found)->Stop(PairingError::kInternalFailure, false);
    }
  }

  void QueueEstablishedEvent(
      EstablishedRuntimeEvent event,
      std::shared_ptr<AuthenticatedEstablishedConnection::RuntimeLease> lease) {
    if (!established_handler_) {
      if (lease != nullptr) {
        lease->Stop();
      }
      return;
    }
    if (!callbacks_open_.load(std::memory_order_acquire)) {
      return;
    }
    const std::weak_ptr<Impl> weak_owner = shared_from_this();
    try {
      EstablishedHandler handler = established_handler_;
      std::function<void()> callback = [weak_owner, handler = std::move(handler),
                                        event = std::move(event),
                                        callback_lease = lease]() mutable {
        const std::shared_ptr<Impl> owner = weak_owner.lock();
        if (owner == nullptr ||
            !owner->callbacks_open_.load(std::memory_order_acquire)) {
          return;
        }
        try {
          handler(event);
        } catch (...) {
          if (callback_lease != nullptr) {
            callback_lease->Close();
          }
        }
      };
      const CallbackDispatchResult result = DispatchEvent(callback);
      if (result == CallbackDispatchResult::kFailed && lease != nullptr) {
        lease->Stop();
      }
    } catch (...) {
      if (lease != nullptr) {
        lease->Stop();
      }
    }
  }

  void RemovePending(const ConnectionId& connection_id) {
    pending_handshakes_.erase(
        std::remove_if(
            pending_handshakes_.begin(), pending_handshakes_.end(),
            [&connection_id](const std::shared_ptr<HandshakeConnection>& candidate) {
              return candidate->connection_id() == connection_id;
            }),
        pending_handshakes_.end());
  }

  void RemovePairing(const ConnectionId& connection_id) {
    CancelEmergencyPairingNetwork(connection_id);
    pairing_connections_.erase(
        std::remove_if(
            pairing_connections_.begin(), pairing_connections_.end(),
            [&connection_id](const std::shared_ptr<PairingConnection>& candidate) {
              return candidate->connection_id() == connection_id;
            }),
        pairing_connections_.end());
  }

  void RemoveEstablished(const ConnectionId& connection_id) {
    established_connections_.erase(
        std::remove_if(established_connections_.begin(), established_connections_.end(),
                       [&connection_id](const TrackedEstablishedConnection& candidate) {
                         return candidate.lease->id() == connection_id;
                       }),
        established_connections_.end());
  }

  static void CloseStream(std::unique_ptr<TlsStream>& stream) {
    if (stream == nullptr) {
      return;
    }
    asio::error_code ignored;
    stream->lowest_layer().cancel(ignored);
    stream->lowest_layer().shutdown(Tcp::socket::shutdown_both, ignored);
    stream->lowest_layer().close(ignored);
  }

  void ShutdownOwnedState() {
    if (shutdown_started_) {
      return;
    }
    shutdown_started_ = true;
    CancelRuntimeCommands();
    BeginCallbackShutdown();
    pairing_handler_ = {};
    established_handler_ = {};
    asio::error_code ignored;
    if (acceptor_ != nullptr) {
      acceptor_->cancel(ignored);
      acceptor_->close(ignored);
    }

    auto pending = std::move(pending_handshakes_);
    pending_handshakes_.clear();
    for (const auto& connection : pending) {
      connection->Stop();
    }

    auto pairing = std::move(pairing_connections_);
    pairing_connections_.clear();
    for (const auto& connection : pairing) {
      connection->Stop(PairingError::kCancelled, true);
    }

    auto established = std::move(established_connections_);
    established_connections_.clear();
    for (const TrackedEstablishedConnection& tracked : established) {
      tracked.lease->Stop();
    }

    admission_.CloseWindow();
    AdmissionBridge::Retire(admission_);
    work_.reset();
    context_.stop();
  }

  security::identity::IdentityRepository* repository_{};
  AuthenticatedConnectionRuntimeConfig config_{};
  PairingHandler pairing_handler_{};
  EstablishedHandler established_handler_{};
  OpenSslSessionEntropy entropy_{};
  PairingAdmissionController admission_{};
  PairingReplayCache replay_cache_{};
  asio::io_context context_{};
  asio::io_context callback_context_{};
  std::optional<WorkGuard> work_{};
  std::optional<WorkGuard> callback_work_{};
  std::unique_ptr<Tcp::acceptor> acceptor_{};
  std::shared_ptr<OpenSslTlsContext> server_context_{};
  std::shared_ptr<OpenSslTlsContext> pairing_client_context_{};
  std::shared_ptr<OpenSslTlsContext> established_client_context_{};
  std::shared_ptr<asio::ssl::context> server_asio_context_{};
  std::shared_ptr<asio::ssl::context> pairing_client_asio_context_{};
  std::shared_ptr<asio::ssl::context> established_client_asio_context_{};
  std::vector<std::shared_ptr<HandshakeConnection>> pending_handshakes_{};
  std::vector<std::shared_ptr<PairingConnection>> pairing_connections_{};
  std::vector<TrackedEstablishedConnection> established_connections_{};
  mutable std::mutex lifecycle_mutex_{};
  std::condition_variable stopped_cv_{};
  std::thread thread_{};
  std::thread callback_thread_{};
  std::thread::id worker_id_{};
  std::atomic_bool running_{};
  std::atomic_bool stopping_{true};
  std::atomic_bool callbacks_open_{};
  std::atomic<std::uint16_t> listen_port_{};
  std::mutex command_mutex_{};
  std::vector<std::shared_ptr<RuntimeCommand>> runtime_commands_{};
  std::atomic_bool commands_open_{};
  std::atomic_size_t completion_reservations_{};
  std::mutex emergency_completion_mutex_{};
  std::array<std::function<void()>, kEmergencyCompletionCapacity>
      emergency_completions_{};
  std::size_t emergency_completion_head_{};
  std::size_t emergency_completion_tail_{};
  std::size_t emergency_completion_count_{};
  std::mutex emergency_event_mutex_{};
  std::array<std::function<void()>, kEmergencyEventCapacity> emergency_events_{};
  std::size_t emergency_event_head_{};
  std::size_t emergency_event_tail_{};
  std::size_t emergency_event_count_{};
  std::mutex emergency_network_mutex_{};
  std::array<EmergencyNetworkEntry, kEmergencyNetworkCapacity> emergency_network_{};
  bool start_attempted_{};
  bool rollback_in_progress_{};
  bool shutdown_started_{};
  bool thread_exited_{};
  bool callback_thread_exited_{};
  bool callback_stop_waiting_for_worker_{};
};

AuthenticatedConnectionRuntime::AuthenticatedConnectionRuntime(
    security::identity::IdentityRepository& repository,
    AuthenticatedConnectionRuntimeConfig config, PairingHandler pairing_handler,
    EstablishedHandler established_handler)
    : implementation_(std::make_shared<Impl>(repository, std::move(config),
                                             std::move(pairing_handler),
                                             std::move(established_handler))) {}

AuthenticatedConnectionRuntime::~AuthenticatedConnectionRuntime() {
  implementation_->Stop();
}

bool AuthenticatedConnectionRuntime::Start() { return implementation_->Start(); }

void AuthenticatedConnectionRuntime::Stop() { implementation_->Stop(); }

bool AuthenticatedConnectionRuntime::running() const noexcept {
  return implementation_->running();
}

std::uint16_t AuthenticatedConnectionRuntime::listen_port() const noexcept {
  return implementation_->listen_port();
}

bool AuthenticatedConnectionRuntime::OpenPairingWindow(
    const std::uint64_t duration_ms) {
  return implementation_->OpenPairingWindow(duration_ms);
}

void AuthenticatedConnectionRuntime::ClosePairingWindow() {
  implementation_->ClosePairingWindow();
}

bool AuthenticatedConnectionRuntime::StartPairing(const NetworkEndpoint& endpoint,
                                                  const std::uint64_t request_id,
                                                  std::string peer_display_label) {
  return implementation_->StartPairing(endpoint, request_id,
                                       std::move(peer_display_label));
}

bool AuthenticatedConnectionRuntime::Decide(
    const AttemptHandle& attempt, const security::tls::ConfirmationDecision decision) {
  return implementation_->Decide(attempt, decision);
}

bool AuthenticatedConnectionRuntime::Cancel(const AttemptHandle& attempt) {
  return implementation_->Cancel(attempt);
}

bool AuthenticatedConnectionRuntime::OpenEstablished(const NetworkEndpoint& endpoint,
                                                     const DeviceId& peer_device_id,
                                                     const std::uint64_t request_id) {
  return implementation_->OpenEstablished(endpoint, peer_device_id, request_id);
}

}  // namespace xnn_transfer::core::session
