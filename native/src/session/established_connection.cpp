#include <asio/buffer.hpp>
#include <asio/post.hpp>
#include <asio/write.hpp>
#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "connection_runtime_internal.hpp"

namespace xnn_transfer::core::session {

class AuthenticatedEstablishedConnection::Impl final
    : public std::enable_shared_from_this<AuthenticatedEstablishedConnection::Impl> {
 public:
  class CompletionReservation final {
   public:
    explicit CompletionReservation(std::function<void()> releaser)
        : releaser_(std::move(releaser)) {}
    ~CompletionReservation() { Release(); }

    void Release() noexcept {
      if (!released_.exchange(true, std::memory_order_acq_rel) && releaser_) {
        try {
          releaser_();
        } catch (...) {
        }
      }
    }

   private:
    std::function<void()> releaser_{};
    std::atomic_bool released_{};
  };

  struct ReadCompletionState {
    ReadHandler handler{};
    std::shared_ptr<CompletionReservation> reservation{};
    ConnectionIoError error{ConnectionIoError::kClosed};
    Bytes bytes{};
  };

  struct PreparedRead {
    std::shared_ptr<ReadCompletionState> state{};
    std::function<void()> callback{};
  };

  struct WriteCompletionState {
    WriteHandler handler{};
    std::shared_ptr<CompletionReservation> reservation{};
    ConnectionIoError error{ConnectionIoError::kClosed};
  };

  struct PreparedWrite {
    std::shared_ptr<WriteCompletionState> state{};
    std::function<void()> callback{};
  };

  struct PendingWrite {
    Bytes bytes{};
    PreparedWrite completion{};
  };

  explicit Impl(std::unique_ptr<Construction> construction)
      : executor_owner_(std::move(construction->executor_owner)),
        context_(std::move(construction->context)),
        stream_(std::move(construction->stream)),
        channel_(std::move(construction->channel)),
        id_(construction->connection_id),
        peer_device_id_(channel_->peer_device_id()),
        inbound_(construction->inbound),
        close_handler_(std::move(construction->close_handler)),
        completion_reserver_(std::move(construction->completion_reserver)),
        completion_releaser_(std::move(construction->completion_releaser)),
        callback_dispatcher_(std::move(construction->callback_dispatcher)),
        network_dispatcher_(std::move(construction->network_dispatcher)),
        network_canceller_(std::move(construction->network_canceller)) {}

  [[nodiscard]] bool Initialize() {
    try {
      const std::weak_ptr<Impl> weak_self = weak_from_this();
      close_callback_ = [weak_self] {
        if (const std::shared_ptr<Impl> self = weak_self.lock()) {
          self->CloseOnExecutor(true);
        }
      };
      return true;
    } catch (...) {
      return false;
    }
  }

  [[nodiscard]] const ConnectionId& id() const noexcept { return id_; }
  [[nodiscard]] const DeviceId& peer_device_id() const noexcept {
    return peer_device_id_;
  }
  [[nodiscard]] bool inbound() const noexcept { return inbound_; }
  [[nodiscard]] bool open() const noexcept {
    return open_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool DispatchChannel(ChannelHandler handler) {
    if (!handler) {
      return false;
    }
    const std::scoped_lock operation_lock(operation_mutex_);
    if (!operations_open_ || !open()) {
      return false;
    }
    try {
      asio::post(stream_->get_executor(),
                 [self = shared_from_this(), handler = std::move(handler)]() mutable {
                   if (!self->open()) {
                     return;
                   }
                   try {
                     handler(*self->channel_);
                   } catch (...) {
                     self->CloseOnExecutor(true);
                   }
                 });
      return true;
    } catch (...) {
      return false;
    }
  }

  [[nodiscard]] bool ReadSome(const std::size_t maximum_bytes, ReadHandler handler) {
    if (maximum_bytes == 0U || maximum_bytes > kMaxEstablishedIoBytes || !handler) {
      return false;
    }
    std::optional<PreparedRead> prepared = PrepareRead(std::move(handler));
    if (!prepared.has_value()) {
      return false;
    }
    const std::scoped_lock operation_lock(operation_mutex_);
    if (!operations_open_ || !open() ||
        read_claimed_.exchange(true, std::memory_order_acq_rel)) {
      return false;
    }
    try {
      asio::post(
          stream_->get_executor(), [self = shared_from_this(), maximum_bytes,
                                    completion = std::move(*prepared)]() mutable {
            if (!self->open()) {
              completion.state->error = ConnectionIoError::kClosed;
              self->NotifyCompletion(std::move(completion.callback));
              return;
            }
            std::shared_ptr<Bytes> buffer;
            try {
              buffer = std::make_shared<Bytes>(maximum_bytes);
            } catch (...) {
              completion.state->error = ConnectionIoError::kBusy;
              self->NotifyCompletion(std::move(completion.callback));
              self->CloseOnExecutor(true);
              return;
            }
            self->read_ = std::move(completion);
            try {
              self->stream_->async_read_some(
                  asio::buffer(*buffer),
                  [self, buffer](const asio::error_code& error,
                                 const std::size_t transferred) mutable {
                    if (!self->read_.has_value()) {
                      return;
                    }
                    PreparedRead completed = std::move(*self->read_);
                    self->read_.reset();
                    if (!self->open()) {
                      return;
                    }
                    if (error) {
                      completed.state->error = ConnectionIoError::kTransportFailure;
                      self->NotifyCompletion(std::move(completed.callback));
                      self->CloseOnExecutor(true);
                      return;
                    }
                    buffer->resize(transferred);
                    completed.state->error = ConnectionIoError::kNone;
                    completed.state->bytes = std::move(*buffer);
                    self->NotifyCompletion(std::move(completed.callback));
                  });
            } catch (...) {
              if (!self->read_.has_value()) {
                self->CloseOnExecutor(true);
                return;
              }
              PreparedRead failed = std::move(*self->read_);
              self->read_.reset();
              failed.state->error = ConnectionIoError::kBusy;
              self->NotifyCompletion(std::move(failed.callback));
              self->CloseOnExecutor(true);
            }
          });
      return true;
    } catch (...) {
      read_claimed_.store(false, std::memory_order_release);
      return false;
    }
  }

  [[nodiscard]] bool Write(Bytes bytes, WriteHandler handler) {
    if (bytes.empty() || bytes.size() > kMaxEstablishedIoBytes || !handler) {
      return false;
    }
    std::optional<PreparedWrite> prepared = PrepareWrite(std::move(handler));
    if (!prepared.has_value()) {
      return false;
    }
    const std::size_t size = bytes.size();
    const std::scoped_lock operation_lock(operation_mutex_);
    if (!operations_open_ || !open()) {
      return false;
    }
    if (pending_write_count_.fetch_add(1U, std::memory_order_acq_rel) >=
        kMaxEstablishedPendingWrites) {
      pending_write_count_.fetch_sub(1U, std::memory_order_acq_rel);
      return false;
    }
    if (queued_write_bytes_.fetch_add(size, std::memory_order_acq_rel) >
        kMaxEstablishedQueuedWriteBytes - size) {
      queued_write_bytes_.fetch_sub(size, std::memory_order_acq_rel);
      pending_write_count_.fetch_sub(1U, std::memory_order_acq_rel);
      return false;
    }
    try {
      asio::post(stream_->get_executor(),
                 [self = shared_from_this(), bytes = std::move(bytes), size,
                  completion = std::move(*prepared)]() mutable {
                   if (!self->open()) {
                     self->ReleaseWriteBytes(size);
                     completion.state->error = ConnectionIoError::kClosed;
                     self->NotifyCompletion(std::move(completion.callback));
                     return;
                   }
                   PendingWrite pending{
                       .bytes = std::move(bytes),
                       .completion = std::move(completion),
                   };
                   try {
                     self->writes_.push_back(std::move(pending));
                   } catch (...) {
                     self->ReleaseWriteBytes(size);
                     pending.completion.state->error = ConnectionIoError::kBusy;
                     self->NotifyCompletion(std::move(pending.completion.callback));
                     self->CloseOnExecutor(true);
                     return;
                   }
                   self->StartWrite();
                 });
      return true;
    } catch (...) {
      ReleaseWriteReservation(size);
      return false;
    }
  }

  void Close() {
    const std::scoped_lock operation_lock(operation_mutex_);
    if (!operations_open_ || !open()) {
      return;
    }
    operations_open_ = false;
    runtime_internal::CallbackDispatchResult result =
        runtime_internal::CallbackDispatchResult::kFailed;
    try {
      if (network_dispatcher_) {
        result = network_dispatcher_(this, close_callback_);
      }
    } catch (...) {
    }
    if (result == runtime_internal::CallbackDispatchResult::kFailed) {
      operations_open_ = true;
    }
  }

  void StopOnExecutor() { CloseOnExecutor(false); }

 private:
  [[nodiscard]] std::shared_ptr<CompletionReservation> PrepareReservation() {
    if (!completion_reserver_ || !completion_releaser_ || !completion_reserver_()) {
      return nullptr;
    }
    try {
      return std::make_shared<CompletionReservation>(completion_releaser_);
    } catch (...) {
      try {
        completion_releaser_();
      } catch (...) {
      }
      return nullptr;
    }
  }

  [[nodiscard]] std::optional<PreparedRead> PrepareRead(ReadHandler handler) {
    std::shared_ptr<CompletionReservation> reservation = PrepareReservation();
    if (reservation == nullptr) {
      return std::nullopt;
    }
    try {
      auto state = std::make_shared<ReadCompletionState>(ReadCompletionState{
          .handler = std::move(handler),
          .reservation = std::move(reservation),
      });
      PreparedRead prepared{
          .state = state,
      };
      prepared.callback = [self = shared_from_this(), state]() mutable {
        state->reservation->Release();
        self->read_claimed_.store(false, std::memory_order_release);
        ReadHandler completion = std::move(state->handler);
        if (!completion) {
          return;
        }
        try {
          completion(state->error, std::move(state->bytes));
        } catch (...) {
          self->Close();
        }
      };
      return prepared;
    } catch (...) {
      return std::nullopt;
    }
  }

  [[nodiscard]] std::optional<PreparedWrite> PrepareWrite(WriteHandler handler) {
    std::shared_ptr<CompletionReservation> reservation = PrepareReservation();
    if (reservation == nullptr) {
      return std::nullopt;
    }
    try {
      auto state = std::make_shared<WriteCompletionState>(WriteCompletionState{
          .handler = std::move(handler),
          .reservation = std::move(reservation),
      });
      PreparedWrite prepared{
          .state = state,
      };
      prepared.callback = [self = shared_from_this(), state]() mutable {
        state->reservation->Release();
        self->pending_write_count_.fetch_sub(1U, std::memory_order_acq_rel);
        WriteHandler completion = std::move(state->handler);
        if (!completion) {
          return;
        }
        try {
          completion(state->error);
        } catch (...) {
          self->Close();
        }
      };
      return prepared;
    } catch (...) {
      return std::nullopt;
    }
  }

  void StartWrite() {
    if (!open() || write_active_ || writes_.empty()) {
      return;
    }
    write_active_ = true;
    try {
      asio::async_write(
          *stream_, asio::buffer(writes_.front().bytes),
          [self = shared_from_this()](const asio::error_code& error,
                                      const std::size_t) {
            self->write_active_ = false;
            if (!self->open() || self->writes_.empty()) {
              return;
            }
            PendingWrite completed = std::move(self->writes_.front());
            self->writes_.pop_front();
            self->ReleaseWriteBytes(completed.bytes.size());
            if (error) {
              completed.completion.state->error = ConnectionIoError::kTransportFailure;
              self->NotifyCompletion(std::move(completed.completion.callback));
              self->CloseOnExecutor(true);
              return;
            }
            completed.completion.state->error = ConnectionIoError::kNone;
            self->NotifyCompletion(std::move(completed.completion.callback));
            self->StartWrite();
          });
    } catch (...) {
      write_active_ = false;
      PendingWrite failed = std::move(writes_.front());
      writes_.pop_front();
      ReleaseWriteBytes(failed.bytes.size());
      failed.completion.state->error = ConnectionIoError::kBusy;
      NotifyCompletion(std::move(failed.completion.callback));
      CloseOnExecutor(true);
    }
  }

  void NotifyCompletion(std::function<void()> callback) {
    if (!callback) {
      return;
    }
    runtime_internal::CallbackDispatchResult result =
        runtime_internal::CallbackDispatchResult::kFailed;
    try {
      result = callback_dispatcher_ ? callback_dispatcher_(callback)
                                    : runtime_internal::CallbackDispatchResult::kFailed;
    } catch (...) {
    }
    if (result == runtime_internal::CallbackDispatchResult::kFailed) {
      CloseOnExecutor(false);
    }
  }

  void CloseOnExecutor(const bool notify_handlers) {
    try {
      if (network_canceller_) {
        network_canceller_(this);
      }
    } catch (...) {
    }
    {
      const std::scoped_lock operation_lock(operation_mutex_);
      operations_open_ = false;
    }
    if (!open_.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    asio::error_code ignored;
    stream_->lowest_layer().cancel(ignored);
    stream_->lowest_layer().shutdown(runtime_internal::Tcp::socket::shutdown_both,
                                     ignored);
    stream_->lowest_layer().close(ignored);
    std::optional<PreparedRead> read = std::move(read_);
    read_.reset();
    if (notify_handlers && read.has_value()) {
      read->state->error = ConnectionIoError::kClosed;
      NotifyCompletion(std::move(read->callback));
    } else {
      read_claimed_.store(false, std::memory_order_release);
    }
    for (PendingWrite& write : writes_) {
      ReleaseWriteBytes(write.bytes.size());
      if (notify_handlers) {
        write.completion.state->error = ConnectionIoError::kClosed;
        NotifyCompletion(std::move(write.completion.callback));
      } else {
        pending_write_count_.fetch_sub(1U, std::memory_order_acq_rel);
      }
    }
    writes_.clear();
    write_active_ = false;
    if (close_handler_) {
      close_handler_(id_);
    }
  }

  void ReleaseWriteReservation(const std::size_t size) noexcept {
    ReleaseWriteBytes(size);
    pending_write_count_.fetch_sub(1U, std::memory_order_acq_rel);
  }

  void ReleaseWriteBytes(const std::size_t size) noexcept {
    queued_write_bytes_.fetch_sub(size, std::memory_order_acq_rel);
  }

  // Declared first so the executor and its services outlive the TLS stream.
  std::shared_ptr<void> executor_owner_;
  std::shared_ptr<security::tls::OpenSslTlsContext> context_;
  std::unique_ptr<runtime_internal::TlsStream> stream_;
  std::unique_ptr<EstablishedTlsChannel> channel_;
  ConnectionId id_{};
  DeviceId peer_device_id_{};
  bool inbound_{};
  std::function<void(const ConnectionId&)> close_handler_;
  std::function<bool()> completion_reserver_;
  std::function<void()> completion_releaser_;
  std::function<runtime_internal::CallbackDispatchResult(std::function<void()>&)>
      callback_dispatcher_;
  std::function<runtime_internal::CallbackDispatchResult(const void*,
                                                         std::function<void()>&)>
      network_dispatcher_;
  std::function<void(const void*)> network_canceller_;
  std::function<void()> close_callback_;
  mutable std::mutex operation_mutex_{};
  bool operations_open_{true};
  std::atomic_bool open_{true};
  std::atomic_bool read_claimed_{};
  std::atomic_size_t queued_write_bytes_{};
  std::atomic_size_t pending_write_count_{};
  std::optional<PreparedRead> read_{};
  std::deque<PendingWrite> writes_;
  bool write_active_{};
};

AuthenticatedEstablishedConnection::AuthenticatedEstablishedConnection(
    std::shared_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

AuthenticatedEstablishedConnection::~AuthenticatedEstablishedConnection() {
  implementation_->Close();
}

std::shared_ptr<AuthenticatedEstablishedConnection>
AuthenticatedEstablishedConnection::Create(
    std::unique_ptr<Construction> construction,
    std::shared_ptr<RuntimeLease>& runtime_lease) {
  runtime_lease.reset();
  try {
    auto implementation = std::make_shared<Impl>(std::move(construction));
    if (!implementation->Initialize()) {
      return nullptr;
    }
    auto connection = std::shared_ptr<AuthenticatedEstablishedConnection>(
        new AuthenticatedEstablishedConnection(std::move(implementation)));
    runtime_lease =
        std::shared_ptr<RuntimeLease>(new RuntimeLease(connection->implementation_));
    return connection;
  } catch (...) {
    return nullptr;
  }
}

AuthenticatedEstablishedConnection::RuntimeLease::RuntimeLease(
    std::shared_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

const ConnectionId& AuthenticatedEstablishedConnection::RuntimeLease::id()
    const noexcept {
  return implementation_->id();
}

void AuthenticatedEstablishedConnection::RuntimeLease::Close() {
  implementation_->Close();
}

void AuthenticatedEstablishedConnection::RuntimeLease::Stop() {
  implementation_->StopOnExecutor();
}

const ConnectionId& AuthenticatedEstablishedConnection::id() const noexcept {
  return implementation_->id();
}

const DeviceId& AuthenticatedEstablishedConnection::peer_device_id() const noexcept {
  return implementation_->peer_device_id();
}

bool AuthenticatedEstablishedConnection::inbound() const noexcept {
  return implementation_->inbound();
}

bool AuthenticatedEstablishedConnection::open() const noexcept {
  return implementation_->open();
}

bool AuthenticatedEstablishedConnection::DispatchChannel(ChannelHandler handler) {
  return implementation_->DispatchChannel(std::move(handler));
}

bool AuthenticatedEstablishedConnection::ReadSome(const std::size_t maximum_bytes,
                                                  ReadHandler handler) {
  return implementation_->ReadSome(maximum_bytes, std::move(handler));
}

bool AuthenticatedEstablishedConnection::Write(Bytes bytes, WriteHandler handler) {
  return implementation_->Write(std::move(bytes), std::move(handler));
}

void AuthenticatedEstablishedConnection::Close() { implementation_->Close(); }

}  // namespace xnn_transfer::core::session
