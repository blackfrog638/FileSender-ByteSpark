#include "xnn_transfer/core/security/tls/tls_provider.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "internal.hpp"

namespace xnn_transfer::core::security::tls {
namespace {

constexpr std::string_view kTls13CipherSuites =
    "TLS_AES_128_GCM_SHA256:TLS_CHACHA20_POLY1305_SHA256";
constexpr std::string_view kMandatoryCipher = "TLS_AES_128_GCM_SHA256";
constexpr std::string_view kOptionalCipher = "TLS_CHACHA20_POLY1305_SHA256";
constexpr std::string_view kKeyExchangeGroup = "X25519";
constexpr std::string_view kSignatureAlgorithm = "ed25519";
constexpr long kCertificateLifetimeSeconds = 10L * 365L * 24L * 60L * 60L;
constexpr int kMaxPeerCertificateDerBytes = 4'096;
constexpr long kMaxPeerCertificateListBytes = 8'192;
// OpenSSL excludes the four-byte handshake header, but includes these TLS 1.3
// request-context and certificate-list length fields in max_cert_list.
constexpr long kTls13CertificateBodyOverhead = 4;
constexpr long kMaxPeerCertificateMessageBodyBytes =
    kTls13CertificateBodyOverhead + kMaxPeerCertificateListBytes;
constexpr std::size_t kMaxFinishedBytes = 64;

using ContextPointer = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using KeyPointer = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using CertificatePointer = std::unique_ptr<X509, decltype(&X509_free)>;

struct ServerAlpnPolicy {
  const std::vector<Bytes>* alpns{};
  const PairingAlpnPolicy* pairing_allowed{};
  bool enforce_pairing_window{};
};

struct HandshakeFinished {
  std::array<std::uint8_t, kMaxFinishedBytes> local{};
  std::array<std::uint8_t, kMaxFinishedBytes> peer{};
  std::size_t local_size{};
  std::size_t peer_size{};
};

struct PairingWindowAuthorization {
  std::uint64_t generation{};
  std::array<std::uint8_t, SSL3_RANDOM_SIZE> client_random{};
};

void FreePairingWindowAuthorization(void*, void* const pointer, CRYPTO_EX_DATA*, int,
                                    long, void*) noexcept {
  delete static_cast<PairingWindowAuthorization*>(pointer);
}

[[nodiscard]] int PairingWindowAuthorizationIndex() noexcept {
  static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr,
                                                &FreePairingWindowAuthorization);
  return index;
}

[[nodiscard]] std::uint64_t PairingWindowGeneration(
    const SSL* const connection) noexcept {
  const int index = PairingWindowAuthorizationIndex();
  if (connection == nullptr || index < 0) {
    return 0U;
  }
  const auto* const authorization = static_cast<const PairingWindowAuthorization*>(
      SSL_get_ex_data(connection, index));
  return authorization == nullptr ? 0U : authorization->generation;
}

[[nodiscard]] bool CaptureClientHelloRandom(
    SSL* const connection,
    std::array<std::uint8_t, SSL3_RANDOM_SIZE>& output) noexcept {
  const unsigned char* random = nullptr;
  const std::size_t size = SSL_client_hello_get0_random(connection, &random);
  if (random == nullptr || size != output.size()) {
    return false;
  }
  std::copy_n(random, output.size(), output.begin());
  return true;
}

[[nodiscard]] std::optional<HandshakeFinished> CaptureHandshakeFinished(
    SSL* const connection) noexcept {
  if (connection == nullptr || SSL_is_init_finished(connection) != 1) {
    return std::nullopt;
  }
  HandshakeFinished output;
  output.local_size =
      SSL_get_finished(connection, output.local.data(), output.local.size());
  output.peer_size =
      SSL_get_peer_finished(connection, output.peer.data(), output.peer.size());
  if (output.local_size == 0U || output.local_size > output.local.size() ||
      output.peer_size == 0U || output.peer_size > output.peer.size()) {
    return std::nullopt;
  }
  return output;
}

[[nodiscard]] int ValidateUntrustedIdentityCertificate(
    int, X509_STORE_CTX* const store_context) noexcept {
  // This callback runs before TLS CertificateVerify. It establishes only key
  // shape and proof-safety; trust still comes from the post-handshake pin.
  if (store_context == nullptr || X509_STORE_CTX_get_error_depth(store_context) != 0) {
    return 0;
  }
  X509* const certificate = X509_STORE_CTX_get_current_cert(store_context);
  const int encoded_size = certificate == nullptr ? -1 : i2d_X509(certificate, nullptr);
  STACK_OF(X509)* const untrusted_chain = X509_STORE_CTX_get0_untrusted(store_context);
  const int not_before = certificate == nullptr
                             ? 0
                             : X509_cmp_current_time(X509_get0_notBefore(certificate));
  const int not_after = certificate == nullptr
                            ? 0
                            : X509_cmp_current_time(X509_get0_notAfter(certificate));
  if (untrusted_chain == nullptr || sk_X509_num(untrusted_chain) != 1 ||
      sk_X509_value(untrusted_chain, 0) != certificate || encoded_size <= 0 ||
      encoded_size > kMaxPeerCertificateDerBytes || not_before >= 0 || not_after <= 0 ||
      X509_NAME_cmp(X509_get_subject_name(certificate),
                    X509_get_issuer_name(certificate)) != 0) {
    return 0;
  }

  KeyPointer public_key(X509_get_pubkey(certificate), &EVP_PKEY_free);
  if (!public_key || !EVP_PKEY_is_a(public_key.get(), "ED25519") ||
      X509_verify(certificate, public_key.get()) != 1) {
    return 0;
  }
  identity::PublicKey raw_public_key{};
  std::size_t raw_public_key_size = raw_public_key.size();
  if (EVP_PKEY_get_raw_public_key(public_key.get(), raw_public_key.data(),
                                  &raw_public_key_size) != 1 ||
      raw_public_key_size != raw_public_key.size()) {
    return 0;
  }
  return ValidateEd25519PublicKey(raw_public_key).ok() ? 1 : 0;
}

[[nodiscard]] bool IsAllowedCipher(const std::string_view cipher) noexcept {
  return cipher == kMandatoryCipher || cipher == kOptionalCipher;
}

[[nodiscard]] bool EqualPublicKeys(const identity::PublicKey& left,
                                   const identity::PublicKey& right) noexcept {
  return CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

[[nodiscard]] bool SetRandomCertificateSerial(X509* const certificate) {
  std::array<std::uint8_t, 16> serial_bytes{};
  if (RAND_bytes(serial_bytes.data(), static_cast<int>(serial_bytes.size())) != 1) {
    return false;
  }
  serial_bytes[0] &= 0x7fU;
  serial_bytes[0] |= 0x40U;

  BIGNUM* serial_number =
      BN_bin2bn(serial_bytes.data(), static_cast<int>(serial_bytes.size()), nullptr);
  OPENSSL_cleanse(serial_bytes.data(), serial_bytes.size());
  if (serial_number == nullptr) {
    return false;
  }
  const std::unique_ptr<BIGNUM, decltype(&BN_free)> serial(serial_number, &BN_free);
  return BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(certificate)) !=
         nullptr;
}

[[nodiscard]] Result<CertificatePointer> BuildIdentityCertificate(
    const std::span<const std::uint8_t> seed,
    const identity::PublicKey& expected_public_key, KeyPointer& private_key) {
  if (seed.size() != identity::kEd25519SeedSize) {
    return {.error = SecurityError::kIdentityUnavailable};
  }

  private_key.reset(EVP_PKEY_new_raw_private_key_ex(nullptr, "ED25519", nullptr,
                                                    seed.data(), seed.size()));
  if (!private_key) {
    return {.error = SecurityError::kCryptoFailure};
  }

  identity::PublicKey actual_public_key{};
  std::size_t actual_public_key_size = actual_public_key.size();
  if (EVP_PKEY_get_raw_public_key(private_key.get(), actual_public_key.data(),
                                  &actual_public_key_size) != 1 ||
      actual_public_key_size != actual_public_key.size() ||
      !EqualPublicKeys(actual_public_key, expected_public_key)) {
    return {.error = SecurityError::kIdentityUnavailable};
  }

  CertificatePointer certificate(X509_new(), &X509_free);
  if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
      !SetRandomCertificateSerial(certificate.get()) ||
      X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60L) == nullptr ||
      X509_gmtime_adj(X509_getm_notAfter(certificate.get()),
                      kCertificateLifetimeSeconds) == nullptr ||
      X509_set_pubkey(certificate.get(), private_key.get()) != 1) {
    return {.error = SecurityError::kCryptoFailure};
  }

  X509_NAME* const subject = X509_get_subject_name(certificate.get());
  constexpr std::string_view kCommonName = "XnnTransfer identity";
  if (subject == nullptr ||
      X509_NAME_add_entry_by_txt(
          subject, "CN", MBSTRING_ASC,
          reinterpret_cast<const unsigned char*>(kCommonName.data()),
          static_cast<int>(kCommonName.size()), -1, 0) != 1 ||
      X509_set_issuer_name(certificate.get(), subject) != 1 ||
      X509_sign(certificate.get(), private_key.get(), nullptr) <= 0) {
    return {.error = SecurityError::kCryptoFailure};
  }

  return {
      .value = std::move(certificate),
      .error = SecurityError::kNone,
  };
}

}  // namespace

