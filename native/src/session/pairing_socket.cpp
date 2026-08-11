#include <algorithm>
#include <asio/buffer.hpp>
#include <asio/write.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>

#include "connection_runtime_internal.hpp"
#include "internal.hpp"

namespace xnn_transfer::core::session::runtime_internal {
namespace {

constexpr std::chrono::milliseconds kPairingTimerInterval(50);
constexpr std::chrono::seconds kFrameAssemblyTimeout(10);
constexpr std::chrono::seconds kTerminalFlushTimeout(1);
constexpr std::chrono::milliseconds kTerminalWriteGrace(250);

[[nodiscard]] std::uint64_t NowMs() noexcept {
  const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  return value < 0 ? 0U : static_cast<std::uint64_t>(value);
}

[[nodiscard]] std::uint16_t ReadU16(
    const std::span<const std::uint8_t> bytes) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) |
                                    static_cast<std::uint16_t>(bytes[1]));
}

[[nodiscard]] std::uint32_t ReadU32(
    const std::span<const std::uint8_t> bytes) noexcept {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4U; ++index) {
    value = static_cast<std::uint32_t>((value << 8U) |
                                       static_cast<std::uint32_t>(bytes[index]));
  }
  return value;
}

}  // namespace

PairingSocket::PairingSocket(std::shared_ptr<security::tls::OpenSslTlsContext> context,
                             std::unique_ptr<TlsStream> stream,
                             std::unique_ptr<PairingAttempt> attempt,
                             ConnectionId connection_id, AttemptHandle attempt_handle,
                             const std::uint64_t request_id, const bool inbound,
                             const std::size_t write_fragment_bytes,
                             EventHandler event_handler, CloseHandler close_handler)
    : context_(std::move(context)),
      stream_(std::move(stream)),
      attempt_(std::move(attempt)),
      timer_(stream_->get_executor()),
      frame_timer_(stream_->get_executor()),
      terminal_flush_timer_(stream_->get_executor()),
      connection_id_(connection_id),
      attempt_handle_(attempt_handle),
      request_id_(request_id),
      inbound_(inbound),
      write_fragment_bytes_(write_fragment_bytes),
      event_handler_(std::move(event_handler)),
      close_handler_(std::move(close_handler)) {}

void PairingSocket::Start() {
  Apply(attempt_->Start(NowMs()));
  if (!closed_) {
    StartRead();
    ArmTimer();
  }
}

void PairingSocket::Decide(const security::tls::ConfirmationDecision decision) {
  if (closed_ || terminal_) {
    return;
  }
  Apply(attempt_->Decide(attempt_handle_, decision, NowMs()));
}

void PairingSocket::Cancel() {
  if (closed_ || terminal_) {
    return;
  }
  Apply(attempt_->Cancel(attempt_handle_, NowMs()));
}

void PairingSocket::Stop(const PairingError error, const bool publish) {
  if (closed_) {
    return;
  }
  if (terminal_) {
    CloseTransport();
    return;
  }
  terminal_ = true;
  if (publish && attempt_ != nullptr) {
    PairingUpdate update = attempt_->Shutdown();
    update.error = error;
    if (event_handler_) {
      event_handler_(*this, std::move(update));
    }
  }
  CloseTransport();
}

const ConnectionId& PairingSocket::connection_id() const noexcept {
  return connection_id_;
}

const AttemptHandle& PairingSocket::attempt_handle() const noexcept {
  return attempt_handle_;
}

std::uint64_t PairingSocket::request_id() const noexcept { return request_id_; }

bool PairingSocket::inbound() const noexcept { return inbound_; }

void PairingSocket::StartRead() {
  if (closed_ || terminal_) {
    return;
  }
  stream_->async_read_some(
      asio::buffer(read_buffer_),
      [self = shared_from_this()](const asio::error_code& error,
                                  const std::size_t transferred) {
        if (self->closed_ || self->terminal_) {
          return;
        }
        if (transferred == 0U) {
          self->Fail(PairingError::kInternalFailure);
          return;
        }
        const bool starts_frame = self->inbound_bytes_.empty();
        if (self->inbound_bytes_.size() + transferred > 2U * kMaxPairingFrameSize) {
          self->Fail(PairingError::kLimitExceeded);
          return;
        }
        try {
          self->inbound_bytes_.insert(
              self->inbound_bytes_.end(), self->read_buffer_.begin(),
              self->read_buffer_.begin() + static_cast<std::ptrdiff_t>(transferred));
        } catch (...) {
          self->Fail(PairingError::kLimitExceeded);
          return;
        }
        if (starts_frame) {
          self->ArmFrameAssembly();
        }
        if (!self->ProcessFrames()) {
          return;
        }
        if (error) {
          self->Fail(PairingError::kInternalFailure);
          return;
        }
        self->StartRead();
      });
}

