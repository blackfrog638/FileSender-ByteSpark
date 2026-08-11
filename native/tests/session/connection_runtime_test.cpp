#include "xnn_transfer/core/session/connection_runtime.hpp"

#include <openssl/ssl.h>

#include <array>
#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../../src/session/connection_runtime_internal.hpp"
#include "pairing_vectors.hpp"
#include "test_support.hpp"

namespace {

using namespace session_test;
using namespace std::chrono_literals;

constexpr std::string_view kInitiatorSeed =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
constexpr std::string_view kResponderSeed =
    "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";

int failures = 0;

void Expect(const bool condition, const std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

void RunTest(const std::string_view name, const std::function<void()>& test) {
  std::cout << "[ RUN      ] " << name << std::endl;
  test();
  Expect(
      session::runtime_internal::PairingAdmissionBridge::ResetProcessStateForTesting(),
      "test releases all process admission owners and leases");
  std::cout << "[     DONE ] " << name << std::endl;
}

void ReportStep(const std::string_view step) {
  std::cout << "[     STEP ] " << step << std::endl;
}

session::NetworkEndpoint Loopback(const std::uint16_t port) {
  std::array<std::uint8_t, 16> address{};
  address.back() = 1U;
  return session::NetworkEndpoint::V6(address, 0U, port);
}

std::uint32_t ReadU32(const std::span<const std::uint8_t> bytes) {
  std::uint32_t value = 0;
  for (const std::uint8_t byte : bytes.first(4U)) {
    value = static_cast<std::uint32_t>((value << 8U) | byte);
  }
  return value;
}

bool ReadPairingFrame(asio::ssl::stream<asio::ip::tcp::socket>& stream,
                      session::Bytes& frame) {
  frame.assign(session::kPairingFrameHeaderSize, 0U);
  asio::error_code error;
  static_cast<void>(asio::read(stream, asio::buffer(frame), error));
  if (error) {
    return false;
  }
  const std::uint32_t body_size =
      ReadU32(std::span<const std::uint8_t>(frame).subspan(16U, 4U));
  if (body_size > session::kMaxPairingBodySize) {
    return false;
  }
  frame.resize(session::kPairingFrameHeaderSize + body_size);
  if (body_size != 0U) {
    static_cast<void>(
        asio::read(stream,
                   asio::buffer(frame.data() + session::kPairingFrameHeaderSize,
                                static_cast<std::size_t>(body_size)),
                   error));
  }
  return !error;
}

class PairingEvents final {
 public:
  void Add(const session::PairingRuntimeEvent& event) {
    const std::scoped_lock lock(mutex_);
    events_.push_back(event);
    condition_.notify_all();
  }

  template <typename Predicate>
  std::optional<session::PairingRuntimeEvent> Wait(
      Predicate predicate, const std::chrono::milliseconds timeout = 5s) {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, timeout, [this, &predicate] {
          return std::any_of(events_.begin(), events_.end(), predicate);
        })) {
      return std::nullopt;
    }
    const auto found = std::find_if(events_.begin(), events_.end(), predicate);
    return found == events_.end() ? std::nullopt
                                  : std::optional<session::PairingRuntimeEvent>(*found);
  }

  [[nodiscard]] std::size_t size() const {
    const std::scoped_lock lock(mutex_);
    return events_.size();
  }

 private:
  mutable std::mutex mutex_{};
  std::condition_variable condition_{};
  std::vector<session::PairingRuntimeEvent> events_{};
};

class EstablishedEvents final {
 public:
  void Add(const session::EstablishedRuntimeEvent& event) {
    const std::scoped_lock lock(mutex_);
    events_.push_back(event);
    condition_.notify_all();
  }

  template <typename Predicate>
  std::optional<session::EstablishedRuntimeEvent> Wait(Predicate predicate) {
    std::unique_lock lock(mutex_);
    if (!condition_.wait_for(lock, 5s, [this, &predicate] {
          return std::any_of(events_.begin(), events_.end(), predicate);
        })) {
      return std::nullopt;
    }
    const auto found = std::find_if(events_.begin(), events_.end(), predicate);
    return found == events_.end()
               ? std::nullopt
               : std::optional<session::EstablishedRuntimeEvent>(*found);
  }

  void Clear() {
    std::vector<session::EstablishedRuntimeEvent> released;
    {
      const std::scoped_lock lock(mutex_);
      released.swap(events_);
    }
  }

 private:
  std::mutex mutex_{};
  std::condition_variable condition_{};
  std::vector<session::EstablishedRuntimeEvent> events_{};
};

class RuntimePair final {
 public:
  RuntimePair()
      : left_identity_(DecodeArray<32>(kInitiatorSeed)),
        right_identity_(DecodeArray<32>(kResponderSeed)) {}

  ~RuntimePair() { Stop(); }

  bool Start(const std::uint64_t handshake_timeout_ms = 1'000U) {
    Expect(left_identity_.repository.Open().ok(), "left identity opens");
    Expect(right_identity_.repository.Open().ok(), "right identity opens");
    if (!left_identity_.repository.ready() || !right_identity_.repository.ready()) {
      return false;
    }

    left_ = std::make_unique<session::AuthenticatedConnectionRuntime>(
        left_identity_.repository,
        session::AuthenticatedConnectionRuntimeConfig{
            .listen_port = 0U,
            .tls_handshake_timeout_ms = handshake_timeout_ms,
            .pairing_write_fragment_bytes = 7U,
            .pairing_offer = InitiatorOffer(),
        },
        [this](const session::PairingRuntimeEvent& event) {
          left_pairing_.Add(event);
          if (left_hook_) {
            left_hook_(event);
          }
        },
        [this](const session::EstablishedRuntimeEvent& event) {
          left_established_.Add(event);
        });
    right_ = std::make_unique<session::AuthenticatedConnectionRuntime>(
        right_identity_.repository,
        session::AuthenticatedConnectionRuntimeConfig{
            .listen_port = 0U,
            .tls_handshake_timeout_ms = handshake_timeout_ms,
            .pairing_write_fragment_bytes = 7U,
            .pairing_offer = ResponderOffer(),
        },
        [this](const session::PairingRuntimeEvent& event) {
          right_pairing_.Add(event);
        },
        [this](const session::EstablishedRuntimeEvent& event) {
          right_established_.Add(event);
        });
    const bool started = left_->Start() && right_->Start();
    Expect(started, "both authenticated runtimes start");
    return started;
  }

  void Stop() {
    if (left_) {
      left_->Stop();
    }
    if (right_) {
      right_->Stop();
    }
  }

  bool Begin(const std::uint64_t window_ms = 5'000U) {
    const bool windows =
        left_->OpenPairingWindow(window_ms) && right_->OpenPairingWindow(window_ms);
    Expect(windows, "both pairing windows open");
    const bool started = windows && left_->StartPairing(Loopback(right_->listen_port()),
                                                        42U, "right peer");
    Expect(started, "outbound loopback pairing starts");
    return started;
  }

  std::optional<session::PairingRuntimeEvent> LeftPrompt() {
    return left_pairing_.Wait([](const session::PairingRuntimeEvent& event) {
      return event.update.prompt.has_value();
    });
  }

  std::optional<session::PairingRuntimeEvent> RightPrompt() {
    return right_pairing_.Wait([](const session::PairingRuntimeEvent& event) {
      return event.update.prompt.has_value();
    });
  }

  std::optional<session::PairingRuntimeEvent> LeftTerminal() {
    return left_pairing_.Wait([](const session::PairingRuntimeEvent& event) {
      return event.update.terminal;
    });
  }

  std::optional<session::PairingRuntimeEvent> RightTerminal() {
    return right_pairing_.Wait([](const session::PairingRuntimeEvent& event) {
      return event.update.terminal;
    });
  }

  IdentityFixture left_identity_;
  IdentityFixture right_identity_;
  PairingEvents left_pairing_{};
  PairingEvents right_pairing_{};
  EstablishedEvents left_established_{};
  EstablishedEvents right_established_{};
  std::function<void(const session::PairingRuntimeEvent&)> left_hook_{};
  std::unique_ptr<session::AuthenticatedConnectionRuntime> left_{};
  std::unique_ptr<session::AuthenticatedConnectionRuntime> right_{};
};