struct OpenSslTlsContext::Implementation {
  Implementation(TlsEndpointRole endpoint_role, std::vector<Bytes> protocols,
                 const bool dispatch_server, PairingAlpnPolicy pairing_policy)
      : role(endpoint_role),
        alpns(std::move(protocols)),
        server_dispatcher(dispatch_server),
        pairing_alpn_policy(std::move(pairing_policy)),
        context(SSL_CTX_new(TLS_method()), &SSL_CTX_free) {}

  TlsEndpointRole role;
  std::vector<Bytes> alpns;
  bool server_dispatcher{};
  PairingAlpnPolicy pairing_alpn_policy{};
  ServerAlpnPolicy server_alpn_policy{};
  ContextPointer context;
};

namespace {

[[nodiscard]] bool IsSingleRegisteredAlpn(
    const unsigned char* input, const std::size_t input_size,
    const std::vector<Bytes>* const alpns) noexcept {
  if (alpns == nullptr || input == nullptr || input_size < 2U || input[0] == 0U ||
      input_size != static_cast<std::size_t>(input[0]) + 1U) {
    return false;
  }
  return std::any_of(alpns->begin(), alpns->end(), [input](const Bytes& alpn) {
    return alpn.size() == input[0] &&
           CRYPTO_memcmp(input + 1, alpn.data(), alpn.size()) == 0;
  });
}

[[nodiscard]] bool PairingAllowed(SSL* const connection, const unsigned char* input,
                                  const std::size_t input_size,
                                  const ServerAlpnPolicy* const policy,
                                  const bool authorize) noexcept {
  if (policy == nullptr || policy->alpns == nullptr ||
      !IsSingleRegisteredAlpn(input, input_size, policy->alpns)) {
    return false;
  }
  const bool pairing =
      input[0] == kPairingAlpn.size() &&
      CRYPTO_memcmp(input + 1, kPairingAlpn.data(), kPairingAlpn.size()) == 0;
  if (!pairing) {
    return true;
  }
  if (!policy->enforce_pairing_window) {
    return true;
  }
  if (policy->pairing_allowed == nullptr || !*policy->pairing_allowed) {
    return false;
  }
  std::uint64_t generation = 0U;
  try {
    generation = (*policy->pairing_allowed)();
  } catch (...) {
    return false;
  }
  if (generation == 0U) {
    return false;
  }

  const int index = PairingWindowAuthorizationIndex();
  if (connection == nullptr || index < 0) {
    return false;
  }
  const auto* const existing = static_cast<const PairingWindowAuthorization*>(
      SSL_get_ex_data(connection, index));
  if (existing != nullptr) {
    if (!authorize) {
      return existing->generation == generation;
    }
    std::array<std::uint8_t, SSL3_RANDOM_SIZE> client_random{};
    if (!CaptureClientHelloRandom(connection, client_random)) {
      return false;
    }
    if (CRYPTO_memcmp(existing->client_random.data(), client_random.data(),
                      client_random.size()) == 0) {
      return existing->generation == generation;
    }
  }
  if (!authorize) {
    return false;
  }
  auto* const authorization = new (std::nothrow) PairingWindowAuthorization{
      .generation = generation,
  };
  if (authorization != nullptr &&
      !CaptureClientHelloRandom(connection, authorization->client_random)) {
    delete authorization;
    return false;
  }
  if (authorization == nullptr ||
      SSL_set_ex_data(connection, index, authorization) != 1) {
    delete authorization;
    return false;
  }
  delete existing;
  return true;
}

[[nodiscard]] int RequireRegisteredAlpnClientHello(SSL* connection, int* alert,
                                                   void* argument) noexcept {
  const unsigned char* extension = nullptr;
  std::size_t extension_size = 0;
  if (connection == nullptr || alert == nullptr ||
      SSL_client_hello_get0_ext(connection,
                                TLSEXT_TYPE_application_layer_protocol_negotiation,
                                &extension, &extension_size) != 1 ||
      extension == nullptr || extension_size < 3U) {
    if (alert != nullptr) {
      *alert = SSL_AD_NO_APPLICATION_PROTOCOL;
    }
    return SSL_CLIENT_HELLO_ERROR;
  }
  const std::size_t list_size = (static_cast<std::size_t>(extension[0]) << 8U) |
                                static_cast<std::size_t>(extension[1]);
  if (list_size != extension_size - 2U ||
      !PairingAllowed(connection, extension + 2U, list_size,
                      static_cast<const ServerAlpnPolicy*>(argument), true)) {
    *alert = SSL_AD_NO_APPLICATION_PROTOCOL;
    return SSL_CLIENT_HELLO_ERROR;
  }
  return SSL_CLIENT_HELLO_SUCCESS;
}

[[nodiscard]] int SelectRegisteredAlpn(SSL* connection, const unsigned char** output,
                                       unsigned char* output_size,
                                       const unsigned char* input,
                                       unsigned int input_size,
                                       void* argument) noexcept {
  if (output == nullptr || output_size == nullptr ||
      !PairingAllowed(connection, input, input_size,
                      static_cast<const ServerAlpnPolicy*>(argument), false)) {
    return SSL_TLSEXT_ERR_ALERT_FATAL;
  }
  *output = input + 1;
  *output_size = input[0];
  return SSL_TLSEXT_ERR_OK;
}

[[nodiscard]] SecurityError ConfigureContext(SSL_CTX* const context,
                                             const TlsEndpointRole role,
                                             const std::vector<Bytes>& alpns,
                                             ServerAlpnPolicy* const server_alpn_policy,
                                             X509* const certificate,
                                             EVP_PKEY* const private_key) {
  if (context == nullptr || alpns.empty()) {
    return SecurityError::kTlsConfigurationFailure;
  }
  static_cast<void>(
      SSL_CTX_set_max_cert_list(context, kMaxPeerCertificateMessageBodyBytes));
  SSL_CTX_set_verify_depth(context, 0);

  if (SSL_CTX_set_min_proto_version(context, TLS1_3_VERSION) != 1 ||
      SSL_CTX_set_max_proto_version(context, TLS1_3_VERSION) != 1 ||
      SSL_CTX_set_ciphersuites(context, kTls13CipherSuites.data()) != 1 ||
      SSL_CTX_set1_groups_list(context, kKeyExchangeGroup.data()) != 1 ||
      SSL_CTX_set1_sigalgs_list(context, kSignatureAlgorithm.data()) != 1 ||
      SSL_CTX_set_num_tickets(context, 0) != 1 ||
      SSL_CTX_set_max_early_data(context, 0) != 1 ||
      SSL_CTX_use_certificate(context, certificate) != 1 ||
      SSL_CTX_use_PrivateKey(context, private_key) != 1 ||
      SSL_CTX_check_private_key(context) != 1) {
    return SecurityError::kTlsConfigurationFailure;
  }

  static_cast<void>(SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF));
  const std::uint64_t options = SSL_CTX_set_options(context, SSL_OP_NO_TICKET);
  if ((options & SSL_OP_NO_TICKET) == 0 ||
      SSL_CTX_get_min_proto_version(context) != TLS1_3_VERSION ||
      SSL_CTX_get_max_proto_version(context) != TLS1_3_VERSION ||
      SSL_CTX_get_max_cert_list(context) != kMaxPeerCertificateMessageBodyBytes ||
      SSL_CTX_get_verify_depth(context) != 0) {
    return SecurityError::kTlsConfigurationFailure;
  }

