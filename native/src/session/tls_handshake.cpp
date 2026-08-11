#include <asio/ssl/stream_base.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "connection_runtime_internal.hpp"

namespace xnn_transfer::core::session::runtime_internal {

TlsHandshake::TlsHandshake(
    std::shared_ptr<security::tls::OpenSslTlsContext> context,
    std::unique_ptr<TlsStream> stream, ConnectionId connection_id,
    Tcp::endpoint peer_endpoint, const Mode mode, const std::uint64_t request_id,
    std::string peer_display_label, std::optional<DeviceId> expected_peer,
    std::unique_ptr<PairingAdmissionLease> admission,
    const std::uint64_t pairing_window_generation, const std::uint64_t timeout_ms,
    CompletionHandler completion_handler, FailureHandler failure_handler)
    : context_(std::move(context)),
      stream_(std::move(stream)),
      timer_(stream_->get_executor()),
      connection_id_(connection_id),
      peer_endpoint_(std::move(peer_endpoint)),
      mode_(mode),
      request_id_(request_id),
      peer_display_label_(std::move(peer_display_label)),
      expected_peer_(std::move(expected_peer)),
      admission_(std::move(admission)),
      pairing_window_generation_(pairing_window_generation),
      timeout_ms_(timeout_ms),
      completion_handler_(std::move(completion_handler)),
      failure_handler_(std::move(failure_handler)) {}

void TlsHandshake::StartServer() {
  ArmTimeout();
  stream_->async_handshake(asio::ssl::stream_base::server,
                           [self = shared_from_this()](const asio::error_code& error) {
                             self->Complete(error);
                           });
}

void TlsHandshake::StartClient(const Tcp::endpoint& endpoint) {
  ArmTimeout();
  stream_->lowest_layer().async_connect(
      endpoint, [self = shared_from_this()](const asio::error_code& error) {
        if (error) {
          self->Complete(error);
          return;
        }
        self->stream_->async_handshake(asio::ssl::stream_base::client,
                                       [self](const asio::error_code& handshake_error) {
                                         self->Complete(handshake_error);
                                       });
      });
}

void TlsHandshake::Stop() {
  if (finished_) {
    return;
  }
  finished_ = true;
  asio::error_code ignored;
  static_cast<void>(timer_.cancel());
  stream_->lowest_layer().cancel(ignored);
  stream_->lowest_layer().close(ignored);
}

std::unique_ptr<TlsStream> TlsHandshake::TakeStream() { return std::move(stream_); }

std::unique_ptr<PairingAdmissionLease> TlsHandshake::TakeAdmission() {
  return std::move(admission_);
}

const std::shared_ptr<security::tls::OpenSslTlsContext>& TlsHandshake::context()
    const noexcept {
  return context_;
}

const ConnectionId& TlsHandshake::connection_id() const noexcept {
  return connection_id_;
}

const Tcp::endpoint& TlsHandshake::peer_endpoint() const noexcept {
  return peer_endpoint_;
}

TlsHandshake::Mode TlsHandshake::mode() const noexcept { return mode_; }

std::uint64_t TlsHandshake::request_id() const noexcept { return request_id_; }

std::uint64_t TlsHandshake::pairing_window_generation() const noexcept {
  return pairing_window_generation_;
}

const std::string& TlsHandshake::peer_display_label() const noexcept {
  return peer_display_label_;
}

const std::optional<DeviceId>& TlsHandshake::expected_peer() const noexcept {
  return expected_peer_;
}

void TlsHandshake::ArmTimeout() {
  timer_.expires_after(std::chrono::milliseconds(timeout_ms_));
  timer_.async_wait([self = shared_from_this()](const asio::error_code& error) {
    if (error || self->finished_) {
      return;
    }
    self->finished_ = true;
    asio::error_code ignored;
    self->stream_->lowest_layer().cancel(ignored);
    self->stream_->lowest_layer().close(ignored);
    if (self->failure_handler_) {
      self->failure_handler_(self, ConnectionIoError::kTransportFailure,
                             security::tls::SecurityError::kHandshakeIncomplete, true);
    }
  });
}

void TlsHandshake::Complete(const asio::error_code& error) {
  if (finished_) {
    return;
  }
  finished_ = true;
  asio::error_code ignored;
  static_cast<void>(timer_.cancel());
  if (error) {
    stream_->lowest_layer().close(ignored);
    if (failure_handler_) {
      failure_handler_(shared_from_this(), ConnectionIoError::kTransportFailure,
                       security::tls::SecurityError::kHandshakeIncomplete, false);
    }
    return;
  }
  if (completion_handler_) {
    completion_handler_(shared_from_this());
  }
}

}  // namespace xnn_transfer::core::session::runtime_internal
