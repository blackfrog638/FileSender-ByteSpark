#include <openssl/ssl.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

#include "pairing_vectors.hpp"
#include "test_support.hpp"
#include "xnn_transfer/core/session/session.hpp"

namespace {

using namespace session_test;

constexpr std::string_view kInitiatorSeed =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
constexpr std::string_view kResponderSeed =
    "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
constexpr std::string_view kReplacementSeed =
    "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7";
constexpr std::array<std::uint8_t, 14> kEstablishedAlpn = {
    'x', 'n', 'n', '-', 't', 'r', 'a', 'n', 's', 'f', 'e', 'r', '/', '1',
};
constexpr std::array<std::uint8_t, 22> kPairingAlpn = {
    'x', 'n', 'n', '-', 't', 'r', 'a', 'n', 's', 'f', 'e',
    'r', '-', 'p', 'a', 'i', 'r', 'i', 'n', 'g', '/', '1',
};

using SslPointer = std::unique_ptr<SSL, decltype(&SSL_free)>;

int failures = 0;

void Expect(const bool condition, const std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

struct Handshake {
  SslPointer client{nullptr, &SSL_free};
  SslPointer server{nullptr, &SSL_free};
};

bool CompleteHandshake(SSL* const client, SSL* const server) {
  bool client_done = false;
  bool server_done = false;
  for (std::size_t iteration = 0; iteration < 1'024; ++iteration) {
    for (const auto [connection, complete] : std::array<std::pair<SSL*, bool*>, 2>{
             std::pair{client, &client_done},
             std::pair{server, &server_done},
         }) {
      if (*complete) {
        continue;
      }
      const int result = SSL_do_handshake(connection);
      if (result == 1) {
        *complete = true;
        continue;
      }
      const int error = SSL_get_error(connection, result);
      if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
        return false;
      }
    }
    if (client_done && server_done) {
      return true;
    }
  }
  return false;
}

std::optional<Handshake> Connect(SSL_CTX* const client_context,
                                 SSL_CTX* const server_context) {
  Handshake handshake{
      .client = SslPointer(SSL_new(client_context), &SSL_free),
      .server = SslPointer(SSL_new(server_context), &SSL_free),
  };
  if (!handshake.client || !handshake.server) {
    return std::nullopt;
  }
  BIO* client_bio = nullptr;
  BIO* server_bio = nullptr;
  if (BIO_new_bio_pair(&client_bio, 0, &server_bio, 0) != 1) {
    return std::nullopt;
  }
  SSL_set_bio(handshake.client.get(), client_bio, client_bio);
  SSL_set_bio(handshake.server.get(), server_bio, server_bio);
  SSL_set_connect_state(handshake.client.get());
  SSL_set_accept_state(handshake.server.get());
  return CompleteHandshake(handshake.client.get(), handshake.server.get())
             ? std::optional<Handshake>(std::move(handshake))
             : std::nullopt;
}

bool ReconnectClient(Handshake& handshake, SSL_CTX* const server_context) {
  if (SSL_clear(handshake.client.get()) != 1) {
    return false;
  }
  handshake.server.reset(SSL_new(server_context));
  if (!handshake.server) {
    return false;
  }
  BIO* client_bio = nullptr;
  BIO* server_bio = nullptr;
  if (BIO_new_bio_pair(&client_bio, 0, &server_bio, 0) != 1) {
    return false;
  }
  SSL_set_bio(handshake.client.get(), client_bio, client_bio);
  SSL_set_bio(handshake.server.get(), server_bio, server_bio);
  SSL_set_connect_state(handshake.client.get());
  SSL_set_accept_state(handshake.server.get());
  return CompleteHandshake(handshake.client.get(), handshake.server.get());
}

struct TlsFixture {
  IdentityFixture client{DecodeArray<32>(kInitiatorSeed)};
  IdentityFixture server{DecodeArray<32>(kResponderSeed)};
  std::optional<identity::DeviceId> server_device_id{};
  std::optional<identity::DeviceId> client_device_id{};
  std::optional<tls::OpenSslTlsContext> client_context{};
  std::optional<tls::OpenSslTlsContext> server_context{};
  std::optional<Handshake> handshake{};