  SSL_CTX_set_verify(context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                     &ValidateUntrustedIdentityCertificate);

  if (role == TlsEndpointRole::kClient) {
    if (alpns.size() != 1U) {
      return SecurityError::kTlsConfigurationFailure;
    }
    const Bytes& alpn = alpns.front();
    std::vector<unsigned char> wire_alpn;
    wire_alpn.reserve(alpn.size() + 1);
    wire_alpn.push_back(static_cast<unsigned char>(alpn.size()));
    wire_alpn.insert(wire_alpn.end(), alpn.begin(), alpn.end());
    if (SSL_CTX_set_alpn_protos(context, wire_alpn.data(),
                                static_cast<unsigned int>(wire_alpn.size())) != 0) {
      return SecurityError::kTlsConfigurationFailure;
    }
  } else {
    SSL_CTX_set_client_hello_cb(context, &RequireRegisteredAlpnClientHello,
                                server_alpn_policy);
    SSL_CTX_set_alpn_select_cb(context, &SelectRegisteredAlpn, server_alpn_policy);
  }
  return SecurityError::kNone;
}

[[nodiscard]] SecurityError ValidateNegotiatedParameters(
    SSL* const connection, SSL_CTX* const expected_context,
    const std::span<const std::uint8_t> alpn) {
  if (connection == nullptr || SSL_get_SSL_CTX(connection) != expected_context ||
      SSL_is_init_finished(connection) != 1) {
    return SecurityError::kHandshakeIncomplete;
  }
  if (SSL_version(connection) != TLS1_3_VERSION) {
    return SecurityError::kTlsVersionMismatch;
  }

  const SSL_CIPHER* const cipher = SSL_get_current_cipher(connection);
  if (cipher == nullptr || !IsAllowedCipher(SSL_CIPHER_get_name(cipher))) {
    return SecurityError::kCipherMismatch;
  }
  if (SSL_get_negotiated_group(connection) != NID_X25519) {
    return SecurityError::kGroupMismatch;
  }

  int signature_type = NID_undef;
  if (SSL_get_peer_signature_type_nid(connection, &signature_type) != 1 ||
      signature_type != NID_ED25519) {
    return SecurityError::kSignatureMismatch;
  }
  if (SSL_session_reused(connection) != 0) {
    return SecurityError::kResumptionDetected;
  }
  if (SSL_get_early_data_status(connection) != SSL_EARLY_DATA_NOT_SENT) {
    return SecurityError::kEarlyDataDetected;
  }

  const unsigned char* selected_alpn = nullptr;
  unsigned int selected_alpn_size = 0;
  SSL_get0_alpn_selected(connection, &selected_alpn, &selected_alpn_size);
  if (selected_alpn == nullptr || selected_alpn_size != alpn.size() ||
      CRYPTO_memcmp(selected_alpn, alpn.data(), alpn.size()) != 0) {
    return SecurityError::kAlpnMismatch;
  }
  return SecurityError::kNone;
}

