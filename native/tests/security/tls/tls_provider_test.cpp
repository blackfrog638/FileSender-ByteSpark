#include "xnn_transfer/core/security/tls/tls_provider.hpp"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/objects.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

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
#include <string>
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
using CertificatePointer = std::unique_ptr<X509, decltype(&X509_free)>;
using ExtensionPointer =
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>;
using ObjectPointer = std::unique_ptr<ASN1_OBJECT, decltype(&ASN1_OBJECT_free)>;
using OctetStringPointer =
    std::unique_ptr<ASN1_OCTET_STRING, decltype(&ASN1_OCTET_STRING_free)>;

constexpr long kMaxPeerCertificateListBytes = 8'192;
constexpr long kTls13CertificateBodyOverhead = 4;
constexpr long kMaxPeerCertificateMessageBodyBytes =
    kTls13CertificateBodyOverhead + kMaxPeerCertificateListBytes;
constexpr std::size_t kMaxPeerCertificateDerBytes = 4'096;
constexpr std::size_t kTls13CertificateEntryOverhead = 5;
constexpr std::size_t kFragmentedTransportBufferBytes = 257;

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

struct HandshakeFailure {
  tls::TlsEndpointRole role{};
  int reason{};
};

void ExpectReason(const std::optional<HandshakeFailure>& failure,
                  const tls::TlsEndpointRole expected_role, const int expected_reason,
                  const std::string_view message) {
  if (failure.has_value() && failure->role == expected_role &&
      failure->reason == expected_reason) {
    return;
  }
  std::cerr << "FAILED: " << message << " (expected "
            << (expected_role == tls::TlsEndpointRole::kClient ? "client" : "server")
            << " OpenSSL reason " << expected_reason << ", got ";
  if (failure.has_value()) {
    std::cerr << (failure->role == tls::TlsEndpointRole::kClient ? "client" : "server")
              << " reason " << failure->reason;
  } else {
    std::cerr << "no handshake error";
  }
  std::cerr << ")\n";
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

bool CompleteHandshake(SSL* const client, SSL* const server,
                       const std::size_t transport_buffer_size = 0) {
  BIO* client_bio = nullptr;
  BIO* server_bio = nullptr;
  if (BIO_new_bio_pair(&client_bio, transport_buffer_size, &server_bio,
                       transport_buffer_size) != 1) {
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
                                 SSL_SESSION* const requested_session = nullptr,
                                 const std::size_t transport_buffer_size = 0) {
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
  if (!CompleteHandshake(handshake.client.get(), handshake.server.get(),
                         transport_buffer_size)) {
    return std::nullopt;
  }
  return handshake;
}

std::optional<HandshakeFailure> HandshakeFailureReason(
    SSL_CTX* const client_context, SSL_CTX* const server_context,
    const std::size_t transport_buffer_size) {
  Handshake handshake{
      .client = SslPointer(SSL_new(client_context), &SSL_free),
      .server = SslPointer(SSL_new(server_context), &SSL_free),
  };
  if (!handshake.client || !handshake.server) {
    return std::nullopt;
  }

  BIO* client_bio = nullptr;
  BIO* server_bio = nullptr;
  if (BIO_new_bio_pair(&client_bio, transport_buffer_size, &server_bio,
                       transport_buffer_size) != 1) {
    return std::nullopt;
  }
  SSL_set_bio(handshake.client.get(), client_bio, client_bio);
  SSL_set_bio(handshake.server.get(), server_bio, server_bio);
  SSL_set_connect_state(handshake.client.get());
  SSL_set_accept_state(handshake.server.get());

  for (std::size_t iteration = 0; iteration < 1024; ++iteration) {
    for (const auto& [role, connection] :
         std::array<std::pair<tls::TlsEndpointRole, SSL*>, 2>{
             std::pair{tls::TlsEndpointRole::kClient, handshake.client.get()},
             std::pair{tls::TlsEndpointRole::kServer, handshake.server.get()}}) {
      ERR_clear_error();
      const int result = SSL_do_handshake(connection);
      if (result == 1) {
        continue;
      }
      const int error = SSL_get_error(connection, result);
      if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
        continue;
      }
      const unsigned long error_code = ERR_peek_last_error();
      return error_code == 0 ? std::nullopt
                             : std::optional<HandshakeFailure>(
                                   HandshakeFailure{role, ERR_GET_REASON(error_code)});
    }
  }
  return std::nullopt;
}

bool ReconnectClient(Handshake& handshake, SSL_CTX* const server_context) {
  if (SSL_clear(handshake.client.get()) != 1) {
    return false;
  }
  handshake.server.reset(SSL_new(server_context));
  return handshake.server != nullptr &&
         CompleteHandshake(handshake.client.get(), handshake.server.get());
}

CertificatePointer BuildPaddedIdentityCertificate(SSL_CTX* const context,
                                                  const std::size_t padding_size) {
  X509* const source = SSL_CTX_get0_certificate(context);
  EVP_PKEY* const private_key = SSL_CTX_get0_privatekey(context);
  CertificatePointer certificate(source == nullptr ? nullptr : X509_dup(source),
                                 &X509_free);
  ObjectPointer object(OBJ_txt2obj("1.3.6.1.4.1.55555.1", 1), &ASN1_OBJECT_free);
  OctetStringPointer padding(ASN1_OCTET_STRING_new(), &ASN1_OCTET_STRING_free);
  if (!certificate || private_key == nullptr || !object || !padding ||
      padding_size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return CertificatePointer(nullptr, &X509_free);
  }

  const std::vector<unsigned char> padding_bytes(padding_size, 0xa5U);
  if (ASN1_OCTET_STRING_set(padding.get(), padding_bytes.data(),
                            static_cast<int>(padding_bytes.size())) != 1) {
    return CertificatePointer(nullptr, &X509_free);
  }
  ExtensionPointer extension(
      X509_EXTENSION_create_by_OBJ(nullptr, object.get(), 0, padding.get()),
      &X509_EXTENSION_free);
  if (!extension || X509_add_ext(certificate.get(), extension.get(), -1) != 1 ||
      X509_sign(certificate.get(), private_key, nullptr) <= 0) {
    return CertificatePointer(nullptr, &X509_free);
  }
  return certificate;
}

CertificatePointer BuildIdentityCertificateWithEncodedSize(
    SSL_CTX* const context, const std::size_t target_size) {
  std::size_t lower = 0;
  std::size_t upper = target_size;
  while (lower <= upper) {
    const std::size_t padding_size = lower + ((upper - lower) / 2);
    CertificatePointer certificate =
        BuildPaddedIdentityCertificate(context, padding_size);
    if (!certificate) {
      return CertificatePointer(nullptr, &X509_free);
    }
    const int encoded_size = i2d_X509(certificate.get(), nullptr);
    if (encoded_size <= 0) {
      return CertificatePointer(nullptr, &X509_free);
    }
    const std::size_t actual_size = static_cast<std::size_t>(encoded_size);
    if (actual_size == target_size) {
      return certificate;
    }
    if (actual_size < target_size) {
      lower = padding_size + 1;
      continue;
    }
    if (padding_size == 0) {
      break;
    }
    upper = padding_size - 1;
  }
  return CertificatePointer(nullptr, &X509_free);
}

bool InstallIdentityCertificate(SSL_CTX* const context, X509* const certificate) {
  return context != nullptr && certificate != nullptr &&
         SSL_CTX_use_certificate(context, certificate) == 1 &&
         SSL_CTX_check_private_key(context) == 1;
}

struct PeerCertificateDirection {
  SSL_CTX* client_context{};
  SSL_CTX* server_context{};
  SSL_CTX* sending_context{};
  tls::TlsEndpointRole rejecting_role{};
  std::string_view description{};
};

std::string Described(const PeerCertificateDirection& direction,
                      const std::string_view behavior) {
  std::string result(direction.description);
  result += " ";
  result += behavior;
  return result;
}

void TestPeerCertificateBoundsForDirection(const PeerCertificateDirection& direction) {
  CertificatePointer original(
      X509_dup(SSL_CTX_get0_certificate(direction.sending_context)), &X509_free);
  CertificatePointer below_der = BuildIdentityCertificateWithEncodedSize(
      direction.sending_context, kMaxPeerCertificateDerBytes - 1);
  CertificatePointer exact_der = BuildIdentityCertificateWithEncodedSize(
      direction.sending_context, kMaxPeerCertificateDerBytes);
  CertificatePointer above_der = BuildIdentityCertificateWithEncodedSize(
      direction.sending_context, kMaxPeerCertificateDerBytes + 1);
  CertificatePointer below_list = BuildIdentityCertificateWithEncodedSize(
      direction.sending_context,
      static_cast<std::size_t>(kMaxPeerCertificateListBytes) - 1 -
          kTls13CertificateEntryOverhead);
  CertificatePointer exact_list = BuildIdentityCertificateWithEncodedSize(
      direction.sending_context,
      static_cast<std::size_t>(kMaxPeerCertificateListBytes) -
          kTls13CertificateEntryOverhead);
  CertificatePointer above_list = BuildIdentityCertificateWithEncodedSize(
      direction.sending_context,
      static_cast<std::size_t>(kMaxPeerCertificateListBytes) + 1 -
          kTls13CertificateEntryOverhead);

  const bool fixtures_ready = original && below_der && exact_der && above_der &&
                              below_list && exact_list && above_list;
  Expect(fixtures_ready,
         Described(direction, "constructs exact certificate boundary fixtures"));
  if (!fixtures_ready) {
    return;
  }

  auto expect_success = [&](X509* const certificate, const std::string_view behavior) {
    const bool installed =
        SSL_CTX_clear_chain_certs(direction.sending_context) == 1 &&
        InstallIdentityCertificate(direction.sending_context, certificate);
    Expect(installed, Described(direction, "installs a valid boundary certificate"));
    if (installed) {
      Expect(Connect(direction.client_context, direction.server_context, nullptr,
                     kFragmentedTransportBufferBytes)
                 .has_value(),
             Described(direction, behavior));
    }
  };
  auto expect_failure = [&](X509* const certificate, const int expected_reason,
                            const std::string_view behavior) {
    const bool installed =
        SSL_CTX_clear_chain_certs(direction.sending_context) == 1 &&
        InstallIdentityCertificate(direction.sending_context, certificate);
    Expect(installed, Described(direction, "installs a hostile boundary certificate"));
    if (installed) {
      const auto reason =
          HandshakeFailureReason(direction.client_context, direction.server_context,
                                 kFragmentedTransportBufferBytes);
      ExpectReason(reason, direction.rejecting_role, expected_reason,
                   Described(direction, behavior));
    }
  };

  expect_success(below_der.get(),
                 "accepts a fragmented certificate below the DER ceiling");
  expect_success(exact_der.get(),
                 "accepts a fragmented certificate at the DER ceiling");
  expect_failure(above_der.get(), SSL_R_CERTIFICATE_VERIFY_FAILED,
                 "rejects a fragmented certificate above the DER ceiling");

  expect_failure(below_list.get(), SSL_R_CERTIFICATE_VERIFY_FAILED,
                 "parses a below-limit certificate message before rejecting its leaf");
  expect_failure(
      exact_list.get(), SSL_R_CERTIFICATE_VERIFY_FAILED,
      "parses the exact-limit certificate message before rejecting its leaf");
  expect_failure(
      above_list.get(), SSL_R_EXCESSIVE_MESSAGE_SIZE,
      "rejects an above-limit certificate message before certificate parsing");

  const bool chain_configured =
      SSL_CTX_clear_chain_certs(direction.sending_context) == 1 &&
      InstallIdentityCertificate(direction.sending_context, original.get()) &&
      SSL_CTX_add1_chain_cert(direction.sending_context, below_der.get()) == 1;
  Expect(chain_configured,
         Described(direction, "constructs a two-certificate hostile chain"));
  if (chain_configured) {
    const auto reason =
        HandshakeFailureReason(direction.client_context, direction.server_context,
                               kFragmentedTransportBufferBytes);
    ExpectReason(
        reason, direction.rejecting_role, SSL_R_CERTIFICATE_VERIFY_FAILED,
        Described(direction, "rejects a peer chain containing two certificates"));
  }

  Expect(SSL_CTX_clear_chain_certs(direction.sending_context) == 1 &&
             InstallIdentityCertificate(direction.sending_context, original.get()),
         Described(direction, "restores its original identity certificate"));
}

void TestPeerCertificateBounds(const tls::OpenSslTlsContext& client,
                               const tls::OpenSslTlsContext& server) {
  TestPeerCertificateBoundsForDirection(PeerCertificateDirection{
      client.native_handle(), server.native_handle(), server.native_handle(),
      tls::TlsEndpointRole::kClient, "client endpoint"});
  TestPeerCertificateBoundsForDirection(PeerCertificateDirection{
      client.native_handle(), server.native_handle(), client.native_handle(),
      tls::TlsEndpointRole::kServer, "server endpoint"});
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
    Expect(SSL_CTX_get_max_cert_list(context) == kMaxPeerCertificateMessageBodyBytes,
           "peer Certificate body is capped before allocation");
    Expect(SSL_CTX_get_verify_depth(context) == 0,
           "peer certificate chain allows no intermediate certificate");
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
  Expect(ReconnectClient(*handshake, server.native_handle()),
         "same SSL object reconnects to the same peer");
  tls::PairingExporterInput input{};
  const auto same_peer_stale =
      client.ExportKeyingMaterial(*stale_capability.value, input);
  Expect(!same_peer_stale.ok() &&
             same_peer_stale.error == tls::SecurityError::kExporterFailure,
         "handshake Finished binding rejects stale same-peer capability");

  Expect(ReconnectClient(*handshake, replacement_server.value->native_handle()),
         "same SSL object reconnects to replacement peer");
  auto fresh_capability =
      client.VerifyPeer(handshake->client.get(), replacement_server_key);
  Expect(fresh_capability.ok(), "reused SSL object verifies replacement peer");
  if (!fresh_capability.ok()) {
    return;
  }

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

void TestExpiredCertificateRejected(const tls::OpenSslTlsContext& client,
                                    const tls::OpenSslTlsContext& server) {
  X509* const source = SSL_CTX_get0_certificate(server.native_handle());
  EVP_PKEY* const private_key = SSL_CTX_get0_privatekey(server.native_handle());
  CertificatePointer original(source == nullptr ? nullptr : X509_dup(source),
                              &X509_free);
  CertificatePointer expired(source == nullptr ? nullptr : X509_dup(source),
                             &X509_free);
  const bool prepared =
      original && expired && private_key != nullptr &&
      X509_gmtime_adj(X509_getm_notBefore(expired.get()), -120L) != nullptr &&
      X509_gmtime_adj(X509_getm_notAfter(expired.get()), -60L) != nullptr &&
      X509_sign(expired.get(), private_key, nullptr) > 0;
  Expect(prepared, "expired identity certificate fixture is prepared");
  if (!prepared) {
    return;
  }
  Expect(InstallIdentityCertificate(server.native_handle(), expired.get()),
         "expired identity certificate installs for negative handshake");
  Expect(!Connect(client.native_handle(), server.native_handle()).has_value(),
         "expired peer identity fails during TLS verification");
  Expect(InstallIdentityCertificate(server.native_handle(), original.get()),
         "valid identity certificate is restored after expiry test");
}

void TestMalformedCertificateTimeRejected(const tls::OpenSslTlsContext& client,
                                          const tls::OpenSslTlsContext& server) {
  X509* const source = SSL_CTX_get0_certificate(server.native_handle());
  EVP_PKEY* const private_key = SSL_CTX_get0_privatekey(server.native_handle());
  CertificatePointer original(source == nullptr ? nullptr : X509_dup(source),
                              &X509_free);
  CertificatePointer malformed(source == nullptr ? nullptr : X509_dup(source),
                               &X509_free);
  constexpr std::string_view kInvalidTime = "not-a-time";
  const bool prepared =
      original && malformed && private_key != nullptr &&
      ASN1_STRING_set(X509_getm_notAfter(malformed.get()), kInvalidTime.data(),
                      static_cast<int>(kInvalidTime.size())) == 1 &&
      X509_cmp_current_time(X509_get0_notAfter(malformed.get())) == 0 &&
      X509_sign(malformed.get(), private_key, nullptr) > 0;
  Expect(prepared, "malformed certificate-time fixture is prepared");
  if (!prepared) {
    return;
  }
  Expect(InstallIdentityCertificate(server.native_handle(), malformed.get()),
         "malformed certificate-time fixture installs");
  Expect(!Connect(client.native_handle(), server.native_handle()).has_value(),
         "unparseable certificate time fails during TLS verification");
  Expect(InstallIdentityCertificate(server.native_handle(), original.get()),
         "valid identity certificate is restored after malformed-time test");
}

void TestTypedServerDispatcher(IdentityFixture& client_identity,
                               IdentityFixture& server_identity) {
  std::uint64_t pairing_window_generation = 7U;
  auto dispatcher = tls::OpenSslTlsContext::CreateServerDispatcher(
      server_identity.repository,
      [&pairing_window_generation] { return pairing_window_generation; });
  auto pairing_client = tls::OpenSslTlsContext::Create(
      tls::TlsEndpointRole::kClient, client_identity.repository, tls::kPairingAlpn);
  auto established_client = tls::OpenSslTlsContext::Create(
      tls::TlsEndpointRole::kClient, client_identity.repository, tls::kEstablishedAlpn);
  Expect(dispatcher.ok() && pairing_client.ok() && established_client.ok(),
         "typed dispatcher and both registered clients configure");
  if (!dispatcher.ok() || !pairing_client.ok() || !established_client.ok()) {
    return;
  }

  auto pairing =
      Connect(pairing_client.value->native_handle(), dispatcher.value->native_handle());
  Expect(pairing.has_value(), "dispatcher accepts exact pairing ALPN");
  if (pairing.has_value()) {
    pairing_window_generation = 8U;
    auto accepted = dispatcher.value->AcceptServerPeer(pairing->server.get(),
                                                       server_identity.repository);
    const auto* pairing_capability =
        accepted.ok() ? std::get_if<tls::AcceptedPairingTlsConnection>(&*accepted.value)
                      : nullptr;
    Expect(pairing_capability != nullptr &&
               pairing_capability->pairing_window_generation() == 7U,
           "pairing capability retains its ClientHello window generation");
    const auto generic =
        dispatcher.value->VerifyPeer(pairing->server.get(), std::nullopt);
    Expect(!generic.ok() && generic.error == tls::SecurityError::kAlpnMismatch,
           "dispatcher never returns a generic unpinned capability");
    pairing_window_generation = 9U;
    const bool reused = SSL_clear(pairing->client.get()) == 1 &&
                        SSL_clear(pairing->server.get()) == 1 &&
                        CompleteHandshake(pairing->client.get(), pairing->server.get());
    Expect(reused, "dispatcher SSL objects complete a fresh pairing handshake");
    if (reused) {
      auto fresh = dispatcher.value->AcceptServerPeer(pairing->server.get(),
                                                      server_identity.repository);
      const auto* fresh_pairing =
          fresh.ok() ? std::get_if<tls::AcceptedPairingTlsConnection>(&*fresh.value)
                     : nullptr;
      Expect(
          fresh_pairing != nullptr && fresh_pairing->pairing_window_generation() == 9U,
          "reused dispatcher SSL records the fresh window generation");
    }
  }
  pairing_window_generation = 0U;
  Expect(
      !Connect(pairing_client.value->native_handle(), dispatcher.value->native_handle())
           .has_value(),
      "dispatcher rejects pairing ALPN while the local window is closed");

  auto unknown = Connect(established_client.value->native_handle(),
                         dispatcher.value->native_handle());
  Expect(unknown.has_value(),
         "unknown established identity may complete proof-safe TLS");
  if (unknown.has_value()) {
    const auto accepted = dispatcher.value->AcceptServerPeer(
        unknown->server.get(), server_identity.repository);
    Expect(!accepted.ok() && accepted.error == tls::SecurityError::kPinMismatch,
           "unknown established identity fails before typed dispatch");
  }

  const auto committed = server_identity.repository.CommitPeer(identity::PeerCommit{
      .public_key = *client_identity.repository.root_public_key(),
      .security_profile = tls::kSecurityProfileV1,
      .display_label = "active client",
  });
  Expect(committed.ok(), "dispatcher test commits an active exact pin");
  if (!committed.ok()) {
    return;
  }
  auto active = Connect(established_client.value->native_handle(),
                        dispatcher.value->native_handle());
  Expect(active.has_value(), "active established identity handshakes");
  if (active.has_value()) {
    auto accepted = dispatcher.value->AcceptServerPeer(active->server.get(),
                                                       server_identity.repository);
    const auto* established =
        accepted.ok()
            ? std::get_if<tls::AcceptedEstablishedTlsConnection>(&*accepted.value)
            : nullptr;
    Expect(established != nullptr && established->peer_device_id() == committed.value(),
           "active exact pin resolves the typed established capability");
  }

  Expect(server_identity.repository.RevokePeer(committed.value()).ok(),
         "dispatcher test revokes the active pin");
  auto revoked = Connect(established_client.value->native_handle(),
                         dispatcher.value->native_handle());
  Expect(revoked.has_value(), "revoked identity still proves its certificate");
  if (revoked.has_value()) {
    const auto accepted = dispatcher.value->AcceptServerPeer(
        revoked->server.get(), server_identity.repository);
    Expect(!accepted.ok() && accepted.error == tls::SecurityError::kPinMismatch,
           "revoked exact pin fails before established dispatch");
  }

  const auto make_hostile_alpn = [&](const std::vector<unsigned char>& wire) {
    const unsigned char empty_marker = 0U;
    const unsigned char* const protocols = wire.empty() ? &empty_marker : wire.data();
    Handshake handshake{
        .client = SslPointer(SSL_new(pairing_client.value->native_handle()), &SSL_free),
        .server = SslPointer(SSL_new(dispatcher.value->native_handle()), &SSL_free),
    };
    if (!handshake.client || !handshake.server ||
        SSL_set_alpn_protos(handshake.client.get(), protocols,
                            static_cast<unsigned int>(wire.size())) != 0) {
      return false;
    }
    return !CompleteHandshake(handshake.client.get(), handshake.server.get());
  };
  std::vector<unsigned char> multiple{
      static_cast<unsigned char>(tls::kPairingAlpn.size())};
  multiple.insert(multiple.end(), tls::kPairingAlpn.begin(), tls::kPairingAlpn.end());
  multiple.push_back(static_cast<unsigned char>(tls::kEstablishedAlpn.size()));
  multiple.insert(multiple.end(), tls::kEstablishedAlpn.begin(),
                  tls::kEstablishedAlpn.end());
  Expect(make_hostile_alpn(multiple),
         "offering both registered ALPN modes fails closed");

  std::vector<unsigned char> duplicate{
      static_cast<unsigned char>(tls::kPairingAlpn.size())};
  duplicate.insert(duplicate.end(), tls::kPairingAlpn.begin(), tls::kPairingAlpn.end());
  duplicate.push_back(static_cast<unsigned char>(tls::kPairingAlpn.size()));
  duplicate.insert(duplicate.end(), tls::kPairingAlpn.begin(), tls::kPairingAlpn.end());
  Expect(make_hostile_alpn(duplicate),
         "duplicating one registered ALPN mode fails closed");

  ContextPointer missing_alpn_context(SSL_CTX_new(TLS_method()), &SSL_CTX_free);
  const bool missing_alpn_configured =
      missing_alpn_context != nullptr &&
      SSL_CTX_set_min_proto_version(missing_alpn_context.get(), TLS1_3_VERSION) == 1 &&
      SSL_CTX_set_max_proto_version(missing_alpn_context.get(), TLS1_3_VERSION) == 1 &&
      SSL_CTX_set_ciphersuites(missing_alpn_context.get(),
                               "TLS_AES_128_GCM_SHA256:"
                               "TLS_CHACHA20_POLY1305_SHA256") == 1 &&
      SSL_CTX_set1_groups_list(missing_alpn_context.get(), "X25519") == 1 &&
      SSL_CTX_set1_sigalgs_list(missing_alpn_context.get(), "ed25519") == 1 &&
      SSL_CTX_use_certificate(
          missing_alpn_context.get(),
          SSL_CTX_get0_certificate(pairing_client.value->native_handle())) == 1 &&
      SSL_CTX_use_PrivateKey(
          missing_alpn_context.get(),
          SSL_CTX_get0_privatekey(pairing_client.value->native_handle())) == 1 &&
      SSL_CTX_check_private_key(missing_alpn_context.get()) == 1;
  Expect(missing_alpn_configured,
         "client context without ALPN retains the required TLS profile");
  if (missing_alpn_configured) {
    Expect(!Connect(missing_alpn_context.get(), dispatcher.value->native_handle())
                .has_value(),
           "omitting ALPN from the shared listener fails closed");
  }
  constexpr std::array<unsigned char, 7> kUnknownAlpn = {
      6U, 'u', 'n', 'k', 'n', 'o', 'w',
  };
  Expect(make_hostile_alpn(
             std::vector<unsigned char>(kUnknownAlpn.begin(), kUnknownAlpn.end())),
         "unknown ALPN on the shared listener fails closed");
}

void TestRotatedPinRejected() {
  IdentityFixture old_client;
  IdentityFixture new_client;
  IdentityFixture server;
  Expect(old_client.repository.Open().ok() && new_client.repository.Open().ok() &&
             server.repository.Open().ok(),
         "rotation dispatcher identities open");
  if (!old_client.repository.ready() || !new_client.repository.ready() ||
      !server.repository.ready()) {
    return;
  }
  const auto committed = server.repository.CommitPeer(identity::PeerCommit{
      .public_key = *old_client.repository.root_public_key(),
      .security_profile = tls::kSecurityProfileV1,
      .display_label = "old client",
  });
  Expect(committed.ok(), "old pin commits before rotation");
  if (!committed.ok()) {
    return;
  }
  const auto rotated = server.repository.RotatePeer(
      committed.value(), *new_client.repository.root_public_key(), 1U);
  Expect(rotated.ok(), "active pin rotates to the replacement identity");
  if (!rotated.ok()) {
    return;
  }
  auto dispatcher = tls::OpenSslTlsContext::CreateServerDispatcher(server.repository,
                                                                   [] { return true; });
  auto old_context = tls::OpenSslTlsContext::Create(
      tls::TlsEndpointRole::kClient, old_client.repository, tls::kEstablishedAlpn);
  auto new_context = tls::OpenSslTlsContext::Create(
      tls::TlsEndpointRole::kClient, new_client.repository, tls::kEstablishedAlpn);
  Expect(dispatcher.ok() && old_context.ok() && new_context.ok(),
         "rotation dispatcher contexts configure");
  if (!dispatcher.ok() || !old_context.ok() || !new_context.ok()) {
    return;
  }
  auto old_connection =
      Connect(old_context.value->native_handle(), dispatcher.value->native_handle());
  auto new_connection =
      Connect(new_context.value->native_handle(), dispatcher.value->native_handle());
  Expect(old_connection.has_value() && new_connection.has_value(),
         "old and replacement identities complete proof-safe TLS");
  if (old_connection.has_value()) {
    const auto accepted = dispatcher.value->AcceptServerPeer(
        old_connection->server.get(), server.repository);
    Expect(!accepted.ok() && accepted.error == tls::SecurityError::kPinMismatch,
           "rotated-away identity fails active-pin resolution");
  }
  if (new_connection.has_value()) {
    auto accepted = dispatcher.value->AcceptServerPeer(new_connection->server.get(),
                                                       server.repository);
    const auto* established =
        accepted.ok()
            ? std::get_if<tls::AcceptedEstablishedTlsConnection>(&*accepted.value)
            : nullptr;
    Expect(established != nullptr && established->peer_device_id() == rotated.value(),
           "replacement identity resolves after rotation");
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
    TestPeerCertificateBounds(*client.value, *server.value);
    TestExpiredCertificateRejected(*client.value, *server.value);
    TestMalformedCertificateTimeRejected(*client.value, *server.value);
  }
  TestTypedServerDispatcher(client_identity, server_identity);
  TestRotatedPinRejected();

  if (failures != 0) {
    std::cerr << failures << " TLS provider test(s) failed\n";
    return 1;
  }
  std::cout << "TLS 1.3 provider configuration and live handshake passed\n";
  return 0;
}