  bool Open(const std::uint16_t stored_profile = tls::kSecurityProfileV1) {
    Expect(client.repository.Open().ok(), "client identity opens");
    Expect(server.repository.Open().ok(), "server identity opens");
    const auto committed = client.repository.CommitPeer(identity::PeerCommit{
        .public_key = *server.repository.root_public_key(),
        .security_profile = stored_profile,
        .display_label = "server",
    });
    Expect(committed.ok(), "server pin commits to client repository");
    if (!committed.ok()) {
      return false;
    }
    server_device_id = committed.value();
    const auto reverse_commit = server.repository.CommitPeer(identity::PeerCommit{
        .public_key = *client.repository.root_public_key(),
        .security_profile = stored_profile,
        .display_label = "client",
    });
    Expect(reverse_commit.ok(), "client pin commits to server repository");
    if (!reverse_commit.ok()) {
      return false;
    }
    client_device_id = reverse_commit.value();

    auto client_result = tls::OpenSslTlsContext::Create(
        tls::TlsEndpointRole::kClient, client.repository, kEstablishedAlpn);
    auto server_result = tls::OpenSslTlsContext::Create(
        tls::TlsEndpointRole::kServer, server.repository, kEstablishedAlpn);
    Expect(client_result.ok() && server_result.ok(),
           "established TLS contexts configure");
    if (!client_result.ok() || !server_result.ok()) {
      return false;
    }
    client_context.emplace(std::move(*client_result.value));
    server_context.emplace(std::move(*server_result.value));
    handshake =
        Connect(client_context->native_handle(), server_context->native_handle());
    Expect(handshake.has_value(), "fresh mutual TLS handshake completes");
    return handshake.has_value();
  }

  std::unique_ptr<session::EstablishedTlsChannel> Authorize() {
    auto result = session::EstablishedTlsChannel::Create(
        *client_context, handshake->client.get(), client.repository, *server_device_id);
    Expect(result.ok(), "exact active pin creates established capability");
    if (!result.ok()) {
      return nullptr;
    }
    return std::move(*result.value);
  }