[[nodiscard]] Result<ValidatedEd25519PublicKey> ExtractValidatedPeerPublicKey(
    SSL* const connection) {
  X509* const certificate = SSL_get0_peer_certificate(connection);
  STACK_OF(X509)* const presented_chain = SSL_get_peer_cert_chain(connection);
  STACK_OF(X509)* const verified_chain = SSL_get0_verified_chain(connection);
  const int presented_chain_size =
      presented_chain == nullptr ? -1 : sk_X509_num(presented_chain);
  const bool exact_presented_chain =
      SSL_is_server(connection) == 1
          ? presented_chain_size == 0
          : presented_chain_size == 1 &&
                sk_X509_value(presented_chain, 0) == certificate;
  if (certificate == nullptr || !exact_presented_chain || verified_chain == nullptr ||
      sk_X509_num(verified_chain) != 1 ||
      sk_X509_value(verified_chain, 0) != certificate ||
      X509_NAME_cmp(X509_get_subject_name(certificate),
                    X509_get_issuer_name(certificate)) != 0) {
    return {.error = SecurityError::kPeerCertificateMismatch};
  }

  KeyPointer public_key(X509_get_pubkey(certificate), &EVP_PKEY_free);
  if (!public_key || !EVP_PKEY_is_a(public_key.get(), "ED25519") ||
      X509_verify(certificate, public_key.get()) != 1) {
    return {.error = SecurityError::kPeerCertificateMismatch};
  }

  identity::PublicKey raw_public_key{};
  std::size_t raw_public_key_size = raw_public_key.size();
  if (EVP_PKEY_get_raw_public_key(public_key.get(), raw_public_key.data(),
                                  &raw_public_key_size) != 1 ||
      raw_public_key_size != raw_public_key.size()) {
    return {.error = SecurityError::kPeerCertificateMismatch};
  }
  return ValidateEd25519PublicKey(raw_public_key);
}

}  // namespace