bool PairingSocket::ProcessFrames() {
  while (!closed_ && !terminal_ && inbound_bytes_.size() >= kPairingFrameHeaderSize) {
    const PairingError header_error = internal::ValidateFrameHeader(
        std::span<const std::uint8_t>(inbound_bytes_).first(kPairingFrameHeaderSize));
    if (header_error != PairingError::kNone) {
      Fail(header_error);
      return false;
    }
    const std::uint32_t body_size =
        ReadU32(std::span<const std::uint8_t>(inbound_bytes_).subspan(16U, 4U));
    const std::size_t frame_size =
        kPairingFrameHeaderSize + static_cast<std::size_t>(body_size);
    const std::uint32_t sequence =
        ReadU32(std::span<const std::uint8_t>(inbound_bytes_).subspan(12U, 4U));
    if (sequence != next_inbound_sequence_ ||
        next_inbound_sequence_ == std::numeric_limits<std::uint32_t>::max()) {
      Fail(PairingError::kSequenceViolation);
      return false;
    }
    if (inbound_frames_ == kMaxInboundPairingFrames ||
        frame_size > kMaxInboundPairingBytes - inbound_frame_bytes_) {
      Fail(PairingError::kLimitExceeded);
      return false;
    }
    if (inbound_bytes_.size() < frame_size) {
      return true;
    }
    Bytes frame;
    try {
      frame.assign(inbound_bytes_.begin(),
                   inbound_bytes_.begin() + static_cast<std::ptrdiff_t>(frame_size));
      inbound_bytes_.erase(
          inbound_bytes_.begin(),
          inbound_bytes_.begin() + static_cast<std::ptrdiff_t>(frame_size));
    } catch (...) {
      Fail(PairingError::kLimitExceeded);
      return false;
    }
    CancelFrameAssembly();
    ++inbound_frames_;
    inbound_frame_bytes_ += frame_size;
    ++next_inbound_sequence_;
    Apply(attempt_->ReceiveFrame(frame, NowMs()));
    if (!closed_ && !terminal_ && !inbound_bytes_.empty()) {
      ArmFrameAssembly();
    }
  }
  return !closed_ && !terminal_;
}

void PairingSocket::Apply(PairingUpdate update) {
  if (closed_) {
    return;
  }
  const bool terminal = update.terminal;
  Bytes outbound;
  try {
    outbound = update.outbound_frame;
  } catch (...) {
    Fail(PairingError::kLimitExceeded);
    return;
  }
  WriteAction action = WriteAction::kNone;
  if (outbound.size() >= 10U) {
    const std::uint16_t type =
        ReadU16(std::span<const std::uint8_t>(outbound).subspan(8U, 2U));
    if (type == static_cast<std::uint16_t>(internal::PairingMessageType::kSelectAck)) {
      action = WriteAction::kSelectionAck;
    } else if (type == static_cast<std::uint16_t>(
                           internal::PairingMessageType::kDecision) &&
               !terminal) {
      action = WriteAction::kConfirmedDecision;
    }
  }
  if (!outbound.empty()) {
    try {
      writes_.push_back(PendingWrite{
          .bytes = std::move(outbound),
          .action = action,
      });
    } catch (...) {
      Fail(PairingError::kLimitExceeded);
      return;
    }
  }
  if (terminal) {
    terminal_ = true;
    static_cast<void>(timer_.cancel());
    CancelFrameAssembly();
    ArmTerminalFlush();
  }

  if (event_handler_) {
    event_handler_(*this, std::move(update));
  }
  if (closed_) {
    return;
  }
  StartWrite();
  if (terminal_ && writes_.empty()) {
    CloseTransport();
  }
}