void TestConfirmedPairingAndEstablishedIo() {
  RuntimePair pair;
  if (!pair.Start() || !pair.Begin()) {
    return;
  }
  ReportStep("pairing started");
  const auto left_prompt = pair.LeftPrompt();
  const auto right_prompt = pair.RightPrompt();
  Expect(left_prompt.has_value() && right_prompt.has_value(),
         "both real peers expose a SAS prompt");
  if (!left_prompt.has_value() || !right_prompt.has_value()) {
    return;
  }
  Expect(left_prompt->update.prompt->sas_word_indices ==
             right_prompt->update.prompt->sas_word_indices,
         "both peers derive the same SAS words");
  Expect(
      pair.left_->Decide(left_prompt->attempt, tls::ConfirmationDecision::kConfirm) &&
          pair.right_->Decide(right_prompt->attempt,
                              tls::ConfirmationDecision::kConfirm),
      "both SAS decisions are accepted");

  const auto left_terminal = pair.LeftTerminal();
  const auto right_terminal = pair.RightTerminal();
  Expect(left_terminal.has_value() && right_terminal.has_value(),
         "both pairing transcripts terminate");
  if (!left_terminal.has_value() || !right_terminal.has_value()) {
    return;
  }
  ReportStep("pairing confirmed");
  Expect(left_terminal->update.error == session::PairingError::kNone &&
             left_terminal->update.paired_peer.has_value() &&
             right_terminal->update.error == session::PairingError::kNone &&
             right_terminal->update.paired_peer.has_value(),
         "confirmed peers commit active trust");
  if (!left_terminal->update.paired_peer.has_value()) {
    return;
  }

  Expect(pair.left_->OpenEstablished(Loopback(pair.right_->listen_port()),
                                     *left_terminal->update.paired_peer, 99U),
         "established TLS client starts from committed trust");
  auto outbound =
      pair.left_established_.Wait([](const session::EstablishedRuntimeEvent& event) {
        return event.request_id == 99U && event.connection != nullptr;
      });
  auto inbound =
      pair.right_established_.Wait([](const session::EstablishedRuntimeEvent& event) {
        return event.inbound && event.connection != nullptr;
      });
  Expect(outbound.has_value() && inbound.has_value(),
         "active exact pins dispatch both established capabilities");
  if (!outbound.has_value() || !inbound.has_value()) {
    return;
  }
  ReportStep("established channels dispatched");

  std::mutex io_mutex;
  std::condition_variable io_condition;
  session::Bytes received;
  bool write_complete = false;
  const session::Bytes payload{0x01U, 0x23U, 0x45U, 0x67U, 0x89U};
  Expect(inbound->connection->ReadSome(
             64U,
             [&](const session::ConnectionIoError error, session::Bytes bytes) {
               const std::scoped_lock lock(io_mutex);
               if (error == session::ConnectionIoError::kNone) {
                 received = std::move(bytes);
               }
               io_condition.notify_all();
             }),
         "established receiver schedules one bounded read");
  Expect(outbound->connection->Write(payload,
                                     [&](const session::ConnectionIoError error) {
                                       const std::scoped_lock lock(io_mutex);
                                       write_complete =
                                           error == session::ConnectionIoError::kNone;
                                       io_condition.notify_all();
                                     }),
         "established sender queues a bounded write");
  {
    std::unique_lock lock(io_mutex);
    Expect(io_condition.wait_for(lock, 5s,
                                 [&] { return write_complete && received == payload; }),
           "established byte stream preserves payload bytes");
  }
  ReportStep("established I/O completed");

  bool channel_dispatched = false;
  std::mutex channel_mutex;
  std::condition_variable channel_condition;
  Expect(outbound->connection->DispatchChannel(
             [&](session::EstablishedTlsChannel& channel) {
               const std::scoped_lock lock(channel_mutex);
               channel_dispatched =
                   channel.peer_device_id() == *left_terminal->update.paired_peer;
               channel_condition.notify_all();
             }),
         "established channel operation is serialized");
  {
    std::unique_lock lock(channel_mutex);
    Expect(channel_condition.wait_for(lock, 5s, [&] { return channel_dispatched; }),
           "serialized channel exposes the authenticated peer");
  }
  ReportStep("channel callback completed");

  std::mutex bound_mutex;
  std::condition_variable bound_condition;
  std::size_t accepted_writes = 0;
  bool extra_write_rejected = false;
  auto outbound_connection = outbound->connection;
  Expect(outbound_connection->DispatchChannel([&, outbound_connection](
                                                  session::EstablishedTlsChannel&) {
    for (std::size_t index = 0; index < session::kMaxEstablishedPendingWrites;
         ++index) {
      if (outbound_connection->Write(session::Bytes{static_cast<std::uint8_t>(index)},
                                     [](const session::ConnectionIoError) {})) {
        ++accepted_writes;
      }
    }
    extra_write_rejected = !outbound_connection->Write(
        session::Bytes{0xffU}, [](const session::ConnectionIoError) {});
    const std::scoped_lock lock(bound_mutex);
    bound_condition.notify_all();
  }),
         "write-bound probe is serialized on the connection executor");
  {
    std::unique_lock lock(bound_mutex);
    Expect(bound_condition.wait_for(
               lock, 5s,
               [&] {
                 return accepted_writes == session::kMaxEstablishedPendingWrites &&
                        extra_write_rejected;
               }),
           "established write queue rejects work beyond its hard bound");
  }
  ReportStep("write bound verified");

  bool close_read_completed = false;
  session::ConnectionIoError close_read_error = session::ConnectionIoError::kNone;
  Expect(outbound->connection->ReadSome(
             1U,
             [&](const session::ConnectionIoError error, session::Bytes) {
               const std::scoped_lock lock(io_mutex);
               close_read_error = error;
               close_read_completed = true;
               io_condition.notify_all();
             }),
         "explicit-close probe schedules an accepted read");
  outbound->connection->Close();
  {
    std::unique_lock lock(io_mutex);
    Expect(io_condition.wait_for(lock, 5s, [&] { return close_read_completed; }) &&
               close_read_error == session::ConnectionIoError::kClosed,
           "explicit close completes the accepted read exactly once");
  }

  const auto stop_started = std::chrono::steady_clock::now();
  pair.Stop();
  ReportStep("runtime pair stopped");
  Expect(std::chrono::steady_clock::now() - stop_started < 2s,
         "runtime stop is a bounded executor barrier");
  Expect(!outbound->connection->open() && !inbound->connection->open(),
         "runtime stop closes externally retained established handles");

  pair.left_.reset();
  pair.right_.reset();
  ReportStep("public runtime owners released");
  outbound_connection.reset();
  outbound.reset();
  inbound.reset();
  pair.left_established_.Clear();
  pair.right_established_.Clear();
  ReportStep("retained established handles released");
}

void RunAuthenticatedRejectionIteration(const std::size_t iteration) {
  RuntimePair pair;
  if (!pair.Start() || !pair.Begin()) {
    return;
  }
  const auto left_prompt = pair.LeftPrompt();
  const auto right_prompt = pair.RightPrompt();
  Expect(left_prompt.has_value() && right_prompt.has_value(),
         "rejection case reaches both SAS prompts");
  if (!left_prompt.has_value() || !right_prompt.has_value()) {
    return;
  }
  Expect(
      pair.right_->Decide(right_prompt->attempt, tls::ConfirmationDecision::kConfirm) &&
          pair.left_->Decide(left_prompt->attempt, tls::ConfirmationDecision::kReject),
      "opposite confirmation and rejection are queued");
  const auto left_terminal = pair.LeftTerminal();
  const auto right_terminal = pair.RightTerminal();
  Expect(left_terminal.has_value() && right_terminal.has_value(),
         "authenticated rejection terminates both peers");
  if (left_terminal.has_value() && right_terminal.has_value()) {
    const bool distinct =
        left_terminal->update.error == session::PairingError::kLocalReject &&
        right_terminal->update.error == session::PairingError::kAuthenticatedReject;
    if (!distinct) {
      std::cerr << "rejection iteration " << iteration << " errors: left="
                << session::PairingErrorName(left_terminal->update.error)
                << " right=" << session::PairingErrorName(right_terminal->update.error)
                << '\n';
    }
    Expect(distinct, "local and authenticated rejection errors remain distinct");
  }
  Expect(pair.left_identity_.repository.peers().empty() &&
             pair.right_identity_.repository.peers().empty(),
         "rejected pairing commits no trust");
}

void TestAuthenticatedRejection() {
  for (std::size_t iteration = 0; iteration < 20U; ++iteration) {
    RunAuthenticatedRejectionIteration(iteration);
    Expect(session::runtime_internal::PairingAdmissionBridge::
               ResetProcessStateForTesting(),
           "rejection stress releases process admission owners and leases");
  }
}

void TestPairingDeadline() {
  RuntimePair pair;
  if (!pair.Start() || !pair.Begin(500U)) {
    return;
  }
  Expect(pair.LeftPrompt().has_value() && pair.RightPrompt().has_value(),
         "timeout case reaches the decision phase");
  const auto left_terminal = pair.LeftTerminal();
  const auto right_terminal = pair.RightTerminal();
  Expect(left_terminal.has_value() && right_terminal.has_value(),
         "monotonic deadline closes both undecided peers");
  if (left_terminal.has_value() && right_terminal.has_value()) {
    Expect(left_terminal->update.error == session::PairingError::kTimeout &&
               right_terminal->update.error == session::PairingError::kTimeout,
           "undecided pairing reports timeout on both peers");
  }
}