Result<OpenSslTlsContext> OpenSslTlsContext::Create(
    const TlsEndpointRole role, identity::IdentityRepository& identity_repository,
    const std::span<const std::uint8_t> alpn_protocol) {
  if (alpn_protocol.empty() || alpn_protocol.size() > 255U ||
      identity_repository.root_public_key() == nullptr) {
    return {.error = alpn_protocol.empty() || alpn_protocol.size() > 255U
                         ? SecurityError::kAlpnMismatch
                         : SecurityError::kIdentityUnavailable};
  }
  return CreateForAlpns(role, identity_repository,
                        {Bytes(alpn_protocol.begin(), alpn_protocol.end())}, false, {});
}

Result<OpenSslTlsContext> OpenSslTlsContext::CreateServerDispatcher(
    identity::IdentityRepository& identity_repository,
    PairingAlpnPolicy pairing_alpn_policy) {
  return CreateForAlpns(TlsEndpointRole::kServer, identity_repository,
                        {Bytes(kPairingAlpn.begin(), kPairingAlpn.end()),
                         Bytes(kEstablishedAlpn.begin(), kEstablishedAlpn.end())},
                        true, std::move(pairing_alpn_policy));
}

Result<OpenSslTlsContext> OpenSslTlsContext::CreateForAlpns(
    const TlsEndpointRole role, identity::IdentityRepository& identity_repository,
    std::vector<Bytes> alpn_protocols, const bool server_dispatcher,
    PairingAlpnPolicy pairing_alpn_policy) {
  if (alpn_protocols.empty() ||
      std::any_of(
          alpn_protocols.cbegin(), alpn_protocols.cend(),
          [](const Bytes& alpn) { return alpn.empty() || alpn.size() > 255U; }) ||
      (role == TlsEndpointRole::kClient && alpn_protocols.size() != 1U) ||
      server_dispatcher !=
          (role == TlsEndpointRole::kServer && alpn_protocols.size() == 2U) ||
      (server_dispatcher && !pairing_alpn_policy) ||
      identity_repository.root_public_key() == nullptr) {
    return {.error = SecurityError::kTlsConfigurationFailure};
  }
  const auto validated_root =
      ValidateEd25519PublicKey(*identity_repository.root_public_key());
  if (!validated_root.ok()) {
    return {.error = SecurityError::kIdentityUnavailable};
  }

  auto implementation = std::make_unique<Implementation>(
      role, std::move(alpn_protocols), server_dispatcher,
      std::move(pairing_alpn_policy));
  if (!implementation->context) {
    return {.error = SecurityError::kTlsConfigurationFailure};
  }
  implementation->server_alpn_policy = {
      .alpns = &implementation->alpns,
      .pairing_allowed = &implementation->pairing_alpn_policy,
      .enforce_pairing_window = implementation->server_dispatcher,
  };

  SecurityError configuration_error = SecurityError::kIdentityUnavailable;
  const identity::Result<void> identity_result = identity_repository.UseIdentitySeed(
      [&](const std::span<const std::uint8_t> seed) {
        KeyPointer private_key(nullptr, &EVP_PKEY_free);
        auto certificate = BuildIdentityCertificate(
            seed, *identity_repository.root_public_key(), private_key);
        if (!certificate.ok()) {
          configuration_error = certificate.error;
          return identity::Result<void>::Failure(identity::ErrorCode::kCryptoFailure);
        }
        configuration_error = ConfigureContext(
            implementation->context.get(), implementation->role, implementation->alpns,
            implementation->role == TlsEndpointRole::kServer
                ? &implementation->server_alpn_policy
                : nullptr,
            certificate.value->get(), private_key.get());
        return configuration_error == SecurityError::kNone
                   ? identity::Result<void>::Success()
                   : identity::Result<void>::Failure(
                         identity::ErrorCode::kCryptoFailure);
      });
  if (!identity_result.ok() || configuration_error != SecurityError::kNone) {
    return {.error = configuration_error};
  }

  return {
      .value = OpenSslTlsContext(std::move(implementation)),
      .error = SecurityError::kNone,
  };
}