  std::unique_ptr<session::EstablishedTlsChannel> AuthorizeServer() {
    auto result = session::EstablishedTlsChannel::Create(
        *server_context, handshake->server.get(), server.repository, *client_device_id);
    Expect(result.ok(), "reverse exact pin creates established capability");
    if (!result.ok()) {
      return nullptr;
    }
    return std::move(*result.value);
  }
};

std::optional<tls::NormalizedNegotiation> BuildNegotiation() {
  const auto decoded = tls::DecodeNormalizedNegotiation(
      DecodeHex(pairing_vectors::kNormalizedNegotiation));
  Expect(decoded.ok(), "registered normalized negotiation decodes");
  return decoded.ok() ? std::optional<tls::NormalizedNegotiation>(*decoded.value)
                      : std::nullopt;
}

std::optional<tls::TransportContext> BuildTransportContext(TlsFixture& fixture) {
  const auto initiator_key =
      tls::ValidateEd25519PublicKey(*fixture.client.repository.root_public_key());
  const auto responder_key =
      tls::ValidateEd25519PublicKey(*fixture.server.repository.root_public_key());
  const auto negotiation = BuildNegotiation();
  if (!initiator_key.ok() || !responder_key.ok() || !negotiation.has_value()) {
    Expect(false, "transport context inputs validate");
    return std::nullopt;
  }
  const auto context = tls::BuildTransportContext(tls::TransportContextInput{
      .initiator_key = *initiator_key.value,
      .responder_key = *responder_key.value,
      .initiator_nonce =
          tls::Nonce256{DecodeArray<32>(pairing_vectors::kInitiatorTransportNonce)},
      .responder_nonce =
          tls::Nonce256{DecodeArray<32>(pairing_vectors::kResponderTransportNonce)},
      .negotiation = *negotiation,
      .raw_negotiation_transcript =
          DecodeHex(pairing_vectors::kRawNegotiationTranscript),
      .session_id =
          tls::TransferSessionId{DecodeArray<16>(pairing_vectors::kTransportSessionId)},
  });
  Expect(context.ok(), "registered transport context builds");
  return context.ok() ? std::optional<tls::TransportContext>(*context.value)
                      : std::nullopt;
}

std::optional<tls::PairingContext> BuildPairingContext(IdentityFixture& initiator,
                                                       IdentityFixture& responder) {
  const auto initiator_key =
      tls::ValidateEd25519PublicKey(*initiator.repository.root_public_key());
  const auto responder_key =
      tls::ValidateEd25519PublicKey(*responder.repository.root_public_key());
  const auto negotiation = BuildNegotiation();
  if (!initiator_key.ok() || !responder_key.ok() || !negotiation.has_value()) {
    return std::nullopt;
  }
  const auto context = tls::BuildPairingContext(tls::PairingContextInput{
      .initiator_nonce =
          tls::Nonce256{DecodeArray<32>(pairing_vectors::kInitiatorTransportNonce)},
      .responder_nonce =
          tls::Nonce256{DecodeArray<32>(pairing_vectors::kResponderTransportNonce)},
      .initiator_key = *initiator_key.value,
      .responder_key = *responder_key.value,
      .negotiation = *negotiation,
  });
  return context.ok() ? std::optional<tls::PairingContext>(*context.value)
                      : std::nullopt;
}

std::unique_ptr<session::EstablishedTlsChannel> AuthorizeBound(TlsFixture& fixture) {
  std::unique_ptr<session::EstablishedTlsChannel> client = fixture.Authorize();
  std::unique_ptr<session::EstablishedTlsChannel> server = fixture.AuthorizeServer();
  const auto context = BuildTransportContext(fixture);
  if (!client || !server || !context.has_value()) {
    return nullptr;
  }
  const auto client_finished =
      client->BeginTransportBinding(*context, tls::Role::kInitiator);
  const auto server_finished =
      server->BeginTransportBinding(*context, tls::Role::kResponder);
  Expect(client_finished.ok() && server_finished.ok(),
         "both exact-pin channels prepare transport finished values");
  if (!client_finished.ok() || !server_finished.ok()) {
    return nullptr;
  }
  Expect(!client->transport_bound() && !server->transport_bound(),
         "finished generation alone does not authorize transport");
  Expect(client->LocalTransportFinishedWritten() == tls::SecurityError::kNone &&
             server->LocalTransportFinishedWritten() == tls::SecurityError::kNone,
         "both local transport finished writes complete");
  Expect(client->VerifyPeerTransportFinished(server_finished.value->message,
                                             server_finished.value->authenticator) ==
                 tls::SecurityError::kNone &&
             server->VerifyPeerTransportFinished(
                 client_finished.value->message,
                 client_finished.value->authenticator) == tls::SecurityError::kNone,
         "both peer transport finished values verify");
  Expect(client->transport_bound() && server->transport_bound(),
         "both channels become transport-bound");
  return client;
}

void TestExactPinAndRevocation() {
  TlsFixture fixture;
  if (!fixture.Open()) {
    return;
  }
  CounterEntropy entropy;
  session::SessionAuthority authority(fixture.client.repository, entropy);
  const session::AuthorizationResult provisional =
      authority.Activate(fixture.Authorize());
  Expect(!provisional.ok() &&
             provisional.error == session::AuthorizationError::kUnauthenticated,
         "exact pin alone remains provisional before transport finished");

  std::unique_ptr<session::EstablishedTlsChannel> stale_before_binding =
      fixture.Authorize();
  const auto stale_context = BuildTransportContext(fixture);
  if (!stale_before_binding || !stale_context.has_value()) {
    return;
  }
  Expect(ReconnectClient(*fixture.handshake, fixture.server_context->native_handle()),
         "established SSL object reconnects before transport binding");
  const auto stale_begin = stale_before_binding->BeginTransportBinding(
      *stale_context, tls::Role::kInitiator);
  Expect(!stale_begin.ok() &&
             stale_begin.error == tls::SecurityError::kHandshakeIncomplete,
         "old exact-pin capability cannot bind a reused SSL handshake");

  std::unique_ptr<session::EstablishedTlsChannel> transient = AuthorizeBound(fixture);
  if (!transient) {
    return;
  }
  const session::AuthorizationResult transient_active =
      authority.Activate(std::move(transient));
  Expect(transient_active.ok() && authority.IsAuthorized(*transient_active.handle),
         "transport-bound exact-pin channel becomes locally authorized");
  Expect(authority.Deactivate(*transient_active.handle) ==
                 session::AuthorizationError::kNone &&
             !authority.IsAuthorized(*transient_active.handle) &&
             authority.active_sessions() == 0,
         "normal disconnect releases authorization without revoking trust");
  const identity::PeerRecord* const still_active =
      fixture.client.repository.FindPeer(*fixture.server_device_id);
  Expect(still_active != nullptr &&
             still_active->trust_state == identity::TrustState::kActive,
         "normal disconnect preserves durable peer trust");

  std::unique_ptr<session::EstablishedTlsChannel> reused_before_activate =
      AuthorizeBound(fixture);
  if (!reused_before_activate) {
    return;
  }
  Expect(ReconnectClient(*fixture.handshake, fixture.server_context->native_handle()),
         "established SSL object reconnects before authority activation");
  const session::AuthorizationResult rejected_reuse =
      authority.Activate(std::move(reused_before_activate));
  Expect(!rejected_reuse.ok() &&
             rejected_reuse.error == session::AuthorizationError::kUnauthenticated,
         "Activate rejects transport capability reused before activation");

  std::unique_ptr<session::EstablishedTlsChannel> stale_channel =
      AuthorizeBound(fixture);
  if (!stale_channel) {
    return;
  }
  const session::AuthorizationResult stale_active =
      authority.Activate(std::move(stale_channel));
  Expect(stale_active.ok(), "pre-reuse transport-bound channel activates");
  Expect(ReconnectClient(*fixture.handshake, fixture.server_context->native_handle()),
         "established SSL object reconnects to the same pinned identity");
  Expect(!authority.IsAuthorized(*stale_active.handle),
         "same-key SSL reuse invalidates the old transport authority");
  authority.InvalidateStale();
  Expect(authority.active_sessions() == 0,
         "stale reused transport is removed from authority");

  std::unique_ptr<session::EstablishedTlsChannel> channel = AuthorizeBound(fixture);
  if (!channel) {
    return;
  }
  const session::AuthorizationResult active = authority.Activate(std::move(channel));
  Expect(active.ok() && authority.IsAuthorized(*active.handle),
         "second transport-bound channel activates");
  Expect(authority.Revoke(*active.handle) == session::AuthorizationError::kNone,
         "authenticated session may revoke its peer");
  Expect(!authority.IsAuthorized(*active.handle) && authority.active_sessions() == 0,
         "revocation invalidates live and cached authorization");
  const identity::PeerRecord* const revoked =
      fixture.client.repository.FindPeer(*fixture.server_device_id);
  Expect(revoked != nullptr && revoked->trust_state == identity::TrustState::kRevoked,
         "revocation is durable before success returns");

  const auto reconnect = session::EstablishedTlsChannel::Create(
      *fixture.client_context, fixture.handshake->client.get(),
      fixture.client.repository, *fixture.server_device_id);
  Expect(!reconnect.ok() && reconnect.error == tls::SecurityError::kPinMismatch,
         "revoked pin cannot create a reconnect capability");
}

void TestWrongPinAndSecurityFloor() {
  TlsFixture expected;
  IdentityFixture attacker{DecodeArray<32>(kReplacementSeed)};
  if (!expected.Open()) {
    return;
  }
  Expect(attacker.repository.Open().ok(), "attacker identity opens");
  auto attacker_context = tls::OpenSslTlsContext::Create(
      tls::TlsEndpointRole::kServer, attacker.repository, kEstablishedAlpn);
  Expect(attacker_context.ok(), "attacker TLS context configures");
  if (!attacker_context.ok()) {
    return;
  }
  std::optional<Handshake> wrong_peer =
      Connect(expected.client_context->native_handle(),
              attacker_context.value->native_handle());
  Expect(wrong_peer.has_value(), "wrong peer proves possession of its own key");
  if (wrong_peer) {
    const auto wrong_pin = session::EstablishedTlsChannel::Create(
        *expected.client_context, wrong_peer->client.get(), expected.client.repository,
        *expected.server_device_id);
    Expect(!wrong_pin.ok() && wrong_pin.error == tls::SecurityError::kPinMismatch,
           "valid certificate metadata cannot override exact pin mismatch");
  }

  TlsFixture higher_floor;
  if (higher_floor.Open(2)) {
    const auto below_floor = session::EstablishedTlsChannel::Create(
        *higher_floor.client_context, higher_floor.handshake->client.get(),
        higher_floor.client.repository, *higher_floor.server_device_id);
    Expect(!below_floor.ok() &&
               below_floor.error == tls::SecurityError::kUnsupportedProfile,
           "reconnect below stored security floor fails closed");
  }
}

void TestRotationResetAndShutdownInvalidate() {
  TlsFixture rotated;
  IdentityFixture replacement{DecodeArray<32>(kReplacementSeed)};
  if (!rotated.Open()) {
    return;
  }
  Expect(replacement.repository.Open().ok(), "replacement identity opens");
  std::unique_ptr<session::EstablishedTlsChannel> rotated_channel =
      AuthorizeBound(rotated);
  if (!rotated_channel) {
    return;
  }
  CounterEntropy rotated_entropy;
  session::SessionAuthority rotated_authority(rotated.client.repository,
                                              rotated_entropy);
  const session::AuthorizationResult before_rotation =
      rotated_authority.Activate(std::move(rotated_channel));
  Expect(before_rotation.ok(), "pre-rotation session activates");
  const auto rotation = rotated.client.repository.RotatePeer(
      *rotated.server_device_id, *replacement.repository.root_public_key(), 1);
  Expect(rotation.ok(), "repository atomically replaces the active pin");
  Expect(!rotated_authority.IsAuthorized(*before_rotation.handle),
         "old-key authorization is stale immediately after rotation");
  rotated_authority.InvalidateStale();
  Expect(rotated_authority.active_sessions() == 0,
         "rotation purges cached old-key sessions");

  TlsFixture reset;
  if (!reset.Open()) {
    return;
  }
  std::unique_ptr<session::EstablishedTlsChannel> reset_channel = AuthorizeBound(reset);
  if (!reset_channel) {
    return;
  }
  CounterEntropy reset_entropy;
  session::SessionAuthority reset_authority(reset.client.repository, reset_entropy);
  const session::AuthorizationResult before_reset =
      reset_authority.Activate(std::move(reset_channel));
  Expect(before_reset.ok(), "pre-reset session activates");
  const auto reset_result = reset.client.repository.Reset();
  Expect(reset_result.ok(), "local identity reset commits");
  Expect(!reset_authority.IsAuthorized(*before_reset.handle),
         "identity generation change invalidates all old sessions");
  reset_authority.InvalidateStale();
  Expect(reset_authority.active_sessions() == 0,
         "identity reset purges cached authorization");

  TlsFixture shutdown;
  if (!shutdown.Open()) {
    return;
  }
  std::unique_ptr<session::EstablishedTlsChannel> shutdown_channel =
      AuthorizeBound(shutdown);
  if (!shutdown_channel) {
    return;
  }
  CounterEntropy shutdown_entropy;
  session::SessionAuthority shutdown_authority(shutdown.client.repository,
                                               shutdown_entropy);
  const session::AuthorizationResult before_shutdown =
      shutdown_authority.Activate(std::move(shutdown_channel));
  Expect(before_shutdown.ok(), "pre-shutdown session activates");
  std::atomic<bool> start{false};
  std::atomic<bool> stop{false};
  std::array<std::thread, 4> readers;
  for (std::thread& reader : readers) {
    reader = std::thread([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      while (!stop.load(std::memory_order_acquire)) {
        static_cast<void>(shutdown_authority.IsAuthorized(*before_shutdown.handle));
        static_cast<void>(shutdown_authority.active_sessions());
      }
    });
  }
  start.store(true, std::memory_order_release);
  shutdown_authority.Shutdown();
  stop.store(true, std::memory_order_release);
  for (std::thread& reader : readers) {
    reader.join();
  }
  Expect(!shutdown_authority.IsAuthorized(*before_shutdown.handle) &&
             shutdown_authority.active_sessions() == 0,
         "shutdown is a barrier for concurrent authorization readers");
}

void TestPairingChannelRejectsSameKeyReuse() {
  IdentityFixture initiator{DecodeArray<32>(kInitiatorSeed)};
  IdentityFixture responder{DecodeArray<32>(kResponderSeed)};
  Expect(initiator.repository.Open().ok() && responder.repository.Open().ok(),
         "same-key reuse identities open");
  auto client_context = tls::OpenSslTlsContext::Create(
      tls::TlsEndpointRole::kClient, initiator.repository, kPairingAlpn);
  auto server_context = tls::OpenSslTlsContext::Create(
      tls::TlsEndpointRole::kServer, responder.repository, kPairingAlpn);
  if (!client_context.ok() || !server_context.ok()) {
    Expect(false, "same-key reuse contexts configure");
    return;
  }
  std::optional<Handshake> handshake = Connect(client_context.value->native_handle(),
                                               server_context.value->native_handle());
  if (!handshake.has_value()) {
    Expect(false, "same-key reuse initial handshake completes");
    return;
  }
  const auto context = BuildPairingContext(initiator, responder);
  auto before_first_export = session::OpenSslPairingChannel::Create(
      *client_context.value, handshake->client.get());
  if (!before_first_export.ok() || !context.has_value()) {
    Expect(false, "same-key reuse pairing capability initializes");
    return;
  }
  Expect(ReconnectClient(*handshake, server_context.value->native_handle()),
         "same SSL object reconnects before the first exporter");
  const auto stale_first = before_first_export.value->get()->ExportPairing(*context);
  Expect(!stale_first.ok(),
         "old pairing channel rejects first exporter after same-key reconnect");

  auto between_exporters = session::OpenSslPairingChannel::Create(
      *client_context.value, handshake->client.get());
  if (!between_exporters.ok()) {
    Expect(false, "fresh same-key pairing capability initializes");
    return;
  }
  const auto first = between_exporters.value->get()->ExportPairing(*context);
  Expect(first.ok(), "initial pairing exporter succeeds");
  Expect(ReconnectClient(*handshake, server_context.value->native_handle()),
         "same SSL object reconnects between pairing exporters");
  const auto stale = between_exporters.value->get()->ExportConfirmation(*context);
  Expect(!stale.ok(),
         "old pairing channel rejects exporter use after same-key reconnect");
}

void TestLivePairingTlsExportersAndModeIsolation() {
  IdentityFixture initiator_identity{DecodeArray<32>(kInitiatorSeed)};
  IdentityFixture responder_identity{DecodeArray<32>(kResponderSeed)};
  Expect(initiator_identity.repository.Open().ok(),
         "live pairing initiator identity opens");
  Expect(responder_identity.repository.Open().ok(),
         "live pairing responder identity opens");
  auto initiator_context = tls::OpenSslTlsContext::Create(
      tls::TlsEndpointRole::kClient, initiator_identity.repository, kPairingAlpn);
  auto responder_context = tls::OpenSslTlsContext::Create(
      tls::TlsEndpointRole::kServer, responder_identity.repository, kPairingAlpn);
  Expect(initiator_context.ok() && responder_context.ok(),
         "registered pairing ALPN contexts configure");
  if (!initiator_context.ok() || !responder_context.ok()) {
    return;
  }
  std::optional<Handshake> handshake =
      Connect(initiator_context.value->native_handle(),
              responder_context.value->native_handle());
  Expect(handshake.has_value(), "live pairing TLS handshake completes");
  if (!handshake) {
    return;
  }
  auto initiator_channel = session::OpenSslPairingChannel::Create(
      *initiator_context.value, handshake->client.get());
  auto responder_channel = session::OpenSslPairingChannel::Create(
      *responder_context.value, handshake->server.get());
  Expect(initiator_channel.ok() && responder_channel.ok(),
         "pairing ALPN yields one-use unpinned pairing channels");
  if (!initiator_channel.ok() || !responder_channel.ok()) {
    return;
  }

  session::PairingReplayCache initiator_replay;
  session::PairingReplayCache responder_replay;
  FixedAttemptEntropy initiator_entropy(0xa0U, 0x00U);
  FixedAttemptEntropy responder_entropy(0xb0U, 0x20U);
  std::unique_ptr<session::PairingAttempt> initiator;
  std::unique_ptr<session::PairingAttempt> responder;
  auto initiator_admission = AdmitForTest(
      *initiator_identity.repository.root_public_key(),
      *responder_identity.repository.root_public_key(), tls::Role::kInitiator, 0xa0U);
  auto responder_admission = AdmitForTest(
      *responder_identity.repository.root_public_key(),
      *initiator_identity.repository.root_public_key(), tls::Role::kResponder, 0xb0U);
  const auto initiator_created = session::PairingAttempt::Create(
      initiator_identity.repository, std::move(*initiator_channel.value),
      initiator_entropy, std::move(initiator_admission), initiator_replay,
      session::PairingAttemptOptions{
          .offer = InitiatorOffer(),
          .peer_display_label = "responder",
      },
      initiator);
  const auto responder_created = session::PairingAttempt::Create(
      responder_identity.repository, std::move(*responder_channel.value),
      responder_entropy, std::move(responder_admission), responder_replay,
      session::PairingAttemptOptions{
          .offer = ResponderOffer(),
          .peer_display_label = "initiator",
      },
      responder);
  Expect(!initiator_created.terminal && !responder_created.terminal,
         "live TLS channels create both pairing endpoints");
  if (!initiator || !responder) {
    return;
  }

  const auto initiator_start = initiator->Start(1'000);
  const auto responder_start = responder->Start(1'000);
  static_cast<void>(responder->ReceiveFrame(initiator_start.outbound_frame, 1'001));
  const auto selected = initiator->ReceiveFrame(responder_start.outbound_frame, 1'001);
  const auto acknowledged = responder->ReceiveFrame(selected.outbound_frame, 1'002);
  const auto responder_prompt =
      responder->LocalSelectionAckWritten(responder->handle(), 1'003);
  const auto initiator_prompt =
      initiator->ReceiveFrame(acknowledged.outbound_frame, 1'003);
  Expect(initiator_prompt.prompt.has_value() && responder_prompt.prompt.has_value() &&
             initiator_prompt.prompt->sas_word_indices ==
                 responder_prompt.prompt->sas_word_indices,
         "live TLS pairing exporters derive identical SAS");
  if (!initiator_prompt.prompt || !responder_prompt.prompt) {
    return;
  }
  const session::AttemptHandle initiator_handle = initiator_prompt.prompt->handle;
  const session::AttemptHandle responder_handle = responder_prompt.prompt->handle;
  const auto initiator_decision =
      initiator->Decide(initiator_handle, tls::ConfirmationDecision::kConfirm, 1'003);
  const auto responder_decision =
      responder->Decide(responder_handle, tls::ConfirmationDecision::kConfirm, 1'003);
  static_cast<void>(initiator->LocalDecisionWritten(initiator_handle, 1'004));
  static_cast<void>(responder->LocalDecisionWritten(responder_handle, 1'004));
  const auto responder_paired =
      responder->ReceiveFrame(initiator_decision.outbound_frame, 1'005);
  const auto initiator_paired =
      initiator->ReceiveFrame(responder_decision.outbound_frame, 1'005);
  Expect(initiator_paired.state == session::PairingState::kPairedLocal &&
             responder_paired.state == session::PairingState::kPairedLocal &&
             initiator_paired.paired_peer.has_value(),
         "live TLS confirmation exporters authenticate both decisions");
  if (!initiator_paired.paired_peer) {
    return;
  }

  const auto wrong_mode = session::EstablishedTlsChannel::Create(
      *initiator_context.value, handshake->client.get(), initiator_identity.repository,
      *initiator_paired.paired_peer);
  Expect(!wrong_mode.ok() && wrong_mode.error == tls::SecurityError::kAlpnMismatch,
         "pairing ALPN can never be promoted to established authority");
}

}  // namespace

int main() {
  TestExactPinAndRevocation();
  TestWrongPinAndSecurityFloor();
  TestRotationResetAndShutdownInvalidate();
  TestPairingChannelRejectsSameKeyReuse();
  TestLivePairingTlsExportersAndModeIsolation();

  if (failures != 0) {
    std::cerr << failures << " session authority test(s) failed\n";
    return 1;
  }
  std::cout << "Exact-pin session authorization invalidation passed\n";
  return 0;
}