void TestPartialHandshakeTimeout() {
  IdentityFixture identity_fixture{DecodeArray<32>(kInitiatorSeed)};
  Expect(identity_fixture.repository.Open().ok(), "partial-handshake identity opens");
  if (!identity_fixture.repository.ready()) {
    return;
  }
  session::AuthenticatedConnectionRuntime runtime(
      identity_fixture.repository,
      session::AuthenticatedConnectionRuntimeConfig{
          .listen_port = 0U,
          .tls_handshake_timeout_ms = 100U,
          .pairing_write_fragment_bytes = 7U,
          .pairing_offer = InitiatorOffer(),
      },
      {}, {});
  Expect(runtime.Start(), "partial-handshake runtime starts");
  if (!runtime.running()) {
    return;
  }

  asio::io_context context;
  asio::ip::tcp::socket socket(context);
  asio::error_code error;
  socket.connect(
      asio::ip::tcp::endpoint(asio::ip::address_v6::loopback(), runtime.listen_port()),
      error);
  Expect(!error, "raw loopback socket connects without a ClientHello");
  std::this_thread::sleep_for(250ms);
  socket.non_blocking(true, error);
  std::array<std::uint8_t, 1> byte{};
  static_cast<void>(socket.read_some(asio::buffer(byte), error));
  Expect(error == asio::error::eof || error == asio::error::connection_reset ||
             error == asio::error::operation_aborted,
         "partial TLS handshake is closed at its bounded deadline");
  runtime.Stop();
}

void TestHandshakeHardLimit() {
  IdentityFixture identity_fixture{DecodeArray<32>(kInitiatorSeed)};
  Expect(identity_fixture.repository.Open().ok(), "handshake-limit identity opens");
  if (!identity_fixture.repository.ready()) {
    return;
  }
  session::AuthenticatedConnectionRuntime runtime(
      identity_fixture.repository,
      session::AuthenticatedConnectionRuntimeConfig{
          .listen_port = 0U,
          .tls_handshake_timeout_ms = session::kDefaultTlsHandshakeTimeoutMs + 1U,
          .pairing_write_fragment_bytes = 7U,
          .pairing_offer = InitiatorOffer(),
      },
      {}, {});
  Expect(!runtime.Start(), "TLS handshake policy cannot exceed five seconds");
}

void TestStartFailureRollsBackListener() {
  IdentityFixture first_identity{DecodeArray<32>(kInitiatorSeed)};
  IdentityFixture second_identity{DecodeArray<32>(kResponderSeed)};
  Expect(
      first_identity.repository.Open().ok() && second_identity.repository.Open().ok(),
      "start-rollback identities open");
  if (!first_identity.repository.ready() || !second_identity.repository.ready()) {
    return;
  }
  const session::AuthenticatedConnectionRuntimeConfig dynamic_config{
      .listen_port = 0U,
      .tls_handshake_timeout_ms = 1'000U,
      .pairing_write_fragment_bytes = 7U,
      .pairing_offer = InitiatorOffer(),
  };
  session::AuthenticatedConnectionRuntime occupying(first_identity.repository,
                                                    dynamic_config, {}, {});
  Expect(occupying.Start(), "start-rollback occupying runtime starts");
  if (!occupying.running()) {
    return;
  }
  const std::uint16_t occupied_port = occupying.listen_port();
  const session::AuthenticatedConnectionRuntimeConfig fixed_config{
      .listen_port = occupied_port,
      .tls_handshake_timeout_ms = 1'000U,
      .pairing_write_fragment_bytes = 7U,
      .pairing_offer = ResponderOffer(),
  };
  session::AuthenticatedConnectionRuntime failed(second_identity.repository,
                                                 fixed_config, {}, {});
  Expect(!failed.Start() && failed.listen_port() == 0U && !failed.running(),
         "failed start rolls back its listener state");
  occupying.Stop();

  session::AuthenticatedConnectionRuntime recovered(second_identity.repository,
                                                    fixed_config, {}, {});
  Expect(recovered.Start() && recovered.listen_port() == occupied_port,
         "rolled-back fixed port can be acquired after the conflict clears");
  recovered.Stop();
}