OpenSslTlsContext::OpenSslTlsContext(std::unique_ptr<Implementation> implementation)
    : implementation_(std::move(implementation)) {}

OpenSslTlsContext::~OpenSslTlsContext() = default;

OpenSslTlsContext::OpenSslTlsContext(OpenSslTlsContext&&) noexcept = default;

OpenSslTlsContext& OpenSslTlsContext::operator=(OpenSslTlsContext&&) noexcept = default;

ssl_ctx_st* OpenSslTlsContext::native_handle() const noexcept {
  return implementation_ == nullptr ? nullptr : implementation_->context.get();
}

std::span<const std::uint8_t> OpenSslTlsContext::alpn_protocol() const noexcept {
  return implementation_ == nullptr || implementation_->alpns.size() != 1U
             ? std::span<const std::uint8_t>{}
             : std::span<const std::uint8_t>(implementation_->alpns.front());
}

bool OpenSslTlsContext::Owns(const VerifiedTlsConnection& connection,
                             ssl_st* const native_connection,
                             const std::span<const std::uint8_t> alpn) const noexcept {
  return implementation_ != nullptr && native_connection != nullptr &&
         connection.owner_ == implementation_.get() &&
         connection.connection_ == native_connection &&
         connection.alpn_.size() == alpn.size() &&
         CRYPTO_memcmp(connection.alpn_.data(), alpn.data(), alpn.size()) == 0 &&
         MatchesHandshake(connection);
}

