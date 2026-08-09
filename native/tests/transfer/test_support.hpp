#ifndef XNN_TRANSFER_TESTS_TRANSFER_TEST_SUPPORT_HPP_
#define XNN_TRANSFER_TESTS_TRANSFER_TEST_SUPPORT_HPP_

#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../session/test_support.hpp"
#include "pairing_vectors.hpp"
#include "xnn_transfer/core/transfer/transfer.hpp"

namespace transfer_test {

namespace identity = xnn_transfer::core::security::identity;
namespace session = xnn_transfer::core::session;
namespace storage = xnn_transfer::core::storage;
namespace tls = xnn_transfer::core::security::tls;
namespace transfer = xnn_transfer::core::transfer;
namespace protocol = xnn_transfer::protocol::v1;

using session_test::CounterEntropy;
using session_test::DecodeArray;
using session_test::DecodeHex;
using session_test::IdentityFixture;

inline constexpr std::string_view kInitiatorSeed =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
inline constexpr std::string_view kResponderSeed =
    "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
inline constexpr std::array<std::uint8_t, 14> kEstablishedAlpn = {
    'x', 'n', 'n', '-', 't', 'r', 'a', 'n', 's', 'f', 'e', 'r', '/', '1',
};

using SslPointer = std::unique_ptr<SSL, decltype(&SSL_free)>;

struct Handshake {
  SslPointer client{nullptr, &SSL_free};
  SslPointer server{nullptr, &SSL_free};
};

inline bool CompleteHandshake(SSL* const client, SSL* const server) {
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

inline std::optional<Handshake> Connect(SSL_CTX* const client_context,
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

class SessionFixture final {
 public:
  SessionFixture()
      : initiator_(DecodeArray<32>(kInitiatorSeed)),
        responder_(DecodeArray<32>(kResponderSeed)),
        initiator_authority_(initiator_.repository, initiator_entropy_),
        responder_authority_(responder_.repository, responder_entropy_) {}

  [[nodiscard]] bool Open() {
    if (!initiator_.repository.Open().ok() || !responder_.repository.Open().ok()) {
      return false;
    }
    const auto responder_commit = initiator_.repository.CommitPeer(identity::PeerCommit{
        .public_key = *responder_.repository.root_public_key(),
        .security_profile = tls::kSecurityProfileV1,
        .display_label = "responder",
    });
    const auto initiator_commit = responder_.repository.CommitPeer(identity::PeerCommit{
        .public_key = *initiator_.repository.root_public_key(),
        .security_profile = tls::kSecurityProfileV1,
        .display_label = "initiator",
    });
    if (!responder_commit.ok() || !initiator_commit.ok()) {
      return false;
    }
    responder_device_id_ = responder_commit.value();
    initiator_device_id_ = initiator_commit.value();

    auto client_context = tls::OpenSslTlsContext::Create(
        tls::TlsEndpointRole::kClient, initiator_.repository, kEstablishedAlpn);
    auto server_context = tls::OpenSslTlsContext::Create(
        tls::TlsEndpointRole::kServer, responder_.repository, kEstablishedAlpn);
    if (!client_context.ok() || !server_context.ok()) {
      return false;
    }
    client_context_.emplace(std::move(*client_context.value));
    server_context_.emplace(std::move(*server_context.value));
    handshake_ =
        Connect(client_context_->native_handle(), server_context_->native_handle());
    if (!handshake_.has_value()) {
      return false;
    }

    auto client_channel = session::EstablishedTlsChannel::Create(
        *client_context_, handshake_->client.get(), initiator_.repository,
        *responder_device_id_);
    auto server_channel = session::EstablishedTlsChannel::Create(
        *server_context_, handshake_->server.get(), responder_.repository,
        *initiator_device_id_);
    const std::optional<tls::TransportContext> transport_context =
        BuildTransportContext();
    if (!client_channel.ok() || !server_channel.ok() ||
        !transport_context.has_value()) {
      return false;
    }

    const auto client_finished = client_channel.value->get()->BeginTransportBinding(
        *transport_context, tls::Role::kInitiator);
    const auto server_finished = server_channel.value->get()->BeginTransportBinding(
        *transport_context, tls::Role::kResponder);
    if (!client_finished.ok() || !server_finished.ok() ||
        client_channel.value->get()->LocalTransportFinishedWritten() !=
            tls::SecurityError::kNone ||
        server_channel.value->get()->LocalTransportFinishedWritten() !=
            tls::SecurityError::kNone ||
        client_channel.value->get()->VerifyPeerTransportFinished(
            server_finished.value->message, server_finished.value->authenticator) !=
            tls::SecurityError::kNone ||
        server_channel.value->get()->VerifyPeerTransportFinished(
            client_finished.value->message, client_finished.value->authenticator) !=
            tls::SecurityError::kNone) {
      return false;
    }
    const session::AuthorizationResult initiator_active =
        initiator_authority_.Activate(std::move(*client_channel.value));
    const session::AuthorizationResult responder_active =
        responder_authority_.Activate(std::move(*server_channel.value));
    if (!initiator_active.ok() || !responder_active.ok()) {
      return false;
    }
    initiator_handle_ = *initiator_active.handle;
    responder_handle_ = *responder_active.handle;
    return true;
  }

  [[nodiscard]] transfer::TransferContext InitiatorContext(
      const std::uint32_t stream_id) {
    return {
        .authority = &initiator_authority_,
        .session_handle = *initiator_handle_,
        .local_role = tls::Role::kInitiator,
        .stream_id = stream_id,
    };
  }

  [[nodiscard]] transfer::TransferContext ResponderContext(
      const std::uint32_t stream_id) {
    return {
        .authority = &responder_authority_,
        .session_handle = *responder_handle_,
        .local_role = tls::Role::kResponder,
        .stream_id = stream_id,
    };
  }

  [[nodiscard]] transfer::ConnectionCreditBudget& InitiatorBudget() noexcept {
    return initiator_budget_;
  }

  [[nodiscard]] transfer::ConnectionCreditBudget& ResponderBudget() noexcept {
    return responder_budget_;
  }

  void DeactivateResponder() {
    if (responder_handle_.has_value()) {
      static_cast<void>(responder_authority_.Deactivate(*responder_handle_));
    }
  }

 private:
  [[nodiscard]] std::optional<tls::TransportContext> BuildTransportContext() {
    const auto initiator_key =
        tls::ValidateEd25519PublicKey(*initiator_.repository.root_public_key());
    const auto responder_key =
        tls::ValidateEd25519PublicKey(*responder_.repository.root_public_key());
    const auto negotiation = tls::DecodeNormalizedNegotiation(
        DecodeHex(pairing_vectors::kNormalizedNegotiation));
    if (!initiator_key.ok() || !responder_key.ok() || !negotiation.ok()) {
      return std::nullopt;
    }
    const auto context = tls::BuildTransportContext(tls::TransportContextInput{
        .initiator_key = *initiator_key.value,
        .responder_key = *responder_key.value,
        .initiator_nonce =
            tls::Nonce256{DecodeArray<32>(pairing_vectors::kInitiatorTransportNonce)},
        .responder_nonce =
            tls::Nonce256{DecodeArray<32>(pairing_vectors::kResponderTransportNonce)},
        .negotiation = *negotiation.value,
        .raw_negotiation_transcript =
            DecodeHex(pairing_vectors::kRawNegotiationTranscript),
        .session_id = tls::TransferSessionId{DecodeArray<16>(
            pairing_vectors::kTransportSessionId)},
    });
    return context.ok() ? std::optional<tls::TransportContext>(*context.value)
                        : std::nullopt;
  }

  IdentityFixture initiator_;
  IdentityFixture responder_;
  CounterEntropy initiator_entropy_;
  CounterEntropy responder_entropy_;
  session::SessionAuthority initiator_authority_;
  session::SessionAuthority responder_authority_;
  transfer::ConnectionCreditBudget initiator_budget_{
      transfer::kMaximumConnectionWindow};
  transfer::ConnectionCreditBudget responder_budget_{
      transfer::kMaximumConnectionWindow};
  std::optional<identity::DeviceId> responder_device_id_;
  std::optional<identity::DeviceId> initiator_device_id_;
  std::optional<tls::OpenSslTlsContext> client_context_;
  std::optional<tls::OpenSslTlsContext> server_context_;
  std::optional<Handshake> handshake_;
  std::optional<session::SessionHandle> initiator_handle_;
  std::optional<session::SessionHandle> responder_handle_;
};

using MessageIds = transfer::ConnectionMessageSequence;

class MemorySource final : public transfer::FileSource {
 public:
  explicit MemorySource(transfer::Bytes bytes) : bytes_(std::move(bytes)) {}

  [[nodiscard]] std::uint64_t size() const noexcept override { return bytes_.size(); }

  [[nodiscard]] transfer::FileReadResult Read(
      const std::uint64_t offset, const std::size_t maximum_bytes) override {
    if (offset > bytes_.size() || maximum_bytes > bytes_.size() - offset) {
      return {.error = transfer::FileSourceError::kOutOfRange};
    }
    const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    return {
        .data =
            transfer::Bytes(begin, begin + static_cast<std::ptrdiff_t>(maximum_bytes)),
    };
  }

 private:
  transfer::Bytes bytes_;
};

inline transfer::Bytes Digest(const std::span<const std::uint8_t> bytes,
                              const std::uint64_t domain = 0xcbf29ce484222325ULL) {
  std::uint64_t value = domain;
  for (const std::uint8_t byte : bytes) {
    value ^= byte;
    value *= 0x100000001b3ULL;
  }
  transfer::Bytes output(32);
  for (std::size_t index = 0; index < output.size(); ++index) {
    value ^= value >> 12U;
    value ^= value << 25U;
    value ^= value >> 27U;
    output[index] = static_cast<std::uint8_t>((value * 0x2545f4914f6cdd1dULL) >> 56U);
  }
  return output;
}

inline void AppendU64(transfer::Bytes& output, const std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    const std::size_t shift = (7U - index) * 8U;
    output.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
  }
}

inline transfer::Bytes ManifestCommitment(const transfer::OneFileManifest& manifest) {
  transfer::Bytes input(
      reinterpret_cast<const std::uint8_t*>(manifest.relative_path.data()),
      reinterpret_cast<const std::uint8_t*>(manifest.relative_path.data()) +
          manifest.relative_path.size());
  AppendU64(input, manifest.file_size);
  input.insert(input.end(), manifest.file_commitment.begin(),
               manifest.file_commitment.end());
  return Digest(input, 0x4d414e4946455354ULL);
}

inline transfer::Bytes ChunkCommitment(const transfer::TransferId& transfer_id,
                                       const std::uint64_t offset,
                                       const std::span<const std::uint8_t> data) {
  transfer::Bytes input(transfer_id.begin(), transfer_id.end());
  AppendU64(input, offset);
  input.insert(input.end(), data.begin(), data.end());
  return Digest(input, 0x4348554e4b000001ULL);
}

struct VerifierState {
  transfer::Bytes expected{};
  transfer::Bytes observed{};
  bool sealed{};
};

class TestFileVerifier final : public storage::StreamingIntegrityVerifier {
 public:
  explicit TestFileVerifier(std::shared_ptr<VerifierState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] bool Update(const std::span<const std::uint8_t> data) override {
    state_->observed.insert(state_->observed.end(), data.begin(), data.end());
    return true;
  }

  [[nodiscard]] bool Seal() override {
    state_->sealed = true;
    return Digest(state_->observed, 0x46494c4500000001ULL) == state_->expected;
  }

 private:
  std::shared_ptr<VerifierState> state_;
};

class TestIntegrityProvider final : public transfer::TransferIntegrityProvider {
 public:
  [[nodiscard]] bool VerifyManifest(
      const transfer::ManifestVerificationInput& input) override {
    return input.manifest != nullptr && !input.offer_frame.empty() &&
           !input.entry_frame.empty() && !input.end_frame.empty() &&
           ManifestCommitment(*input.manifest) == input.manifest->manifest_commitment;
  }

  [[nodiscard]] bool BuildChunkCommitment(const transfer::TransferId& transfer_id,
                                          const std::uint32_t entry_index,
                                          const std::uint64_t offset,
                                          const std::span<const std::uint8_t> data,
                                          transfer::Bytes& output) override {
    if (entry_index != 0) {
      return false;
    }
    output = ChunkCommitment(transfer_id, offset, data);
    return true;
  }

  [[nodiscard]] bool VerifyChunkCommitment(
      const transfer::TransferId& transfer_id, const std::uint32_t entry_index,
      const std::uint64_t offset, const std::span<const std::uint8_t> data,
      const std::span<const std::uint8_t> commitment) override {
    return entry_index == 0 &&
           ChunkCommitment(transfer_id, offset, data) ==
               transfer::Bytes(commitment.begin(), commitment.end());
  }

  [[nodiscard]] std::unique_ptr<storage::StreamingIntegrityVerifier> CreateFileVerifier(
      const transfer::TransferId&, const std::uint32_t entry_index,
      const std::span<const std::uint8_t> expected_commitment) override {
    if (entry_index != 0) {
      return nullptr;
    }
    last_verifier = std::make_shared<VerifierState>();
    last_verifier->expected.assign(expected_commitment.begin(),
                                   expected_commitment.end());
    return std::make_unique<TestFileVerifier>(last_verifier);
  }

  std::shared_ptr<VerifierState> last_verifier{};
};

class MemoryPlatform final : public storage::PlatformBackend {
 public:
  [[nodiscard]] storage::PlatformResult CreateTemporary(
      const storage::ValidatedReceivePath& path, const std::uint64_t declared_size,
      storage::TemporaryFileHandle& output) override {
    ++create_calls;
    created_path = path.utf8();
    created_size = declared_size;
    output = {.value = ++next_handle};
    open = true;
    temporary.clear();
    return {.error = create_error};
  }

  [[nodiscard]] storage::PlatformWriteResult WriteTemporary(
      const storage::TemporaryFileHandle,
      const std::span<const std::uint8_t> bytes) override {
    ++write_calls;
    temporary.insert(temporary.end(), bytes.begin(), bytes.end());
    return {
        .error = write_error,
        .bytes_written =
            write_error == storage::PlatformError::kNone ? bytes.size() : 0,
    };
  }

  [[nodiscard]] storage::PlatformResult FlushTemporary(
      const storage::TemporaryFileHandle) override {
    ++flush_calls;
    return {.error = flush_error};
  }

  [[nodiscard]] storage::PlatformCommitResult CommitTemporary(
      const storage::TemporaryFileHandle,
      const storage::ValidatedReceivePath& path) override {
    ++commit_calls;
    committed_path = path.utf8();
    observed_sealed_before_commit = verifier_state != nullptr && verifier_state->sealed;
    if (commit_disposition == storage::PlatformCommitDisposition::kCommitted) {
      destination = temporary;
      open = false;
    }
    return {
        .disposition = commit_disposition,
        .error = commit_error,
    };
  }

  [[nodiscard]] storage::PlatformResult CleanupTemporary(
      const storage::TemporaryFileHandle) override {
    ++cleanup_calls;
    open = false;
    temporary.clear();
    return {.error = cleanup_error};
  }

  std::shared_ptr<VerifierState> verifier_state{};
  storage::PlatformError create_error{storage::PlatformError::kNone};
  storage::PlatformError write_error{storage::PlatformError::kNone};
  storage::PlatformError flush_error{storage::PlatformError::kNone};
  storage::PlatformError commit_error{storage::PlatformError::kNone};
  storage::PlatformError cleanup_error{storage::PlatformError::kNone};
  storage::PlatformCommitDisposition commit_disposition{
      storage::PlatformCommitDisposition::kCommitted};
  std::uint64_t next_handle{};
  std::size_t create_calls{};
  std::size_t write_calls{};
  std::size_t flush_calls{};
  std::size_t commit_calls{};
  std::size_t cleanup_calls{};
  std::uint64_t created_size{};
  bool open{};
  bool observed_sealed_before_commit{};
  std::string created_path{};
  std::string committed_path{};
  transfer::Bytes temporary{};
  transfer::Bytes destination{};
};

inline transfer::TransferId TransferId(const std::uint8_t seed) {
  transfer::TransferId output{};
  for (std::size_t index = 0; index < output.size(); ++index) {
    output[index] = static_cast<std::uint8_t>(seed + index);
  }
  return output;
}

inline transfer::OneFileManifest Manifest(const std::uint8_t id_seed,
                                          const std::string_view path,
                                          const std::span<const std::uint8_t> data) {
  transfer::OneFileManifest manifest{
      .transfer_id = TransferId(id_seed),
      .relative_path = std::string(path),
      .file_size = data.size(),
      .file_commitment = Digest(data, 0x46494c4500000001ULL),
      .display_name = "one file",
  };
  manifest.manifest_commitment = ManifestCommitment(manifest);
  return manifest;
}

}  // namespace transfer_test

#endif  // XNN_TRANSFER_TESTS_TRANSFER_TEST_SUPPORT_HPP_