void TestPairingAlpnRequiresOpenWindow() {
  IdentityFixture client{DecodeArray<32>(kInitiatorSeed)};
  IdentityFixture server{DecodeArray<32>(kResponderSeed)};
  Expect(client.repository.Open().ok() && server.repository.Open().ok(),
         "pairing-window ALPN identities open");
  if (!client.repository.ready() || !server.repository.ready()) {
    return;
  }
  session::AuthenticatedConnectionRuntime runtime(
      server.repository,
      session::AuthenticatedConnectionRuntimeConfig{
          .listen_port = 0U,
          .tls_handshake_timeout_ms = 1'000U,
          .pairing_write_fragment_bytes = 7U,
          .pairing_offer = ResponderOffer(),
      },
      {}, {});
  Expect(runtime.Start(), "pairing-window ALPN runtime starts");
  auto client_context = tls::OpenSslTlsContext::Create(
      tls::TlsEndpointRole::kClient, client.repository, tls::kPairingAlpn);
  if (!runtime.running() || !client_context.ok() ||
      SSL_CTX_up_ref(client_context.value->native_handle()) != 1) {
    Expect(false, "pairing-window ALPN client configures");
    runtime.Stop();
    return;
  }
  asio::ssl::context asio_context(client_context.value->native_handle());
  const auto handshake = [&](asio::error_code& error) {
    asio::io_context io_context;
    asio::ssl::stream<asio::ip::tcp::socket> stream(io_context, asio_context);
    stream.lowest_layer().connect(
        asio::ip::tcp::endpoint(asio::ip::address_v6::loopback(),
                                runtime.listen_port()),
        error);
    if (!error) {
      stream.handshake(asio::ssl::stream_base::client, error);
    }
  };

  asio::error_code closed_error;
  handshake(closed_error);
  Expect(static_cast<bool>(closed_error),
         "closed pairing window rejects pairing ALPN during ClientHello");
  Expect(runtime.OpenPairingWindow(2'000U),
         "pairing window opens for the same dispatcher");
  asio::error_code open_error;
  handshake(open_error);
  Expect(!open_error, "open pairing window admits pairing ALPN");
  runtime.Stop();
}

void TestPreTlsAdmissionPerSource() {
  IdentityFixture identity_fixture{DecodeArray<32>(kInitiatorSeed)};
  Expect(identity_fixture.repository.Open().ok(), "pre-TLS admission identity opens");
  if (!identity_fixture.repository.ready()) {
    return;
  }
  session::AuthenticatedConnectionRuntime runtime(
      identity_fixture.repository,
      session::AuthenticatedConnectionRuntimeConfig{
          .listen_port = 0U,
          .tls_handshake_timeout_ms = 1'000U,
          .pairing_write_fragment_bytes = 7U,
          .pairing_offer = InitiatorOffer(),
      },
      {}, {});
  Expect(runtime.Start(), "pre-TLS admission runtime starts");
  if (!runtime.running()) {
    return;
  }

  asio::io_context context;
  std::array<asio::ip::tcp::socket, 3> sockets = {
      asio::ip::tcp::socket(context),
      asio::ip::tcp::socket(context),
      asio::ip::tcp::socket(context),
  };
  for (asio::ip::tcp::socket& socket : sockets) {
    asio::error_code error;
    socket.connect(asio::ip::tcp::endpoint(asio::ip::address_v6::loopback(),
                                           runtime.listen_port()),
                   error);
    Expect(!error, "raw pre-TLS socket reaches the shared listener");
  }

  std::this_thread::sleep_for(100ms);
  std::array<std::uint8_t, 1> byte{};
  for (std::size_t index = 0; index < 2U; ++index) {
    asio::error_code error;
    sockets[index].non_blocking(true, error);
    static_cast<void>(sockets[index].read_some(asio::buffer(byte), error));
    Expect(error == asio::error::would_block || error == asio::error::try_again,
           "the first two source-bound handshakes retain their admission leases");
  }

  asio::error_code third_error;
  sockets[2].non_blocking(true, third_error);
  const auto deadline = std::chrono::steady_clock::now() + 750ms;
  do {
    static_cast<void>(sockets[2].read_some(asio::buffer(byte), third_error));
    if (third_error != asio::error::would_block &&
        third_error != asio::error::try_again) {
      break;
    }
    std::this_thread::sleep_for(10ms);
  } while (std::chrono::steady_clock::now() < deadline);
  Expect(third_error == asio::error::eof ||
             third_error == asio::error::connection_reset ||
             third_error == asio::error::operation_aborted,
         "the per-source admission ceiling rejects TLS work before ClientHello");
  runtime.Stop();
}

void TestProcessWidePreTlsAdmission() {
  IdentityFixture first_identity{DecodeArray<32>(kInitiatorSeed)};
  IdentityFixture second_identity{DecodeArray<32>(kResponderSeed)};
  Expect(
      first_identity.repository.Open().ok() && second_identity.repository.Open().ok(),
      "process-wide admission identities open");
  if (!first_identity.repository.ready() || !second_identity.repository.ready()) {
    return;
  }
  const session::AuthenticatedConnectionRuntimeConfig config{
      .listen_port = 0U,
      .tls_handshake_timeout_ms = 1'000U,
      .pairing_write_fragment_bytes = 7U,
      .pairing_offer = InitiatorOffer(),
  };
  session::AuthenticatedConnectionRuntime first(first_identity.repository, config, {},
                                                {});
  session::AuthenticatedConnectionRuntime second(second_identity.repository, config, {},
                                                 {});
  Expect(first.Start() && second.Start(),
         "two runtimes with distinct identities start");
  if (!first.running() || !second.running()) {
    first.Stop();
    second.Stop();
    return;
  }

  asio::io_context context;
  std::array<asio::ip::tcp::socket, 3> sockets = {
      asio::ip::tcp::socket(context),
      asio::ip::tcp::socket(context),
      asio::ip::tcp::socket(context),
  };
  const std::array<std::uint16_t, 3> ports{
      first.listen_port(),
      second.listen_port(),
      first.listen_port(),
  };
  auto process_admission =
      session::runtime_internal::PairingAdmissionBridge::ProcessScoped();
  for (std::size_t index = 0; index < sockets.size(); ++index) {
    asio::error_code error;
    sockets[index].connect(
        asio::ip::tcp::endpoint(asio::ip::address_v6::loopback(), ports[index]), error);
    Expect(!error, "raw socket reaches a process-scoped listener");
    if (index < 2U) {
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      while (process_admission.active_connections() < index + 1U &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
      }
      Expect(process_admission.active_connections() == index + 1U,
             "process-wide admission records each retained source lease");
    }
  }

  std::this_thread::sleep_for(100ms);
  std::array<std::uint8_t, 1> byte{};
  for (std::size_t index = 0; index < 2U; ++index) {
    asio::error_code error;
    sockets[index].non_blocking(true, error);
    static_cast<void>(sockets[index].read_some(asio::buffer(byte), error));
    Expect(error == asio::error::would_block || error == asio::error::try_again,
           "the process-wide limiter retains only its first two source leases");
  }
  asio::error_code third_error;
  sockets[2].non_blocking(true, third_error);
  const auto deadline = std::chrono::steady_clock::now() + 750ms;
  do {
    static_cast<void>(sockets[2].read_some(asio::buffer(byte), third_error));
    if (third_error != asio::error::would_block &&
        third_error != asio::error::try_again) {
      break;
    }
    std::this_thread::sleep_for(10ms);
  } while (std::chrono::steady_clock::now() < deadline);
  Expect(third_error == asio::error::eof ||
             third_error == asio::error::connection_reset ||
             third_error == asio::error::operation_aborted,
         "distinct-identity runtimes share the process-wide per-source ceiling");
  first.Stop();
  second.Stop();
  session::runtime_internal::PairingAdmissionBridge::Retire(process_admission);
}

void TestProcessAdmissionRateSurvivesRuntimeRecreation() {
  IdentityFixture identity_fixture{DecodeArray<32>(kInitiatorSeed)};
  Expect(identity_fixture.repository.Open().ok(),
         "persistent admission identity opens");
  if (!identity_fixture.repository.ready()) {
    return;
  }
  const session::AuthenticatedConnectionRuntimeConfig config{
      .listen_port = 0U,
      .tls_handshake_timeout_ms = 1'000U,
      .pairing_write_fragment_bytes = 7U,
      .pairing_offer = InitiatorOffer(),
  };
  for (std::size_t round = 0; round < 2U; ++round) {
    session::AuthenticatedConnectionRuntime runtime(identity_fixture.repository, config,
                                                    {}, {});
    Expect(runtime.Start(), "rate-limit consuming runtime starts");
    asio::io_context context;
    std::array<asio::ip::tcp::socket, 2> sockets{
        asio::ip::tcp::socket(context),
        asio::ip::tcp::socket(context),
    };
    for (asio::ip::tcp::socket& socket : sockets) {
      asio::error_code error;
      socket.connect(asio::ip::tcp::endpoint(asio::ip::address_v6::loopback(),
                                             runtime.listen_port()),
                     error);
      Expect(!error, "rate-limit consuming socket connects");
    }
    std::this_thread::sleep_for(100ms);
    runtime.Stop();
  }

  session::AuthenticatedConnectionRuntime throttled(identity_fixture.repository, config,
                                                    {}, {});
  Expect(throttled.Start(), "rate-limit verification runtime starts");
  asio::io_context context;
  asio::ip::tcp::socket socket(context);
  asio::error_code error;
  socket.connect(asio::ip::tcp::endpoint(asio::ip::address_v6::loopback(),
                                         throttled.listen_port()),
                 error);
  Expect(!error, "rate-limit verification socket reaches the listener");
  socket.non_blocking(true, error);
  std::array<std::uint8_t, 1> byte{};
  const auto deadline = std::chrono::steady_clock::now() + 750ms;
  do {
    static_cast<void>(socket.read_some(asio::buffer(byte), error));
    if (error != asio::error::would_block && error != asio::error::try_again) {
      break;
    }
    std::this_thread::sleep_for(10ms);
  } while (std::chrono::steady_clock::now() < deadline);
  Expect(error == asio::error::eof || error == asio::error::connection_reset ||
             error == asio::error::operation_aborted,
         "runtime recreation does not reset the process source token bucket");
  throttled.Stop();
}

void TestFrameAssemblyDeadline() {
  IdentityFixture client{DecodeArray<32>(kInitiatorSeed)};
  IdentityFixture server{DecodeArray<32>(kResponderSeed)};
  Expect(client.repository.Open().ok() && server.repository.Open().ok(),
         "frame-deadline identities open");
  if (!client.repository.ready() || !server.repository.ready()) {
    return;
  }

  PairingEvents pairing_events;
  session::AuthenticatedConnectionRuntime runtime(
      server.repository,
      session::AuthenticatedConnectionRuntimeConfig{
          .listen_port = 0U,
          .tls_handshake_timeout_ms = 1'000U,
          .pairing_write_fragment_bytes = session::kMaxPairingFrameSize,
          .pairing_offer = ResponderOffer(),
      },
      [&](const session::PairingRuntimeEvent& event) { pairing_events.Add(event); },
      {});
  Expect(runtime.Start() && runtime.OpenPairingWindow(20'000U),
         "frame-deadline runtime and pairing window start");

  auto client_context = tls::OpenSslTlsContext::Create(
      tls::TlsEndpointRole::kClient, client.repository, tls::kPairingAlpn);
  if (!client_context.ok() ||
      SSL_CTX_up_ref(client_context.value->native_handle()) != 1) {
    Expect(false, "frame-deadline TLS client configures");
    runtime.Stop();
    return;
  }
  asio::ssl::context asio_context(client_context.value->native_handle());
  asio::io_context io_context;
  asio::ssl::stream<asio::ip::tcp::socket> stream(io_context, asio_context);
  asio::error_code error;
  stream.lowest_layer().connect(
      asio::ip::tcp::endpoint(asio::ip::address_v6::loopback(), runtime.listen_port()),
      error);
  if (!error) {
    stream.handshake(asio::ssl::stream_base::client, error);
  }
  Expect(!error, "frame-deadline pairing TLS handshake completes");
  if (error) {
    runtime.Stop();
    return;
  }

  const session::Bytes hello = DecodeHex(pairing_vectors::kIHello);
  asio::write(stream, asio::buffer(hello), error);
  session::Bytes peer_frame;
  const bool read_hello = !error && ReadPairingFrame(stream, peer_frame);
  const session::Bytes selection = DecodeHex(pairing_vectors::kISelect);
  if (read_hello) {
    asio::write(stream, asio::buffer(selection), error);
  }
  const bool read_ack = !error && ReadPairingFrame(stream, peer_frame);
  const auto prompt =
      pairing_events.Wait([](const session::PairingRuntimeEvent& event) {
        return event.update.prompt.has_value();
      });
  Expect(read_hello && read_ack && prompt.has_value(),
         "manual peer reaches the decision phase");
  if (!read_hello || !read_ack || !prompt.has_value()) {
    runtime.Stop();
    return;
  }

  const std::array<std::uint8_t, session::kPairingFrameHeaderSize> partial_decision{
      'X', 'N', 'N',
      'P', 0U,  static_cast<std::uint8_t>(session::kPairingFrameHeaderSize),
      1U,  0U,  0U,
      4U,  0U,  0U,
      0U,  0U,  0U,
      3U,  0U,  0U,
      16U, 0U,
  };
  const auto started = std::chrono::steady_clock::now();
  asio::write(stream, asio::buffer(partial_decision), error);
  const auto terminal = pairing_events.Wait(
      [](const session::PairingRuntimeEvent& event) { return event.update.terminal; },
      12s);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  Expect(!error && terminal.has_value() &&
             terminal->update.error == session::PairingError::kTimeout &&
             elapsed >= 9s && elapsed < 12s,
         "partial frame closes at the ten-second assembly deadline");
  runtime.Stop();
}

void TestFrameSequencePrefixRejected() {
  IdentityFixture client{DecodeArray<32>(kInitiatorSeed)};
  IdentityFixture server{DecodeArray<32>(kResponderSeed)};
  Expect(client.repository.Open().ok() && server.repository.Open().ok(),
         "sequence-prefix identities open");
  if (!client.repository.ready() || !server.repository.ready()) {
    return;
  }

  PairingEvents pairing_events;
  session::AuthenticatedConnectionRuntime runtime(
      server.repository,
      session::AuthenticatedConnectionRuntimeConfig{
          .listen_port = 0U,
          .tls_handshake_timeout_ms = 1'000U,
          .pairing_write_fragment_bytes = session::kMaxPairingFrameSize,
          .pairing_offer = ResponderOffer(),
      },
      [&](const session::PairingRuntimeEvent& event) { pairing_events.Add(event); },
      {});
  Expect(runtime.Start() && runtime.OpenPairingWindow(5'000U),
         "sequence-prefix runtime and pairing window start");

  auto client_context = tls::OpenSslTlsContext::Create(
      tls::TlsEndpointRole::kClient, client.repository, tls::kPairingAlpn);
  if (!client_context.ok() ||
      SSL_CTX_up_ref(client_context.value->native_handle()) != 1) {
    Expect(false, "sequence-prefix TLS client configures");
    runtime.Stop();
    return;
  }
  asio::ssl::context asio_context(client_context.value->native_handle());
  asio::io_context io_context;
  asio::ssl::stream<asio::ip::tcp::socket> stream(io_context, asio_context);
  asio::error_code error;
  stream.lowest_layer().connect(
      asio::ip::tcp::endpoint(asio::ip::address_v6::loopback(), runtime.listen_port()),
      error);
  if (!error) {
    stream.handshake(asio::ssl::stream_base::client, error);
  }
  const session::Bytes hello = DecodeHex(pairing_vectors::kIHello);
  if (!error) {
    asio::write(stream, asio::buffer(hello), error);
  }
  session::Bytes peer_frame;
  const bool read_hello = !error && ReadPairingFrame(stream, peer_frame);
  const session::Bytes selection = DecodeHex(pairing_vectors::kISelect);
  if (read_hello) {
    asio::write(stream, asio::buffer(selection), error);
  }
  const bool read_ack = !error && ReadPairingFrame(stream, peer_frame);
  const auto prompt =
      pairing_events.Wait([](const session::PairingRuntimeEvent& event) {
        return event.update.prompt.has_value();
      });
  Expect(read_hello && read_ack && prompt.has_value(),
         "sequence-prefix peer reaches the decision phase");
  if (!read_hello || !read_ack || !prompt.has_value()) {
    runtime.Stop();
    return;
  }

  const std::array<std::uint8_t, session::kPairingFrameHeaderSize> wrong_sequence{
      'X', 'N', 'N',
      'P', 0U,  static_cast<std::uint8_t>(session::kPairingFrameHeaderSize),
      1U,  0U,  0U,
      4U,  0U,  0U,
      0U,  0U,  0U,
      4U,  0U,  0U,
      16U, 0U,
  };
  const auto started = std::chrono::steady_clock::now();
  asio::write(stream, asio::buffer(wrong_sequence), error);
  const auto terminal = pairing_events.Wait(
      [](const session::PairingRuntimeEvent& event) { return event.update.terminal; },
      2s);
  Expect(!error && terminal.has_value() &&
             terminal->update.error == session::PairingError::kSequenceViolation &&
             std::chrono::steady_clock::now() - started < 2s,
         "wrong sequence is rejected from the header without waiting for its body");
  runtime.Stop();
}

void TestTerminalFlushClosesAfterWrite() {
  IdentityFixture client{DecodeArray<32>(kInitiatorSeed)};
  IdentityFixture server{DecodeArray<32>(kResponderSeed)};
  Expect(client.repository.Open().ok() && server.repository.Open().ok(),
         "terminal-flush identities open");
  if (!client.repository.ready() || !server.repository.ready()) {
    return;
  }

  PairingEvents pairing_events;
  session::AuthenticatedConnectionRuntime runtime(
      server.repository,
      session::AuthenticatedConnectionRuntimeConfig{
          .listen_port = 0U,
          .tls_handshake_timeout_ms = 1'000U,
          .pairing_write_fragment_bytes = session::kMaxPairingFrameSize,
          .pairing_offer = ResponderOffer(),
      },
      [&](const session::PairingRuntimeEvent& event) { pairing_events.Add(event); },
      {});
  Expect(runtime.Start() && runtime.OpenPairingWindow(5'000U),
         "terminal-flush runtime and pairing window start");

  auto client_context = tls::OpenSslTlsContext::Create(
      tls::TlsEndpointRole::kClient, client.repository, tls::kPairingAlpn);
  if (!client_context.ok() ||
      SSL_CTX_up_ref(client_context.value->native_handle()) != 1) {
    Expect(false, "terminal-flush TLS client configures");
    runtime.Stop();
    return;
  }
  asio::ssl::context asio_context(client_context.value->native_handle());
  asio::io_context io_context;
  asio::ssl::stream<asio::ip::tcp::socket> stream(io_context, asio_context);
  asio::error_code error;
  stream.lowest_layer().connect(
      asio::ip::tcp::endpoint(asio::ip::address_v6::loopback(), runtime.listen_port()),
      error);
  if (!error) {
    stream.handshake(asio::ssl::stream_base::client, error);
  }
  const session::Bytes hello = DecodeHex(pairing_vectors::kIHello);
  if (!error) {
    asio::write(stream, asio::buffer(hello), error);
  }
  session::Bytes peer_frame;
  const bool read_hello = !error && ReadPairingFrame(stream, peer_frame);
  const session::Bytes selection = DecodeHex(pairing_vectors::kISelect);
  if (read_hello) {
    asio::write(stream, asio::buffer(selection), error);
  }
  const bool read_ack = !error && ReadPairingFrame(stream, peer_frame);
  const auto prompt =
      pairing_events.Wait([](const session::PairingRuntimeEvent& event) {
        return event.update.prompt.has_value();
      });
  Expect(read_hello && read_ack && prompt.has_value(),
         "terminal-flush manual peer reaches the decision phase");
  if (!read_hello || !read_ack || !prompt.has_value()) {
    runtime.Stop();
    return;
  }

  const auto started = std::chrono::steady_clock::now();
  Expect(runtime.Decide(prompt->attempt, tls::ConfirmationDecision::kReject),
         "local rejection queues its authenticated terminal frame");
  const bool read_rejection = ReadPairingFrame(stream, peer_frame);
  stream.next_layer().non_blocking(true, error);
  std::array<std::uint8_t, 1> byte{};
  const auto deadline = started + 750ms;
  do {
    static_cast<void>(stream.next_layer().read_some(asio::buffer(byte), error));
    if (error != asio::error::would_block && error != asio::error::try_again) {
      break;
    }
    std::this_thread::sleep_for(10ms);
  } while (std::chrono::steady_clock::now() < deadline);
  Expect(read_rejection &&
             (error == asio::error::eof || error == asio::error::connection_reset ||
              error == asio::error::operation_aborted) &&
             std::chrono::steady_clock::now() - started < 750ms,
         "terminal transport closes immediately when its final write completes");
  runtime.Stop();
}

void TestNoncanonicalEndpointsRejected() {
  IdentityFixture identity_fixture{DecodeArray<32>(kInitiatorSeed)};
  Expect(identity_fixture.repository.Open().ok(), "endpoint identity opens");
  if (!identity_fixture.repository.ready()) {
    return;
  }
  session::AuthenticatedConnectionRuntime runtime(
      identity_fixture.repository,
      session::AuthenticatedConnectionRuntimeConfig{
          .listen_port = 0U,
          .tls_handshake_timeout_ms = 1'000U,
          .pairing_write_fragment_bytes = 7U,
          .pairing_offer = InitiatorOffer(),
      },
      {}, {});
  Expect(runtime.Start(), "endpoint validation runtime starts");
  if (!runtime.running()) {
    return;
  }

  session::NetworkEndpoint v4 =
      session::NetworkEndpoint::V4({127U, 0U, 0U, 1U}, runtime.listen_port());
  v4.address[4] = 1U;
  Expect(!runtime.StartPairing(v4, 1U, "peer"),
         "IPv4 trailing address bytes are rejected");
  v4.address[4] = 0U;
  v4.scope_id = 1U;
  Expect(!runtime.StartPairing(v4, 2U, "peer"), "IPv4 scope identifiers are rejected");

  session::NetworkEndpoint v6 = Loopback(runtime.listen_port());
  v6.scope_id = 1U;
  Expect(!runtime.StartPairing(v6, 3U, "peer"),
         "non-link-local IPv6 scope identifiers are rejected");
  runtime.Stop();
}

void TestUnknownEstablishedPeerIsNotDispatched() {
  IdentityFixture attacker{DecodeArray<32>(kInitiatorSeed)};
  IdentityFixture server{DecodeArray<32>(kResponderSeed)};
  Expect(attacker.repository.Open().ok() && server.repository.Open().ok(),
         "unknown-pin runtime identities open");
  if (!attacker.repository.ready() || !server.repository.ready()) {
    return;
  }
  const auto server_pin = attacker.repository.CommitPeer(identity::PeerCommit{
      .public_key = *server.repository.root_public_key(),
      .security_profile = tls::kSecurityProfileV1,
      .display_label = "server",
  });
  Expect(server_pin.ok(), "attacker knows the server exact pin");
  if (!server_pin.ok()) {
    return;
  }

  std::atomic_size_t server_dispatches{};
  std::mutex mutex;
  std::condition_variable condition;
  bool client_completed = false;
  session::AuthenticatedConnectionRuntime server_runtime(
      server.repository,
      session::AuthenticatedConnectionRuntimeConfig{
          .listen_port = 0U,
          .tls_handshake_timeout_ms = 1'000U,
          .pairing_write_fragment_bytes = 7U,
          .pairing_offer = ResponderOffer(),
      },
      {}, [&](const session::EstablishedRuntimeEvent& event) {
        if (event.connection != nullptr) {
          ++server_dispatches;
        }
      });
  session::AuthenticatedConnectionRuntime attacker_runtime(
      attacker.repository,
      session::AuthenticatedConnectionRuntimeConfig{
          .listen_port = 0U,
          .tls_handshake_timeout_ms = 1'000U,
          .pairing_write_fragment_bytes = 7U,
          .pairing_offer = InitiatorOffer(),
      },
      {}, [&](const session::EstablishedRuntimeEvent&) {
        const std::scoped_lock lock(mutex);
        client_completed = true;
        condition.notify_all();
      });
  Expect(server_runtime.Start() && attacker_runtime.Start(),
         "unknown-pin runtimes start");
  Expect(attacker_runtime.OpenEstablished(Loopback(server_runtime.listen_port()),
                                          server_pin.value(), 55U),
         "unknown peer opens a proof-safe established handshake");
  {
    std::unique_lock lock(mutex);
    Expect(condition.wait_for(lock, 5s, [&] { return client_completed; }),
           "outbound established attempt completes");
  }
  std::this_thread::sleep_for(100ms);
  Expect(server_dispatches.load() == 0U,
         "unknown inbound pin is rejected before session dispatch");
  attacker_runtime.Stop();
  server_runtime.Stop();
}

void TestPairingAlpnRejectsTransferFrame() {
  IdentityFixture client{DecodeArray<32>(kInitiatorSeed)};
  IdentityFixture server{DecodeArray<32>(kResponderSeed)};
  Expect(client.repository.Open().ok() && server.repository.Open().ok(),
         "ALPN-confusion runtime identities open");
  if (!client.repository.ready() || !server.repository.ready()) {
    return;
  }

  PairingEvents pairing_events;
  std::atomic_size_t established_dispatches{};
  session::AuthenticatedConnectionRuntime runtime(
      server.repository,
      session::AuthenticatedConnectionRuntimeConfig{
          .listen_port = 0U,
          .tls_handshake_timeout_ms = 1'000U,
          .pairing_write_fragment_bytes = 7U,
          .pairing_offer = ResponderOffer(),
      },
      [&](const session::PairingRuntimeEvent& event) { pairing_events.Add(event); },
      [&](const session::EstablishedRuntimeEvent& event) {
        if (event.connection != nullptr) {
          ++established_dispatches;
        }
      });
  Expect(runtime.Start() && runtime.OpenPairingWindow(2'000U),
         "ALPN-confusion runtime and pairing window start");

  auto client_context = tls::OpenSslTlsContext::Create(
      tls::TlsEndpointRole::kClient, client.repository, tls::kPairingAlpn);
  Expect(client_context.ok(), "manual pairing TLS client configures");
  if (!client_context.ok() ||
      SSL_CTX_up_ref(client_context.value->native_handle()) != 1) {
    runtime.Stop();
    return;
  }
  asio::ssl::context asio_context(client_context.value->native_handle());
  asio::io_context io_context;
  asio::ssl::stream<asio::ip::tcp::socket> stream(io_context, asio_context);
  asio::error_code error;
  stream.lowest_layer().connect(
      asio::ip::tcp::endpoint(asio::ip::address_v6::loopback(), runtime.listen_port()),
      error);
  if (!error) {
    stream.handshake(asio::ssl::stream_base::client, error);
  }
  Expect(!error, "manual pairing TLS handshake completes");
  if (!error) {
    const std::array<std::uint8_t, session::kPairingFrameHeaderSize> transfer_header{
        'X', 'N', 'N',
        'T', 0U,  static_cast<std::uint8_t>(session::kPairingFrameHeaderSize),
        1U,  0U,  0U,
        1U,  0U,  0U,
        0U,  0U,  0U,
        1U,  0U,  0U,
        16U, 0U,
    };
    for (const std::uint8_t byte : transfer_header) {
      asio::write(stream, asio::buffer(&byte, 1U), error);
      if (error) {
        break;
      }
    }
    Expect(!error, "invalid transfer header with a large declaration is fragmented");
  }
  const auto terminal = pairing_events.Wait(
      [](const session::PairingRuntimeEvent& event) { return event.update.terminal; });
  Expect(terminal.has_value() &&
             terminal->update.error == session::PairingError::kMalformed,
         "pairing ALPN rejects a transfer frame as malformed");
  Expect(established_dispatches.load() == 0U,
         "pairing ALPN never dispatches transfer traffic");
  runtime.Stop();
}

void TestConcurrentStop() {
  IdentityFixture identity_fixture{DecodeArray<32>(kResponderSeed)};
  Expect(identity_fixture.repository.Open().ok(), "concurrent-stop identity opens");
  if (!identity_fixture.repository.ready()) {
    return;
  }

  session::AuthenticatedConnectionRuntime runtime(
      identity_fixture.repository,
      session::AuthenticatedConnectionRuntimeConfig{
          .listen_port = 0U,
          .tls_handshake_timeout_ms = 1'000U,
          .pairing_write_fragment_bytes = 7U,
          .pairing_offer = InitiatorOffer(),
      },
      {}, {});
  Expect(runtime.Start(), "concurrent-stop runtime starts");
  if (!runtime.running()) {
    return;
  }

  std::atomic_bool begin{};
  std::vector<std::thread> stoppers;
  for (std::size_t index = 0; index < 4U; ++index) {
    stoppers.emplace_back([&] {
      while (!begin.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      runtime.Stop();
    });
  }
  begin.store(true, std::memory_order_release);
  for (std::thread& stopper : stoppers) {
    stopper.join();
  }
  Expect(!runtime.running(), "concurrent stop calls share one completed barrier");
}

void TestRepeatedStartStop() {
  IdentityFixture identity_fixture{DecodeArray<32>(kResponderSeed)};
  Expect(identity_fixture.repository.Open().ok(), "repeated-stop identity opens");
  if (!identity_fixture.repository.ready()) {
    return;
  }
  for (std::size_t iteration = 0; iteration < 8U; ++iteration) {
    auto runtime = std::make_unique<session::AuthenticatedConnectionRuntime>(
        identity_fixture.repository,
        session::AuthenticatedConnectionRuntimeConfig{
            .listen_port = 0U,
            .tls_handshake_timeout_ms = 100U,
            .pairing_write_fragment_bytes = 7U,
            .pairing_offer = ResponderOffer(),
        },
        session::AuthenticatedConnectionRuntime::PairingHandler{},
        session::AuthenticatedConnectionRuntime::EstablishedHandler{});
    Expect(runtime->Start(), "repeated-stop runtime starts");
    runtime->Stop();
    runtime.reset();
  }
}

void TestCommandResultPrecedesBlockingCallback() {
  RuntimePair pair;
  std::mutex mutex;
  std::condition_variable condition;
  bool block_next = false;
  bool callback_entered = false;
  bool release_callback = false;
  pair.left_hook_ = [&](const session::PairingRuntimeEvent& event) {
    std::unique_lock lock(mutex);
    if (!block_next || event.update.outbound_frame.empty() || event.update.terminal) {
      return;
    }
    block_next = false;
    callback_entered = true;
    condition.notify_all();
    condition.wait(lock, [&] { return release_callback; });
  };
  if (!pair.Start() || !pair.Begin()) {
    return;
  }
  const auto left_prompt = pair.LeftPrompt();
  const auto right_prompt = pair.RightPrompt();
  Expect(left_prompt.has_value() && right_prompt.has_value(),
         "blocking-callback case reaches both prompts");
  if (!left_prompt.has_value() || !right_prompt.has_value()) {
    return;
  }

  {
    const std::scoped_lock lock(mutex);
    block_next = true;
  }
  bool command_result = false;
  bool command_returned = false;
  std::thread command([&] {
    command_result =
        pair.left_->Decide(left_prompt->attempt, tls::ConfirmationDecision::kConfirm);
    const std::scoped_lock lock(mutex);
    command_returned = true;
    condition.notify_all();
  });
  bool returned_before_release = false;
  {
    std::unique_lock lock(mutex);
    Expect(condition.wait_for(lock, 2s, [&] { return callback_entered; }),
           "decision event reaches the intentionally blocking callback");
    returned_before_release =
        condition.wait_for(lock, 2s, [&] { return command_returned; });
    release_callback = true;
    condition.notify_all();
  }
  command.join();
  Expect(returned_before_release && command_result,
         "command result is exact before its user callback is delivered");
  pair.Stop();
}

void TestBlockingCallbackDoesNotBlockNetworkTimers() {
  IdentityFixture identity_fixture{DecodeArray<32>(kResponderSeed)};
  Expect(identity_fixture.repository.Open().ok(), "isolated-callback identity opens");
  if (!identity_fixture.repository.ready()) {
    return;
  }
  std::mutex mutex;
  std::condition_variable condition;
  bool callback_entered = false;
  bool release_callback = false;
  session::AuthenticatedConnectionRuntime runtime(
      identity_fixture.repository,
      session::AuthenticatedConnectionRuntimeConfig{
          .listen_port = 0U,
          .tls_handshake_timeout_ms = 200U,
          .pairing_write_fragment_bytes = 7U,
          .pairing_offer = InitiatorOffer(),
      },
      [&](const session::PairingRuntimeEvent&) {
        std::unique_lock lock(mutex);
        callback_entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release_callback; });
      },
      {});
  Expect(runtime.Start() && runtime.OpenPairingWindow(2'000U),
         "isolated-callback runtime and window start");
  Expect(runtime.StartPairing(Loopback(runtime.listen_port()), 9U, "self"),
         "self pairing emits an isolated callback");
  {
    std::unique_lock lock(mutex);
    Expect(condition.wait_for(lock, 3s, [&] { return callback_entered; }),
           "isolated callback reaches its blocking section");
  }

  asio::io_context context;
  asio::ip::tcp::socket socket(context);
  asio::error_code error;
  socket.connect(
      asio::ip::tcp::endpoint(asio::ip::address_v6::loopback(), runtime.listen_port()),
      error);
  Expect(!error, "raw socket connects while the user callback is blocked");
  socket.non_blocking(true, error);
  std::array<std::uint8_t, 1> byte{};
  const auto deadline = std::chrono::steady_clock::now() + 750ms;
  do {
    static_cast<void>(socket.read_some(asio::buffer(byte), error));
    if (error != asio::error::would_block && error != asio::error::try_again) {
      break;
    }
    std::this_thread::sleep_for(10ms);
  } while (std::chrono::steady_clock::now() < deadline);
  Expect(error == asio::error::eof || error == asio::error::connection_reset ||
             error == asio::error::operation_aborted,
         "TLS timeout progresses on the network executor while callback blocks");

  std::atomic_bool stop_returned{};
  std::thread stopper([&] {
    runtime.Stop();
    stop_returned.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(100ms);
  Expect(!stop_returned.load(std::memory_order_acquire),
         "stop waits for the callback already in flight");
  {
    const std::scoped_lock lock(mutex);
    release_callback = true;
    condition.notify_all();
  }
  stopper.join();
  Expect(stop_returned.load(std::memory_order_acquire) && !runtime.running(),
         "stop completes after the isolated callback returns");
}

void TestNetworkExecutorStopWaitsForCallback() {
  RuntimePair pair;
  if (!pair.Start() || !pair.Begin()) {
    return;
  }
  const auto left_prompt = pair.LeftPrompt();
  const auto right_prompt = pair.RightPrompt();
  if (!left_prompt.has_value() || !right_prompt.has_value()) {
    Expect(false, "network-stop case reaches both pairing prompts");
    return;
  }
  Expect(
      pair.left_->Decide(left_prompt->attempt, tls::ConfirmationDecision::kConfirm) &&
          pair.right_->Decide(right_prompt->attempt,
                              tls::ConfirmationDecision::kConfirm),
      "network-stop case confirms both pairing prompts");
  const auto left_terminal = pair.LeftTerminal();
  const auto right_terminal = pair.RightTerminal();
  if (!left_terminal.has_value() || !right_terminal.has_value() ||
      !left_terminal->update.paired_peer.has_value()) {
    Expect(false, "network-stop case completes pairing");
    return;
  }
  Expect(pair.left_->OpenEstablished(Loopback(pair.right_->listen_port()),
                                     *left_terminal->update.paired_peer, 117U),
         "network-stop established connection starts");
  const auto outbound =
      pair.left_established_.Wait([](const session::EstablishedRuntimeEvent& event) {
        return event.request_id == 117U && event.connection != nullptr;
      });
  const auto inbound =
      pair.right_established_.Wait([](const session::EstablishedRuntimeEvent& event) {
        return event.inbound && event.connection != nullptr;
      });
  if (!outbound.has_value() || !inbound.has_value()) {
    Expect(false, "network-stop established endpoints dispatch");
    return;
  }

  std::mutex mutex;
  std::condition_variable condition;
  bool callback_entered = false;
  bool release_callback = false;
  bool network_gate_entered = false;
  bool release_network_gate = false;
  bool stop_returned = false;
  bool command_started = false;
  bool command_returned = false;
  bool callback_stop_returned = false;
  bool command_result = true;
  std::chrono::steady_clock::duration command_elapsed{};
  bool queued_handler_invoked = false;
  Expect(outbound->connection->ReadSome(
             1U,
             [&](const session::ConnectionIoError, session::Bytes) {
               std::unique_lock lock(mutex);
               callback_entered = true;
               condition.notify_all();
               condition.wait(lock, [&] { return release_callback; });
               command_started = true;
               condition.notify_all();
               lock.unlock();
               const auto started = std::chrono::steady_clock::now();
               const bool result = pair.left_->OpenPairingWindow(1'000U);
               const auto elapsed = std::chrono::steady_clock::now() - started;
               pair.left_->Stop();
               lock.lock();
               command_result = result;
               command_elapsed = elapsed;
               command_returned = true;
               callback_stop_returned = true;
               condition.notify_all();
             }),
         "network-stop read callback is accepted");
  Expect(inbound->connection->Write(session::Bytes{0x5aU},
                                    [](const session::ConnectionIoError) {}),
         "network-stop peer writes one byte");
  {
    std::unique_lock lock(mutex);
    Expect(condition.wait_for(lock, 5s, [&] { return callback_entered; }),
           "network-stop callback reaches its blocking section");
  }
  Expect(outbound->connection->DispatchChannel([&](session::EstablishedTlsChannel&) {
    std::unique_lock lock(mutex);
    network_gate_entered = true;
    condition.notify_all();
    condition.wait(lock, [&] { return release_network_gate; });
  }),
         "network executor enters its ordering gate");
  {
    std::unique_lock lock(mutex);
    Expect(condition.wait_for(lock, 5s, [&] { return network_gate_entered; }),
           "network executor blocks before the stop command");
  }
  Expect(outbound->connection->DispatchChannel([&](session::EstablishedTlsChannel&) {
    pair.left_->Stop();
    const std::scoped_lock lock(mutex);
    stop_returned = true;
    condition.notify_all();
  }),
         "network executor schedules reentrant stop");
  auto queued_capture = std::make_shared<std::uint8_t>(0x5aU);
  const std::weak_ptr<std::uint8_t> queued_capture_weak = queued_capture;
  Expect(outbound->connection->DispatchChannel(
             [&, queued_capture](session::EstablishedTlsChannel&) {
               queued_handler_invoked = true;
             }),
         "handler queues behind the network-executor stop");
  queued_capture.reset();
  {
    std::unique_lock lock(mutex);
    release_callback = true;
    condition.notify_all();
    Expect(condition.wait_for(lock, 5s, [&] { return command_started; }),
           "callback starts a runtime command queued behind the network gate");
    lock.unlock();
    std::this_thread::sleep_for(20ms);
    lock.lock();
    release_network_gate = true;
    condition.notify_all();
    Expect(condition.wait_for(lock, 1s,
                              [&] {
                                return command_returned && callback_stop_returned &&
                                       stop_returned;
                              }),
           "worker and callback stop calls complete without circular waiting");
  }
  pair.Stop();
  Expect(!command_result && command_elapsed < 1s,
         "cancelled runtime command returns false promptly");
  Expect(!queued_handler_invoked && queued_capture_weak.expired(),
         "stop discards queued handlers and releases their captures");
}

void TestCallbackFirstAndWorkerStop() {
  RuntimePair pair;
  if (!pair.Start() || !pair.Begin()) {
    return;
  }
  const auto left_prompt = pair.LeftPrompt();
  const auto right_prompt = pair.RightPrompt();
  if (!left_prompt.has_value() || !right_prompt.has_value()) {
    Expect(false, "callback-first stop reaches both pairing prompts");
    return;
  }
  Expect(
      pair.left_->Decide(left_prompt->attempt, tls::ConfirmationDecision::kConfirm) &&
          pair.right_->Decide(right_prompt->attempt,
                              tls::ConfirmationDecision::kConfirm),
      "callback-first stop confirms both pairing prompts");
  const auto left_terminal = pair.LeftTerminal();
  const auto right_terminal = pair.RightTerminal();
  if (!left_terminal.has_value() || !right_terminal.has_value() ||
      !left_terminal->update.paired_peer.has_value()) {
    Expect(false, "callback-first stop completes pairing");
    return;
  }
  Expect(pair.left_->OpenEstablished(Loopback(pair.right_->listen_port()),
                                     *left_terminal->update.paired_peer, 118U),
         "callback-first stop establishes a connection");
  const auto outbound =
      pair.left_established_.Wait([](const session::EstablishedRuntimeEvent& event) {
        return event.request_id == 118U && event.connection != nullptr;
      });
  const auto inbound =
      pair.right_established_.Wait([](const session::EstablishedRuntimeEvent& event) {
        return event.inbound && event.connection != nullptr;
      });
  if (!outbound.has_value() || !inbound.has_value()) {
    Expect(false, "callback-first stop dispatches both endpoints");
    return;
  }

  std::mutex mutex;
  std::condition_variable condition;
  bool callback_entered = false;
  bool callback_stop_started = false;
  bool callback_stop_returned = false;
  bool network_entered = false;
  bool release_callback_stop = false;
  bool release_worker_stop = false;
  bool worker_stop_returned = false;
  Expect(outbound->connection->ReadSome(
             1U,
             [&](const session::ConnectionIoError, session::Bytes) {
               std::unique_lock lock(mutex);
               callback_entered = true;
               condition.notify_all();
               condition.wait(lock, [&] { return release_callback_stop; });
               callback_stop_started = true;
               condition.notify_all();
               lock.unlock();
               pair.left_->Stop();
               lock.lock();
               callback_stop_returned = true;
               condition.notify_all();
             }),
         "callback-first stop accepts a read callback");
  Expect(inbound->connection->Write(session::Bytes{0x5bU},
                                    [](const session::ConnectionIoError) {}),
         "callback-first stop peer writes one byte");
  {
    std::unique_lock lock(mutex);
    Expect(condition.wait_for(lock, 5s, [&] { return callback_entered; }),
           "callback-first stop callback is active");
  }
  Expect(outbound->connection->DispatchChannel([&](session::EstablishedTlsChannel&) {
    std::unique_lock lock(mutex);
    network_entered = true;
    condition.notify_all();
    condition.wait(lock, [&] { return release_worker_stop; });
    lock.unlock();
    pair.left_->Stop();
    lock.lock();
    worker_stop_returned = true;
    condition.notify_all();
  }),
         "callback-first stop enters a network handler");
  {
    std::unique_lock lock(mutex);
    Expect(condition.wait_for(lock, 5s, [&] { return network_entered; }),
           "callback and worker handlers are both active");
    release_callback_stop = true;
    condition.notify_all();
    Expect(condition.wait_for(lock, 5s, [&] { return callback_stop_started; }),
           "callback initiates stop before the worker");
  }
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (pair.left_->running() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  Expect(!pair.left_->running(), "callback stop publishes shutdown before worker stop");
  {
    std::unique_lock lock(mutex);
    release_worker_stop = true;
    condition.notify_all();
    Expect(
        condition.wait_for(
            lock, 1s, [&] { return callback_stop_returned && worker_stop_returned; }),
        "callback-first and worker stop calls complete without circular waiting");
  }
  pair.Stop();
}

void TestStopFromCallback() {
  IdentityFixture identity_fixture{DecodeArray<32>(kResponderSeed)};
  Expect(identity_fixture.repository.Open().ok(), "callback-stop identity opens");
  if (!identity_fixture.repository.ready()) {
    return;
  }

  std::mutex mutex;
  std::condition_variable condition;
  std::atomic_bool stop_called{};
  std::atomic_size_t callback_count{};
  session::AuthenticatedConnectionRuntime* runtime_pointer = nullptr;
  session::AuthenticatedConnectionRuntime runtime(
      identity_fixture.repository,
      session::AuthenticatedConnectionRuntimeConfig{
          .listen_port = 0U,
          .tls_handshake_timeout_ms = 1'000U,
          .pairing_write_fragment_bytes = 7U,
          .pairing_offer = InitiatorOffer(),
      },
      [&](const session::PairingRuntimeEvent&) {
        ++callback_count;
        if (!stop_called.exchange(true) && runtime_pointer != nullptr) {
          runtime_pointer->Stop();
          const std::scoped_lock lock(mutex);
          condition.notify_all();
        }
      },
      {});
  runtime_pointer = &runtime;
  Expect(runtime.Start() && runtime.OpenPairingWindow(2'000U),
         "callback-stop runtime and window start");
  Expect(runtime.StartPairing(Loopback(runtime.listen_port()), 7U, "self"),
         "self-pairing reaches a runtime callback");
  {
    std::unique_lock lock(mutex);
    Expect(condition.wait_for(lock, 5s, [&] { return stop_called.load(); }),
           "stop is callable from the runtime callback");
  }
  runtime.Stop();
  const std::size_t callbacks_after_stop = callback_count.load();
  std::this_thread::sleep_for(100ms);
  Expect(callback_count.load() == callbacks_after_stop && !runtime.running(),
         "stop barrier prevents callbacks after shutdown");
}

}  // namespace

int main() {
  RunTest("confirmed pairing and established I/O",
          TestConfirmedPairingAndEstablishedIo);
  RunTest("authenticated rejection", TestAuthenticatedRejection);
  RunTest("pairing deadline", TestPairingDeadline);
  RunTest("partial handshake timeout", TestPartialHandshakeTimeout);
  RunTest("handshake hard limit", TestHandshakeHardLimit);
  RunTest("start failure rollback", TestStartFailureRollsBackListener);
  RunTest("pairing ALPN window", TestPairingAlpnRequiresOpenWindow);
  RunTest("pre-TLS admission per source", TestPreTlsAdmissionPerSource);
  RunTest("process-wide pre-TLS admission", TestProcessWidePreTlsAdmission);
  RunTest("persistent process admission rate",
          TestProcessAdmissionRateSurvivesRuntimeRecreation);
  RunTest("frame assembly deadline", TestFrameAssemblyDeadline);
  RunTest("frame sequence prefix rejection", TestFrameSequencePrefixRejected);
  RunTest("terminal flush closes after write", TestTerminalFlushClosesAfterWrite);
  RunTest("noncanonical endpoint rejection", TestNoncanonicalEndpointsRejected);
  RunTest("unknown established peer rejection",
          TestUnknownEstablishedPeerIsNotDispatched);
  RunTest("pairing ALPN transfer rejection", TestPairingAlpnRejectsTransferFrame);
  RunTest("concurrent stop", TestConcurrentStop);
  RunTest("repeated start-stop", TestRepeatedStartStop);
  RunTest("command result before callback", TestCommandResultPrecedesBlockingCallback);
  RunTest("blocking callback network isolation",
          TestBlockingCallbackDoesNotBlockNetworkTimers);
  RunTest("network stop callback barrier", TestNetworkExecutorStopWaitsForCallback);
  RunTest("callback-first worker stop", TestCallbackFirstAndWorkerStop);
  RunTest("stop from callback", TestStopFromCallback);

  if (failures != 0) {
    std::cerr << failures << " authenticated runtime test(s) failed\n";
    return 1;
  }
  std::cout << "Authenticated loopback connection runtime passed\n";
  return 0;
}