bool OpenSslTlsContext::MatchesHandshake(
    const VerifiedTlsConnection& connection) const noexcept {
  const std::optional<HandshakeFinished> current =
      CaptureHandshakeFinished(connection.connection_);
  return current.has_value() &&
         current->local_size == connection.local_finished_size_ &&
         current->peer_size == connection.peer_finished_size_ &&
         CRYPTO_memcmp(current->local.data(), connection.local_finished_.data(),
                       current->local_size) == 0 &&
         CRYPTO_memcmp(current->peer.data(), connection.peer_finished_.data(),
                       current->peer_size) == 0;
}

Result<VerifiedTlsConnection> OpenSslTlsContext::VerifyPeer(
    ssl_st* const connection,
    const std::optional<identity::PublicKey> expected_pin) const {
  if (implementation_ == nullptr) {
    return {.error = SecurityError::kTlsConfigurationFailure};
  }
  if (implementation_->server_dispatcher) {
    return {.error = SecurityError::kAlpnMismatch};
  }
  if (std::equal(implementation_->alpns.front().cbegin(),
                 implementation_->alpns.front().cend(), kEstablishedAlpn.cbegin(),
                 kEstablishedAlpn.cend()) &&
      !expected_pin.has_value()) {
    return {.error = SecurityError::kPinMismatch};
  }
  const SecurityError parameter_error = ValidateNegotiatedParameters(
      connection, implementation_->context.get(), implementation_->alpns.front());
  if (parameter_error != SecurityError::kNone) {
    return {.error = parameter_error};
  }

  auto peer_public_key = ExtractValidatedPeerPublicKey(connection);
  if (!peer_public_key.ok()) {
    return {.error = peer_public_key.error};
  }
  if (expected_pin.has_value() &&
      !EqualPublicKeys(peer_public_key.value->bytes(), *expected_pin)) {
    return {.error = SecurityError::kPinMismatch};
  }
  const std::optional<HandshakeFinished> finished =
      CaptureHandshakeFinished(connection);
  if (!finished.has_value()) {
    return {.error = SecurityError::kHandshakeIncomplete};
  }

  return {
      .value = VerifiedTlsConnection(
          connection, implementation_.get(), *peer_public_key.value,
          implementation_->alpns.front(), finished->local,
          static_cast<std::uint8_t>(finished->local_size), finished->peer,
          static_cast<std::uint8_t>(finished->peer_size)),
      .error = SecurityError::kNone,
  };
}

