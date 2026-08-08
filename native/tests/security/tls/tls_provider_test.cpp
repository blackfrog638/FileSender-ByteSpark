#include "xnn_transfer/core/security/tls/tls_provider.hpp"

#include <openssl/bio.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "xnn_transfer/core/security/identity/crypto.hpp"
#include "xnn_transfer/core/security/identity/identity_repository.hpp"
#include "xnn_transfer/core/security/identity/protected_store.hpp"

namespace {

namespace identity = xnn_transfer::core::security::identity;
namespace tls = xnn_transfer::core::security::tls;

using SslPointer = std::unique_ptr<SSL, decltype(&SSL_free)>;
using SessionPointer = std::unique_ptr<SSL_SESSION, decltype(&SSL_SESSION_free)>;
using ContextPointer = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;

static_assert(!std::is_copy_constructible_v<tls::VerifiedTlsConnection>);
static_assert(!std::is_copy_assignable_v<tls::VerifiedTlsConnection>);
static_assert(std::is_nothrow_move_constructible_v<tls::VerifiedTlsConnection>);
static_assert(std::is_nothrow_move_assignable_v<tls::VerifiedTlsConnection>);

int failures = 0;

void Expect(const bool condition, const std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

class MemoryProtectedStore final : public identity::ProtectedStore {
 public:
  identity::Result<std::vector<identity::ProtectedItemMetadata>> Enumerate() override {
    std::vector<identity::ProtectedItemMetadata> metadata;
    metadata.reserve(items_.size());
    for (const auto& [id, item] : items_) {
      metadata.push_back({id, item.revision});
    }
    return identity::Result<std::vector<identity::ProtectedItemMetadata>>::Success(
        std::move(metadata));
  }

  identity::Result<std::optional<identity::ProtectedItem>> Get(
      const identity::ProtectedItemId& item_id) override {
    const auto iterator = items_.find(item_id);
    if (iterator == items_.end()) {
      return identity::Result<std::optional<identity::ProtectedItem>>::Success(
          std::nullopt);
    }
    return identity::Result<std::optional<identity::ProtectedItem>>::Success(
        identity::ProtectedItem(
            iterator->second.revision,
            identity::SecretBuffer(iterator->second.payload.bytes())));
  }

  identity::Result<void> CompareExchangePut(
      const identity::ProtectedItemId& item_id,
      const std::optional<std::uint64_t> expected_revision,
      identity::ProtectedItem replacement) override {
    auto iterator = items_.find(item_id);
    const bool creates = !expected_revision.has_value() && iterator == items_.end() &&
                         replacement.revision == 1;
    const bool updates =
        expected_revision.has_value() && iterator != items_.end() &&
        iterator->second.revision == *expected_revision &&
        *expected_revision != (std::numeric_limits<std::uint64_t>::max)() &&
        replacement.revision == *expected_revision + 1;
    if (!creates && !updates) {
      return identity::Result<void>::Failure(identity::ErrorCode::kRevisionConflict);
    }
    if (iterator == items_.end()) {
      items_.emplace(item_id, std::move(replacement));
    } else {
      iterator->second = std::move(replacement);
    }
    return identity::Result<void>::Success();
  }

  identity::Result<void> CompareExchangeDelete(
      const identity::ProtectedItemId& item_id,
      const std::uint64_t expected_revision) override {
    const auto iterator = items_.find(item_id);
    if (iterator == items_.end() || iterator->second.revision != expected_revision) {
      return identity::Result<void>::Failure(identity::ErrorCode::kRevisionConflict);
    }
    items_.erase(iterator);
    return identity::Result<void>::Success();
  }

 private:
  std::map<identity::ProtectedItemId, identity::ProtectedItem> items_;
};

struct IdentityFixture {
  MemoryProtectedStore store;
  identity::OpenSslIdentityCrypto crypto;
  tls::OpenSslPeerPublicKeyValidator validator;
  identity::IdentityRepository repository{store, crypto, validator};
};

struct Handshake {
  SslPointer client{nullptr, &SSL_free};
  SslPointer server{nullptr, &SSL_free};
};

bool AdvanceHandshake(SSL* const connection, bool& complete) {
  if (complete) {
    return true;
  }
  const int result = SSL_do_handshake(connection);
  if (result == 1) {
    complete = true;
    return true;
  }
  const int error = SSL_get_error(connection, result);
  return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE;
}

bool CompleteHandshake(SSL* const client, SSL* const server) {
  BIO* client_bio = nullptr;
  BIO* server_bio = nullptr;
  if (BIO_new_bio_pair(&client_bio, 0, &server_bio, 0) != 1) {
    return false;
  }
  SSL_set_bio(client, client_bio, client_bio);
  SSL_set_bio(server, server_bio, server_bio);
  SSL_set_connect_state(client);
  SSL_set_accept_state(server);

  bool client_complete = false;
  bool server_complete = false;
  for (std::size_t iteration = 0;
       iteration < 1024 && (!client_complete || !server_complete); ++iteration) {
    if (!AdvanceHandshake(client, client_complete) ||
        !AdvanceHandshake(server, server_complete)) {
      return false;
    }
  }
  return client_complete && server_complete;
}

std::optional<Handshake> Connect(SSL_CTX* const client_context,
                                 SSL_CTX* const server_context,
                                 SSL_SESSION* const requested_session = nullptr) {
  Handshake handshake{
      .client = SslPointer(SSL_new(client_context), &SSL_free),
      .server = SslPointer(SSL_new(server_context), &SSL_free),
  };
  if (!handshake.client || !handshake.server) {
    return std::nullopt;
  }
  if (requested_session != nullptr &&
      SSL_set_session(handshake.client.get(), requested_session) != 1) {
    return std::nullopt;
  }
  if (!CompleteHandshake(handshake.client.get(), handshake.server.get())) {
    return std::nullopt;
  }
  return handshake;
}

bool ReconnectClient(Handshake& handshake, SSL_CTX* const server_context) {
  if (SSL_clear(handshake.client.get()) != 1) {
    return false;
  }
  handshake.server.reset(SSL_new(server_context));
  return handshake.server != nullptr &&
         CompleteHandshake(handshake.client.get(), handshake.server.get());
}

void TestConfiguration(const tls::OpenSslTlsContext& client,
                       const tls::OpenSslTlsContext& server) {
  for (SSL_CTX* context : {client.native_handle(), server.native_handle()}) {
    Expect(SSL_CTX_get_min_proto_version(context) == TLS1_3_VERSION,
           "minimum protocol is TLS 1.3");
    Expect(SSL_CTX_get_max_proto_version(context) == TLS1_3_VERSION,
           "maximum protocol is TLS 1.3");
    Expect(SSL_CTX_get_session_cache_mode(context) == SSL_SESS_CACHE_OFF,
           "session cache is disabled");
    Expect(SSL_CTX_get_num_tickets(context) == 0, "session tickets are disabled");
    Expect(SSL_CTX_get_max_early_data(context) == 0, "early data is disabled");
    Expect((SSL_CTX_get_options(context) & SSL_OP_NO_TICKET) != 0,
           "ticket option remains disabled");
  }

  SslPointer early_data_connection(SSL_new(client.native_handle()), &SSL_free);
  std::size_t written = 0;
  constexpr std::array<std::uint8_t, 1> kByte = {0x5aU};
  Expect(early_data_connection != nullptr, "early-data probe allocates SSL");
  if (early_data_connection) {
    SSL_set_connect_state(early_data_connection.get());
    Expect(SSL_write_early_data(early_data_connection.get(), kByte.data(), kByte.size(),
                                &written) != 1 &&
               written == 0,
           "early data cannot be emitted");
  }
}

void TestHandshakeAndExporters(const tls::OpenSslTlsContext& client,
                               const tls::OpenSslTlsContext& server,
                               const identity::PublicKey& client_key,
                               const identity::PublicKey& server_key) {
  auto handshake = Connect(client.native_handle(), server.native_handle());
  Expect(handshake.has_value(), "matching profile completes mutual handshake");
  if (!handshake.has_value()) {
    return;
  }

  auto client_peer = client.VerifyPeer(handshake->client.get(), server_key);
  auto server_peer = server.VerifyPeer(handshake->server.get(), client_key);
  Expect(client_peer.ok(), "client verifies exact responder pin and profile");
  Expect(server_peer.ok(), "server verifies exact initiator pin and profile");
  if (!client_peer.ok() || !server_peer.ok()) {
    return;
  }
  Expect(client_peer.value->peer_public_key().bytes() == server_key,
         "client receives exact responder key bytes");
  Expect(server_peer.value->peer_public_key().bytes() == client_key,
         "server receives exact initiator key bytes");

  identity::PublicKey wrong_pin = server_key;
  wrong_pin[0] ^= 0x80U;
  const auto wrong_pin_result = client.VerifyPeer(handshake->client.get(), wrong_pin);
  Expect(!wrong_pin_result.ok() &&
             wrong_pin_result.error == tls::SecurityError::kPinMismatch,
         "wrong exact pin fails closed");

  tls::PairingExporterInput pairing_input{};
  tls::ConfirmationExporterInput confirmation_input{};
  tls::TransportExporterInput transport_input{};
  for (std::size_t index = 0; index < tls::kSha256Size; ++index) {
    pairing_input.context[index] = static_cast<std::uint8_t>(index);
    confirmation_input.context[index] = static_cast<std::uint8_t>(index);
    transport_input.context[index] = static_cast<std::uint8_t>(index);
  }

  tls::VerifiedTlsConnection moved_client_peer = std::move(*client_peer.value);
  const auto moved_from_export =
      client.ExportKeyingMaterial(*client_peer.value, pairing_input);
  Expect(!moved_from_export.ok() &&
             moved_from_export.error == tls::SecurityError::kExporterFailure,
         "moving verified capability invalidates its source");

  auto client_pairing = client.ExportKeyingMaterial(moved_client_peer, pairing_input);
  auto server_pairing = server.ExportKeyingMaterial(*server_peer.value, pairing_input);
  auto client_confirmation =
      client.ExportKeyingMaterial(moved_client_peer, confirmation_input);
  auto client_transport =
      client.ExportKeyingMaterial(moved_client_peer, transport_input);
  Expect(client_pairing.ok() && server_pairing.ok() && client_confirmation.ok() &&
             client_transport.ok(),
         "verified connection derives all typed exporters");
  if (client_pairing.ok() && server_pairing.ok()) {
    Expect(std::equal(client_pairing.value->bytes().begin(),
                      client_pairing.value->bytes().end(),
                      server_pairing.value->bytes().begin(),
                      server_pairing.value->bytes().end()),
           "client and server pairing exporters agree");
  }
  if (client_pairing.ok() && client_confirmation.ok() && client_transport.ok()) {
    Expect(!std::equal(client_pairing.value->bytes().begin(),
                       client_pairing.value->bytes().end(),
                       client_confirmation.value->bytes().begin(),
                       client_confirmation.value->bytes().end()),
           "pairing and confirmation labels are separated");
    Expect(!std::equal(client_pairing.value->bytes().begin(),
                       client_pairing.value->bytes().end(),
                       client_transport.value->bytes().begin(),
                       client_transport.value->bytes().end()),
           "pairing and transport labels are separated");
  }

  SessionPointer session(SSL_get1_session(handshake->client.get()), &SSL_SESSION_free);
  Expect(session != nullptr, "completed handshake exposes a session object");
  if (session) {
    auto second =
        Connect(client.native_handle(), server.native_handle(), session.get());
    Expect(second.has_value(), "attempted resumption falls back to fresh handshake");
    if (second.has_value()) {
      Expect(SSL_session_reused(second->client.get()) == 0 &&
                 SSL_session_reused(second->server.get()) == 0,
             "attempted resumption never reuses session");
      Expect(client.VerifyPeer(second->client.get(), server_key).ok(),
             "fresh replacement handshake verifies");
    }
  }
}

void TestStaleCapabilityRejectedAfterConnectionReuse(
    const tls::OpenSslTlsContext& client, const tls::OpenSslTlsContext& server,
    const identity::PublicKey& original_server_key) {
  IdentityFixture replacement_server_identity;
  Expect(replacement_server_identity.repository.Open().ok(),
         "replacement server protected identity opens");
  if (!replacement_server_identity.repository.ready()) {
    return;
  }

  auto replacement_server = tls::OpenSslTlsContext::Create(
      tls::TlsEndpointRole::kServer, replacement_server_identity.repository,
      client.alpn_protocol());
  Expect(replacement_server.ok(), "replacement server TLS context configures");
  if (!replacement_server.ok()) {
    return;
  }
  const identity::PublicKey& replacement_server_key =
      *replacement_server_identity.repository.root_public_key();
  Expect(replacement_server_key != original_server_key,
         "replacement server has a distinct identity");

  auto handshake = Connect(client.native_handle(), server.native_handle());
  Expect(handshake.has_value(), "initial stale-capability handshake completes");
  if (!handshake.has_value()) {
    return;
  }
  auto stale_capability =
      client.VerifyPeer(handshake->client.get(), original_server_key);
  Expect(stale_capability.ok(), "initial peer capability verifies");
  if (!stale_capability.ok()) {
    return;
  }

  Expect(ReconnectClient(*handshake, replacement_server.value->native_handle()),
         "same SSL object reconnects to replacement peer");
  auto fresh_capability =
      client.VerifyPeer(handshake->client.get(), replacement_server_key);
  Expect(fresh_capability.ok(), "reused SSL object verifies replacement peer");
  if (!fresh_capability.ok()) {
    return;
  }

  tls::PairingExporterInput input{};
  const auto fresh_export = client.ExportKeyingMaterial(*fresh_capability.value, input);
  Expect(fresh_export.ok(), "fresh capability exports after SSL object reuse");

  const auto stale_export = client.ExportKeyingMaterial(*stale_capability.value, input);
  Expect(
      !stale_export.ok() && stale_export.error == tls::SecurityError::kExporterFailure,
      "stale capability rejects a different current peer key");
}

void TestAlpnAndCertificateFailures(identity::IdentityRepository& client_identity,
                                    const tls::OpenSslTlsContext& server) {
  constexpr std::array<std::uint8_t, 17> kWrongAlpn = {'x', 'n', 'n', '-', 't', 'r',
                                                       'a', 'n', 's', 'f', 'e', 'r',
                                                       '-', 'w', 'r', 'o', 'n'};
  auto wrong_client = tls::OpenSslTlsContext::Create(tls::TlsEndpointRole::kClient,
                                                     client_identity, kWrongAlpn);
  Expect(wrong_client.ok(), "different strict ALPN context can be constructed");
  if (wrong_client.ok()) {
    Expect(!Connect(wrong_client.value->native_handle(), server.native_handle())
                .has_value(),
           "ALPN mismatch aborts handshake");
  }

  ContextPointer anonymous_context(SSL_CTX_new(TLS_method()), &SSL_CTX_free);
  Expect(anonymous_context != nullptr, "anonymous client context allocates");
  if (!anonymous_context) {
    return;
  }
  constexpr std::array<std::uint8_t, 20> kTestAlpn = {'x', 'n', 'n', '-', 't', 'r', 'a',
                                                      'n', 's', 'f', 'e', 'r', '-', 't',
                                                      'e', 's', 't', '-', 'v', '1'};
  std::array<unsigned char, kTestAlpn.size() + 1> wire_alpn{};
  wire_alpn[0] = static_cast<unsigned char>(kTestAlpn.size());
  std::copy(kTestAlpn.begin(), kTestAlpn.end(), wire_alpn.begin() + 1);
  const bool configured =
      SSL_CTX_set_min_proto_version(anonymous_context.get(), TLS1_3_VERSION) == 1 &&
      SSL_CTX_set_max_proto_version(anonymous_context.get(), TLS1_3_VERSION) == 1 &&
      SSL_CTX_set_ciphersuites(anonymous_context.get(),
                               "TLS_AES_128_GCM_SHA256:"
                               "TLS_CHACHA20_POLY1305_SHA256") == 1 &&
      SSL_CTX_set1_groups_list(anonymous_context.get(), "X25519") == 1 &&
      SSL_CTX_set_alpn_protos(anonymous_context.get(), wire_alpn.data(),
                              static_cast<unsigned int>(wire_alpn.size())) == 0;
  Expect(configured, "anonymous negative context configures");
  if (configured) {
    Expect(!Connect(anonymous_context.get(), server.native_handle()).has_value(),
           "missing client identity certificate aborts handshake");
  }
}

}  // namespace

int main() {
  IdentityFixture client_identity;
  IdentityFixture server_identity;
  Expect(client_identity.repository.Open().ok(), "client protected identity opens");
  Expect(server_identity.repository.Open().ok(), "server protected identity opens");
  if (!client_identity.repository.ready() || !server_identity.repository.ready()) {
    return 1;
  }

  constexpr std::array<std::uint8_t, 20> kTestAlpn = {'x', 'n', 'n', '-', 't', 'r', 'a',
                                                      'n', 's', 'f', 'e', 'r', '-', 't',
                                                      'e', 's', 't', '-', 'v', '1'};
  auto client = tls::OpenSslTlsContext::Create(tls::TlsEndpointRole::kClient,
                                               client_identity.repository, kTestAlpn);
  auto server = tls::OpenSslTlsContext::Create(tls::TlsEndpointRole::kServer,
                                               server_identity.repository, kTestAlpn);
  Expect(client.ok(), "client TLS 1.3 context configures");
  Expect(server.ok(), "server TLS 1.3 context configures");

  const std::array<std::uint8_t, 0> empty_alpn{};
  const auto missing_alpn = tls::OpenSslTlsContext::Create(
      tls::TlsEndpointRole::kClient, client_identity.repository, empty_alpn);
  Expect(!missing_alpn.ok() && missing_alpn.error == tls::SecurityError::kAlpnMismatch,
         "missing ALPN fails closed");

  if (client.ok() && server.ok()) {
    TestConfiguration(*client.value, *server.value);
    TestHandshakeAndExporters(*client.value, *server.value,
                              *client_identity.repository.root_public_key(),
                              *server_identity.repository.root_public_key());
    TestStaleCapabilityRejectedAfterConnectionReuse(
        *client.value, *server.value, *server_identity.repository.root_public_key());
    TestAlpnAndCertificateFailures(client_identity.repository, *server.value);
  }

  if (failures != 0) {
    std::cerr << failures << " TLS provider test(s) failed\n";
    return 1;
  }
  std::cout << "TLS 1.3 provider configuration and live handshake passed\n";
  return 0;
}