void PairingSocket::StartWrite() {
  if (closed_ || write_active_ || writes_.empty()) {
    return;
  }
  write_active_ = true;
  PendingWrite& write = writes_.front();
  const std::size_t remaining = write.bytes.size() - write.offset;
  const std::size_t fragment = std::min(remaining, write_fragment_bytes_);
  asio::async_write(
      *stream_, asio::buffer(write.bytes.data() + write.offset, fragment),
      [self = shared_from_this()](const asio::error_code& error,
                                  const std::size_t transferred) {
        self->write_active_ = false;
        if (self->closed_ || self->writes_.empty()) {
          return;
        }
        if (error) {
          if (self->terminal_) {
            self->CloseTransport();
            return;
          }
          self->Fail(PairingError::kInternalFailure);
          return;
        }
        self->writes_.front().offset += transferred;
        if (self->writes_.front().offset < self->writes_.front().bytes.size()) {
          self->StartWrite();
          return;
        }
        const WriteAction action = self->writes_.front().action;
        self->writes_.pop_front();
        if (!self->terminal_ && action == WriteAction::kSelectionAck) {
          self->Apply(
              self->attempt_->LocalSelectionAckWritten(self->attempt_handle_, NowMs()));
        } else if (!self->terminal_ && action == WriteAction::kConfirmedDecision) {
          self->Apply(
              self->attempt_->LocalDecisionWritten(self->attempt_handle_, NowMs()));
        }
        if (self->closed_) {
          return;
        }
        if (self->terminal_ && self->writes_.empty()) {
          self->ShortenTerminalFlushAfterWrite();
          return;
        }
        self->StartWrite();
      });
}

void PairingSocket::ArmTimer() {
  if (closed_ || terminal_) {
    return;
  }
  timer_.expires_after(kPairingTimerInterval);
  timer_.async_wait([self = shared_from_this()](const asio::error_code& error) {
    if (error || self->closed_ || self->terminal_) {
      return;
    }
    self->Apply(self->attempt_->Advance(NowMs()));
    self->ArmTimer();
  });
}

void PairingSocket::ArmFrameAssembly() {
  if (closed_ || terminal_ || frame_assembly_active_ || inbound_bytes_.empty()) {
    return;
  }
  frame_assembly_active_ = true;
  frame_timer_.expires_after(kFrameAssemblyTimeout);
  frame_timer_.async_wait([self = shared_from_this()](const asio::error_code& error) {
    if (!error && !self->closed_ && !self->terminal_ && self->frame_assembly_active_) {
      self->Fail(PairingError::kTimeout);
    }
  });
}

void PairingSocket::CancelFrameAssembly() {
  if (!frame_assembly_active_) {
    return;
  }
  frame_assembly_active_ = false;
  static_cast<void>(frame_timer_.cancel());
}

void PairingSocket::ArmTerminalFlush() {
  if (closed_ || !terminal_ || terminal_flush_active_) {
    return;
  }
  terminal_flush_active_ = true;
  terminal_flush_timer_.expires_after(kTerminalFlushTimeout);
  terminal_flush_timer_.async_wait(
      [self = shared_from_this()](const asio::error_code& error) {
        if (!error) {
          self->CloseTransport();
        }
      });
}

void PairingSocket::ShortenTerminalFlushAfterWrite() {
  if (closed_ || !terminal_ || !writes_.empty()) {
    return;
  }
  const auto deadline = std::chrono::steady_clock::now() + kTerminalWriteGrace;
  if (terminal_flush_active_ && terminal_flush_timer_.expiry() <= deadline) {
    return;
  }
  terminal_flush_active_ = true;
  terminal_flush_timer_.expires_at(deadline);
  terminal_flush_timer_.async_wait(
      [self = shared_from_this()](const asio::error_code& error) {
        if (!error) {
          self->CloseTransport();
        }
      });
}

void PairingSocket::Fail(const PairingError error) {
  if (closed_) {
    return;
  }
  terminal_ = true;
  PairingUpdate update = attempt_->Shutdown();
  update.error = error;
  if (event_handler_) {
    event_handler_(*this, std::move(update));
  }
  CloseTransport();
}

void PairingSocket::CloseTransport() {
  if (closed_) {
    return;
  }
  closed_ = true;
  asio::error_code ignored;
  static_cast<void>(timer_.cancel());
  CancelFrameAssembly();
  static_cast<void>(terminal_flush_timer_.cancel());
  stream_->lowest_layer().cancel(ignored);
  stream_->lowest_layer().shutdown(Tcp::socket::shutdown_both, ignored);
  stream_->lowest_layer().close(ignored);
  writes_.clear();
  if (close_handler_) {
    close_handler_(connection_id_);
  }
}

}  // namespace xnn_transfer::core::session::runtime_internal