Result<AcceptedServerTlsConnection> OpenSslTlsContext::AcceptServerPeer(
    ssl_st* const connection,
    const identity::IdentityRepository& identity_repository) const {
  if (implementation_ == nullptr || !implementation_->server_dispatcher ||
      implementation_->role != TlsEndpointRole::kServer ||
      !identity_repository.ready()) {
    return {.error = SecurityError::kTlsConfigurationFailure};
  }

  const unsigned char* selected_alpn = nullptr;
  unsigned int selected_alpn_size = 0;
  SSL_get0_alpn_selected(connection, &selected_alpn, &selected_alpn_size);
  const std::span<const std::uint8_t> negotiated(
      reinterpret_cast<const std::uint8_t*>(selected_alpn), selected_alpn_size);
  const bool is_pairing =
      negotiated.size() == kPairingAlpn.size() &&
      CRYPTO_memcmp(negotiated.data(), kPairingAlpn.data(), kPairingAlpn.size()) == 0;
  const bool is_established = negotiated.size() == kEstablishedAlpn.size() &&
                              CRYPTO_memcmp(negotiated.data(), kEstablishedAlpn.data(),
                                            kEstablishedAlpn.size()) == 0;
  if (!is_pairing && !is_established) {
    return {.error = SecurityError::kAlpnMismatch};
  }
  const SecurityError parameter_error = ValidateNegotiatedParameters(
      connection, implementation_->context.get(), negotiated);
  if (parameter_error != SecurityError::kNone) {
    return {.error = parameter_error};
  }

  auto peer_public_key = ExtractValidatedPeerPublicKey(connection);
  if (!peer_public_key.ok()) {
    return {.error = peer_public_key.error};
  }
  const std::optional<HandshakeFinished> finished =
      CaptureHandshakeFinished(connection);
  if (!finished.has_value()) {
    return {.error = SecurityError::kHandshakeIncomplete};
  }
  VerifiedTlsConnection verified(
      connection, implementation_.get(), *peer_public_key.value, negotiated,
      finished->local, static_cast<std::uint8_t>(finished->local_size), finished->peer,
      static_cast<std::uint8_t>(finished->peer_size));
  if (is_pairing) {
    const std::uint64_t pairing_window_generation = PairingWindowGeneration(connection);
    if (pairing_window_generation == 0U) {
      return {.error = SecurityError::kAlpnMismatch};
    }
    return {
        .value = AcceptedServerTlsConnection(AcceptedPairingTlsConnection(
            std::move(verified), pairing_window_generation)),
        .error = SecurityError::kNone,
    };
  }

  const identity::PeerRecord* resolved_peer = nullptr;
  for (const identity::PeerRecord& peer : identity_repository.peers()) {
    if (peer.trust_state != identity::TrustState::kActive ||
        !EqualPublicKeys(peer.public_key, peer_public_key.value->bytes())) {
      continue;
    }
    if (resolved_peer != nullptr) {
      return {.error = SecurityError::kPinMismatch};
    }
    resolved_peer = &peer;
  }
  if (resolved_peer == nullptr) {
    return {.error = SecurityError::kPinMismatch};
  }
  if (resolved_peer->security_profile == 0U ||
      resolved_peer->security_profile > kSecurityProfileV1) {
    return {.error = SecurityError::kUnsupportedProfile};
  }

  return {
      .value = AcceptedServerTlsConnection(AcceptedEstablishedTlsConnection(
          std::move(verified), resolved_peer->device_id,
          resolved_peer->security_profile, identity_repository.revision(),
          resolved_peer->record_revision)),
      .error = SecurityError::kNone,
  };
}

Result<identity::SecretBuffer> OpenSslTlsContext::ExportKeyingMaterial(
    const VerifiedTlsConnection& connection, const PairingExporterInput& input) const {
  return ExportKeyingMaterial(connection, ExporterLabel(PairingExporterLabel{}),
                              input.context);
}

Result<identity::SecretBuffer> OpenSslTlsContext::ExportKeyingMaterial(
    const VerifiedTlsConnection& connection,
    const ConfirmationExporterInput& input) const {
  return ExportKeyingMaterial(connection, ExporterLabel(ConfirmationExporterLabel{}),
                              input.context);
}

Result<identity::SecretBuffer> OpenSslTlsContext::ExportKeyingMaterial(
    const VerifiedTlsConnection& connection,
    const TransportExporterInput& input) const {
  return ExportKeyingMaterial(connection, ExporterLabel(TransportExporterLabel{}),
                              input.context);
}

Result<identity::SecretBuffer> OpenSslTlsContext::ExportKeyingMaterial(
    const VerifiedTlsConnection& connection, const std::string_view label,
    const std::span<const std::uint8_t> context) const {
  if (implementation_ == nullptr || connection.owner_ != implementation_.get() ||
      connection.connection_ == nullptr || context.size() != kSha256Size ||
      !MatchesHandshake(connection) ||
      ValidateNegotiatedParameters(connection.connection_,
                                   implementation_->context.get(),
                                   connection.alpn_) != SecurityError::kNone) {
    return {.error = SecurityError::kExporterFailure};
  }

  const auto current_peer_public_key =
      ExtractValidatedPeerPublicKey(connection.connection_);
  if (!current_peer_public_key.ok() ||
      !EqualPublicKeys(current_peer_public_key.value->bytes(),
                       connection.peer_public_key_.bytes())) {
    return {.error = SecurityError::kExporterFailure};
  }

  identity::SecretBuffer output(kSha256Size);
  if (SSL_export_keying_material(connection.connection_, output.mutable_bytes().data(),
                                 output.size(), label.data(), label.size(),
                                 context.data(), context.size(), 1) != 1) {
    return {.error = SecurityError::kExporterFailure};
  }
  return {
      .value = std::move(output),
      .error = SecurityError::kNone,
  };
}

}  // namespace xnn_transfer::core::security::tls
