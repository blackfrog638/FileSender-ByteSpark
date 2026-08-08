#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "xnn_transfer/core/security/identity/secret_buffer.hpp"
#include "xnn_transfer/core/security/tls/security_profile.hpp"

namespace {

using namespace xnn_transfer::core::security::tls;

struct DecodeCase {
  std::string_view id;
  std::string_view encoded;
  std::string_view error;
};

struct KeyCase {
  std::string_view id;
  std::string_view public_key;
  std::string_view error;
};

struct ContextCase {
  std::string_view id;
  std::string_view encoded;
  std::uint8_t expected_kind;
  std::string_view expected_digest;
  std::string_view error;
};

struct SasCase {
  std::string_view id;
  std::string_view exporter;
  std::string_view pair_context;
  std::string_view error;
};

struct ConfirmationCase {
  std::string_view id;
  std::string_view key;
  std::string_view message;
  std::string_view presented;
  std::string_view expected_context;
  std::uint8_t expected_role;
  std::string_view error;
};

struct TrustCase {
  std::string_view id;
  std::string_view key;
  std::string_view message;
  std::string_view presented;
  std::string_view expected_context;
  std::uint8_t expected_role;
  std::uint8_t local_decision;
  std::string_view error;
};

struct DeviceInputCase {
  std::string_view id;
  std::string_view canonical_input;
  std::string_view error;
};

struct DeviceVerifyCase {
  std::string_view id;
  std::string_view public_key;
  std::string_view public_key_identifier;
  std::string_view identifier_public_key;
  std::string_view expected_identifier;
  std::string_view error;
};

struct DeviceTextCase {
  std::string_view id;
  std::string_view public_key;
  std::string_view presented;
  std::string_view error;
};

struct FinishedCase {
  std::string_view id;
  std::string_view key;
  std::string_view message;
  std::string_view presented;
  std::uint8_t expected_role;
  std::string_view error;
};

struct RotationCounterCase {
  std::string_view id;
  std::uint64_t previous;
  std::uint64_t current;
  std::string_view error;
};

struct OutputCase {
  std::string_view id;
  std::string_view value;
  std::string_view expected_digest;
  std::string_view error;
};

#include "security_v1_golden_vectors.hpp"

static_assert(!std::is_same_v<PairingExporterInput::LabelType,
                              ConfirmationExporterInput::LabelType>);
static_assert(!std::is_same_v<PairingExporterInput::LabelType,
                              TransportExporterInput::LabelType>);
static_assert(!std::is_same_v<ConfirmationExporterInput::LabelType,
                              TransportExporterInput::LabelType>);
static_assert(!std::is_copy_constructible_v<
              xnn_transfer::core::security::identity::SecretBuffer>);
static_assert(
    !std::is_copy_assignable_v<xnn_transfer::core::security::identity::SecretBuffer>);
static_assert(std::is_nothrow_move_constructible_v<
              xnn_transfer::core::security::identity::SecretBuffer>);
static_assert(std::is_nothrow_move_assignable_v<
              xnn_transfer::core::security::identity::SecretBuffer>);
static_assert(!std::is_copy_constructible_v<SasWords>);
static_assert(!std::is_copy_assignable_v<SasWords>);
static_assert(std::is_nothrow_move_constructible_v<SasWords>);
static_assert(std::is_nothrow_move_assignable_v<SasWords>);
static_assert(golden::kDecodeCases.size() + golden::kKeyCases.size() +
                  golden::kContextCases.size() + golden::kSasCases.size() +
                  golden::kConfirmationCases.size() + golden::kTrustCases.size() +
                  golden::kDeviceInputCases.size() + golden::kDeviceVerifyCases.size() +
                  golden::kDeviceTextCases.size() + golden::kFinishedCases.size() +
                  golden::kRotationCounterCases.size() + golden::kOutputCases.size() ==
              37);

int failures = 0;

void Expect(const bool condition, const std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

Bytes DecodeHex(const std::string_view encoded) {
  auto nibble = [](const char value) -> int {
    if (value >= '0' && value <= '9') {
      return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
      return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
      return value - 'A' + 10;
    }
    return -1;
  };

  Bytes output;
  if ((encoded.size() & 1U) != 0U) {
    return output;
  }
  output.reserve(encoded.size() / 2);
  for (std::size_t offset = 0; offset < encoded.size(); offset += 2) {
    const int high = nibble(encoded[offset]);
    const int low = nibble(encoded[offset + 1]);
    if (high < 0 || low < 0) {
      return {};
    }
    output.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return output;
}

template <std::size_t Size>
std::array<std::uint8_t, Size> DecodeArray(const std::string_view encoded,
                                           const std::string_view name) {
  const Bytes bytes = DecodeHex(encoded);
  Expect(bytes.size() == Size, std::string(name) + " has the fixed size");
  std::array<std::uint8_t, Size> output{};
  std::copy_n(bytes.begin(), std::min(bytes.size(), output.size()), output.begin());
  return output;
}

void ExpectHex(const std::span<const std::uint8_t> actual,
               const std::string_view expected_hex,
               const std::string_view description) {
  const Bytes expected = DecodeHex(expected_hex);
  Expect(std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()),
         description);
}

void ExpectError(const SecurityError actual, const std::string_view expected,
                 const std::string_view vector_id) {
  if (SecurityErrorName(actual) == expected) {
    return;
  }
  std::cerr << "FAILED: " << vector_id << " expected " << expected << ", got "
            << SecurityErrorName(actual) << '\n';
  ++failures;
}

template <typename T>
void ExpectRejected(const Result<T>& result, const std::string_view expected,
                    const std::string_view vector_id) {
  if (result.ok()) {
    std::cerr << "FAILED: " << vector_id << " unexpectedly accepted\n";
    ++failures;
    return;
  }
  ExpectError(result.error, expected, vector_id);
}

struct Baseline {
  ValidatedEd25519PublicKey initiator_key;
  ValidatedEd25519PublicKey responder_key;
  ValidatedEd25519PublicKey rotation_old_key;
  ValidatedEd25519PublicKey rotation_new_key;
  NormalizedNegotiation negotiation;
  PairingContext pairing_context;
  TransportContext transport_context;
  RotationContext rotation_context;
};

std::optional<Baseline> MakeBaseline() {
  const Bytes initiator_bytes = DecodeHex(golden::kInitiatorKey);
  const Bytes responder_bytes = DecodeHex(golden::kResponderKey);
  const Bytes rotation_old_bytes = DecodeHex(golden::kRotationOldKey);
  const Bytes rotation_new_bytes = DecodeHex(golden::kRotationNewKey);
  const auto initiator = ValidateEd25519PublicKey(initiator_bytes);
  const auto responder = ValidateEd25519PublicKey(responder_bytes);
  const auto rotation_old = ValidateEd25519PublicKey(rotation_old_bytes);
  const auto rotation_new = ValidateEd25519PublicKey(rotation_new_bytes);
  const Bytes negotiation_bytes = DecodeHex(golden::kNormalizedNegotiation);
  const auto negotiation = DecodeNormalizedNegotiation(negotiation_bytes);
  Expect(initiator.ok(), "baseline initiator key validates");
  Expect(responder.ok(), "baseline responder key validates");
  Expect(rotation_old.ok(), "baseline rotation-old key validates");
  Expect(rotation_new.ok(), "baseline rotation-new key validates");
  Expect(negotiation.ok(), "baseline negotiation validates");
  if (!initiator.ok() || !responder.ok() || !rotation_old.ok() || !rotation_new.ok() ||
      !negotiation.ok()) {
    return std::nullopt;
  }

  const PairingContextInput pairing_input{
      .initiator_nonce = Nonce256{DecodeArray<32>(golden::kInitiatorPairingNonce,
                                                  "initiator pairing nonce")},
      .responder_nonce = Nonce256{DecodeArray<32>(golden::kResponderPairingNonce,
                                                  "responder pairing nonce")},
      .initiator_key = *initiator.value,
      .responder_key = *responder.value,
      .negotiation = *negotiation.value,
  };
  const auto pairing_context = BuildPairingContext(pairing_input);
  Expect(pairing_context.ok(), "baseline pairing context builds");
  if (!pairing_context.ok()) {
    return std::nullopt;
  }

  const TransportContextInput transport_input{
      .initiator_key = *initiator.value,
      .responder_key = *responder.value,
      .initiator_nonce = Nonce256{DecodeArray<32>(golden::kInitiatorTransportNonce,
                                                  "initiator transport nonce")},
      .responder_nonce = Nonce256{DecodeArray<32>(golden::kResponderTransportNonce,
                                                  "responder transport nonce")},
      .negotiation = *negotiation.value,
      .raw_negotiation_transcript = DecodeHex(golden::kRawNegotiationTranscript),
      .session_id = TransferSessionId{DecodeArray<16>(golden::kTransferSessionId,
                                                      "transfer session id")},
  };
  const auto transport_context = BuildTransportContext(transport_input);
  Expect(transport_context.ok(), "baseline transport context builds");
  if (!transport_context.ok()) {
    return std::nullopt;
  }

  const RotationContextInput rotation_input{
      .old_key = *rotation_old.value,
      .new_key = *rotation_new.value,
      .counter = golden::kRotationCounter,
      .nonce = Nonce256{DecodeArray<32>(golden::kRotationNonce, "rotation nonce")},
      .transport_context = *transport_context.value,
  };
  const auto rotation_context = BuildRotationContext(rotation_input);
  Expect(rotation_context.ok(), "baseline rotation context builds");
  if (!rotation_context.ok()) {
    return std::nullopt;
  }

  return Baseline{
      .initiator_key = *initiator.value,
      .responder_key = *responder.value,
      .rotation_old_key = *rotation_old.value,
      .rotation_new_key = *rotation_new.value,
      .negotiation = *negotiation.value,
      .pairing_context = *pairing_context.value,
      .transport_context = *transport_context.value,
      .rotation_context = *rotation_context.value,
  };
}

void TestCanonicalContexts(const Baseline& baseline) {
  ExpectHex(baseline.negotiation.encoded(), golden::kNormalizedNegotiation,
            "normalized negotiation bytes match");
  const auto negotiation_digest = Sha256(baseline.negotiation.encoded());
  Expect(negotiation_digest.ok(), "normalized negotiation hashes");
  if (negotiation_digest.ok()) {
    ExpectHex(*negotiation_digest.value, golden::kNormalizedNegotiationSha256,
              "normalized negotiation SHA-256 matches");
  }

  ExpectHex(baseline.pairing_context.encoded(), golden::kPairContext,
            "pairing context bytes match");
  ExpectHex(baseline.pairing_context.digest(), golden::kPairContextSha256,
            "pairing context SHA-256 matches");
  ExpectHex(baseline.transport_context.encoded(), golden::kTransportContext,
            "transport context bytes match");
  ExpectHex(baseline.transport_context.digest(), golden::kTransportContextSha256,
            "transport context SHA-256 matches");
  ExpectHex(baseline.rotation_context.encoded(), golden::kRotationContext,
            "rotation context bytes match");
  ExpectHex(baseline.rotation_context.digest(), golden::kRotationContextSha256,
            "rotation context SHA-256 matches");
}

void TestExporterInputs(const Baseline& baseline) {
  const PairingExporterInput pairing =
      MakePairingExporterInput(baseline.pairing_context);
  Expect(ExporterLabel(PairingExporterLabel{}) == golden::kPairingExporterLabel,
         "pairing exporter label is fixed");
  ExpectHex(pairing.context, golden::kPairingExporterContext,
            "pairing exporter context matches");
  Expect(pairing.kOutputLength == golden::kPairingExporterLength,
         "pairing exporter output length is fixed");

  const ConfirmationExporterInput confirmation =
      MakeConfirmationExporterInput(baseline.pairing_context);
  Expect(
      ExporterLabel(ConfirmationExporterLabel{}) == golden::kConfirmationExporterLabel,
      "confirmation exporter label is fixed");
  ExpectHex(confirmation.context, golden::kConfirmationExporterContext,
            "confirmation exporter context matches");
  Expect(confirmation.kOutputLength == golden::kConfirmationExporterLength,
         "confirmation exporter output length is fixed");

  const TransportExporterInput transport =
      MakeTransportExporterInput(baseline.transport_context);
  Expect(ExporterLabel(TransportExporterLabel{}) == golden::kTransportExporterLabel,
         "transport exporter label is fixed");
  ExpectHex(transport.context, golden::kTransportExporterContext,
            "transport exporter context matches");
  Expect(transport.kOutputLength == golden::kTransportExporterLength,
         "transport exporter output length is fixed");
}

void TestSas(const Baseline& baseline) {
  const Bytes exporter = DecodeHex(golden::kPairingExporter);
  const auto sas = DeriveSasWords(exporter, baseline.pairing_context);
  Expect(sas.ok(), "SAS derivation succeeds");
  if (!sas.ok()) {
    return;
  }
  ExpectHex(sas.value->hkdf_information.encoded, golden::kSasInformation,
            "SAS HKDF information matches");
  ExpectHex(sas.value->expanded.bytes(), golden::kSasExpanded,
            "SAS HKDF output matches");
  Expect(sas.value->indices == golden::kSasIndices, "SAS five 11-bit indices match");
  Expect(golden::kSasWords == std::array<std::string_view, 5>{"chest", "forward", "eye",
                                                              "dress", "rule"},
         "generated indices select accepted word-list entries");
}

void TestConfirmation(const Baseline& baseline, const Role sender,
                      const ConfirmationDecision decision,
                      const std::string_view expected_message,
                      const std::string_view expected_hmac) {
  const Bytes exporter = DecodeHex(golden::kConfirmationExporter);
  const auto value =
      BuildConfirmation(exporter, baseline.pairing_context, sender, decision);
  Expect(value.ok(), "confirmation value builds");
  if (!value.ok()) {
    return;
  }
  ExpectHex(value.value->message, expected_message, "confirmation message matches");
  ExpectHex(value.value->authenticator, expected_hmac, "confirmation HMAC matches");

  const auto verified =
      VerifyConfirmation(exporter, value.value->message, value.value->authenticator,
                         baseline.pairing_context, sender);
  Expect(verified.ok(), "generated confirmation verifies");
  if (!verified.ok()) {
    return;
  }
  if (decision == ConfirmationDecision::kReject) {
    Expect(verified.value->outcome == ConfirmationOutcome::kAuthenticatedReject &&
               verified.value->terminal && !verified.value->trust_commit_permitted,
           "authenticated rejection is terminal");
  } else {
    const auto committed = RequireTrustCommit(
        exporter, value.value->message, value.value->authenticator,
        baseline.pairing_context, sender, ConfirmationDecision::kConfirm);
    Expect(committed.ok() && committed.value->trust_commit_permitted,
           "affirmative decisions permit trust gate");
  }
}

void TestConfirmations(const Baseline& baseline) {
  TestConfirmation(baseline, Role::kInitiator, ConfirmationDecision::kConfirm,
                   golden::kConfirmationInitiatorMessage,
                   golden::kConfirmationInitiatorHmac);
  TestConfirmation(baseline, Role::kResponder, ConfirmationDecision::kConfirm,
                   golden::kConfirmationResponderMessage,
                   golden::kConfirmationResponderHmac);
  TestConfirmation(baseline, Role::kInitiator, ConfirmationDecision::kReject,
                   golden::kRejectionInitiatorMessage, golden::kRejectionInitiatorHmac);
  TestConfirmation(baseline, Role::kResponder, ConfirmationDecision::kReject,
                   golden::kRejectionResponderMessage, golden::kRejectionResponderHmac);
}

void TestDeviceIdentifier(const Baseline& baseline) {
  const auto identifier = DeriveDeviceIdentifier(baseline.initiator_key);
  Expect(identifier.ok(), "device identifier derives");
  if (!identifier.ok()) {
    return;
  }
  ExpectHex(identifier.value->input.encoded, golden::kDeviceIdentifierInput,
            "device identifier canonical input matches");
  ExpectHex(identifier.value->digest, golden::kDeviceIdentifierSha256,
            "device identifier SHA-256 matches");
  Expect(identifier.value->text == golden::kDeviceIdentifierSha256,
         "device identifier text is lowercase canonical hex");
  Expect(VerifyDeviceIdentifier(baseline.initiator_key, identifier.value->digest).ok(),
         "device identifier verifies for exact key");
  Expect(
      VerifyDeviceIdentifierText(baseline.initiator_key, identifier.value->text).ok(),
      "canonical device identifier text verifies");
}

void TestTransportFinished(const Baseline& baseline, const Role sender,
                           const std::string_view expected_message,
                           const std::string_view expected_hmac) {
  const Bytes exporter = DecodeHex(golden::kTransportExporter);
  const auto value =
      BuildTransportFinished(exporter, baseline.transport_context, sender);
  Expect(value.ok(), "transport finished builds");
  if (!value.ok()) {
    return;
  }
  ExpectHex(value.value->message, expected_message,
            "transport finished message matches");
  ExpectHex(value.value->authenticator, expected_hmac,
            "transport finished HMAC matches");
  Expect(VerifyTransportFinished(exporter, value.value->message,
                                 value.value->authenticator, baseline.transport_context,
                                 sender)
             .ok(),
         "generated transport finished verifies");
}

void TestTransportFinishedValues(const Baseline& baseline) {
  TestTransportFinished(baseline, Role::kInitiator, golden::kFinishedInitiatorMessage,
                        golden::kFinishedInitiatorHmac);
  TestTransportFinished(baseline, Role::kResponder, golden::kFinishedResponderMessage,
                        golden::kFinishedResponderHmac);
}

void TestRotationProofs(const Baseline& baseline) {
  const auto old_proof =
      BuildRotationProofInput(baseline.rotation_context, RotationSigner::kOldKey);
  const auto new_proof =
      BuildRotationProofInput(baseline.rotation_context, RotationSigner::kNewKey);
  Expect(old_proof.ok(), "old-key rotation proof builds");
  Expect(new_proof.ok(), "new-key rotation proof builds");
  if (old_proof.ok()) {
    ExpectHex(old_proof.value->encoded, golden::kRotationProofOldMessage,
              "old-key rotation proof bytes match");
    const auto digest = Sha256(old_proof.value->encoded);
    Expect(digest.ok(), "old-key rotation proof hashes");
    if (digest.ok()) {
      ExpectHex(*digest.value, golden::kRotationProofOldSha256,
                "old-key rotation proof SHA-256 matches");
    }
  }
  if (new_proof.ok()) {
    ExpectHex(new_proof.value->encoded, golden::kRotationProofNewMessage,
              "new-key rotation proof bytes match");
    const auto digest = Sha256(new_proof.value->encoded);
    Expect(digest.ok(), "new-key rotation proof hashes");
    if (digest.ok()) {
      ExpectHex(*digest.value, golden::kRotationProofNewSha256,
                "new-key rotation proof SHA-256 matches");
    }
  }
}

void TestNegativeCanonicalAndKeyVectors() {
  for (const DecodeCase& test : golden::kDecodeCases) {
    const Bytes encoded = DecodeHex(test.encoded);
    ExpectRejected(DecodeCanonicalObject(encoded), test.error, test.id);
  }
  for (const KeyCase& test : golden::kKeyCases) {
    const Bytes public_key = DecodeHex(test.public_key);
    ExpectRejected(ValidateEd25519PublicKey(public_key), test.error, test.id);
  }
  for (const ContextCase& test : golden::kContextCases) {
    const Bytes encoded = DecodeHex(test.encoded);
    const Bytes digest = DecodeHex(test.expected_digest);
    const auto kind = static_cast<CanonicalObjectKind>(test.expected_kind);
    ExpectRejected(VerifyCanonicalDigest(encoded, kind, digest), test.error, test.id);
  }
}

void TestNegativeConfirmationVectors(const Baseline& baseline) {
  for (const SasCase& test : golden::kSasCases) {
    ExpectHex(baseline.pairing_context.digest(), test.pair_context,
              "negative SAS uses live pair context");
    const Bytes exporter = DecodeHex(test.exporter);
    ExpectRejected(DeriveSasWords(exporter, baseline.pairing_context), test.error,
                   test.id);
  }
  for (const ConfirmationCase& test : golden::kConfirmationCases) {
    ExpectHex(baseline.pairing_context.digest(), test.expected_context,
              "negative confirmation uses live context");
    const Bytes key = DecodeHex(test.key);
    const Bytes message = DecodeHex(test.message);
    const Bytes presented = DecodeHex(test.presented);
    ExpectRejected(VerifyConfirmation(key, message, presented, baseline.pairing_context,
                                      static_cast<Role>(test.expected_role)),
                   test.error, test.id);
  }
  for (const TrustCase& test : golden::kTrustCases) {
    ExpectHex(baseline.pairing_context.digest(), test.expected_context,
              "negative trust uses live pair context");
    const Bytes key = DecodeHex(test.key);
    const Bytes message = DecodeHex(test.message);
    const Bytes presented = DecodeHex(test.presented);
    ExpectRejected(
        RequireTrustCommit(key, message, presented, baseline.pairing_context,
                           static_cast<Role>(test.expected_role),
                           static_cast<ConfirmationDecision>(test.local_decision)),
        test.error, test.id);
  }
}

void TestNegativeDeviceVectors() {
  for (const DeviceInputCase& test : golden::kDeviceInputCases) {
    const Bytes input = DecodeHex(test.canonical_input);
    ExpectRejected(DeriveDeviceIdentifier(input), test.error, test.id);
  }
  for (const DeviceVerifyCase& test : golden::kDeviceVerifyCases) {
    const Bytes public_key_bytes = DecodeHex(test.public_key);
    const auto public_key = ValidateEd25519PublicKey(public_key_bytes);
    if (!public_key.ok()) {
      ExpectError(public_key.error, test.error, test.id);
      continue;
    }

    const Bytes identifier_key_bytes = DecodeHex(test.identifier_public_key);
    const auto identifier_key = ValidateEd25519PublicKey(identifier_key_bytes);
    Expect(identifier_key.ok(), std::string(test.id) + " identifier key validates");
    if (!identifier_key.ok()) {
      continue;
    }
    const Bytes expected = DecodeHex(test.expected_identifier);
    Expect(VerifyDeviceIdentifier(*identifier_key.value, expected).ok(),
           std::string(test.id) + " expected identifier matches fixture key");
    if (!test.public_key_identifier.empty()) {
      const Bytes own_identifier = DecodeHex(test.public_key_identifier);
      Expect(VerifyDeviceIdentifier(*public_key.value, own_identifier).ok(),
             std::string(test.id) + " public-key identifier matches");
    }
    ExpectRejected(VerifyDeviceIdentifier(*public_key.value, expected), test.error,
                   test.id);
  }
  for (const DeviceTextCase& test : golden::kDeviceTextCases) {
    const Bytes key_bytes = DecodeHex(test.public_key);
    const auto key = ValidateEd25519PublicKey(key_bytes);
    Expect(key.ok(), std::string(test.id) + " public key validates");
    if (key.ok()) {
      ExpectRejected(VerifyDeviceIdentifierText(*key.value, test.presented), test.error,
                     test.id);
    }
  }
}

void TestNegativeFinishedAndRotationVectors(const Baseline& baseline) {
  for (const FinishedCase& test : golden::kFinishedCases) {
    const Bytes key = DecodeHex(test.key);
    const Bytes message = DecodeHex(test.message);
    const Bytes presented = DecodeHex(test.presented);
    ExpectRejected(
        VerifyTransportFinished(key, message, presented, baseline.transport_context,
                                static_cast<Role>(test.expected_role)),
        test.error, test.id);
  }
  for (const RotationCounterCase& test : golden::kRotationCounterCases) {
    ExpectError(ValidateRotationCounter(test.previous, test.current), test.error,
                test.id);
  }
  for (const OutputCase& test : golden::kOutputCases) {
    const Bytes value = DecodeHex(test.value);
    const Bytes expected = DecodeHex(test.expected_digest);
    ExpectRejected(VerifySha256(value, expected), test.error, test.id);
  }
}

void TestSecretBufferMoveAndClear() {
  using xnn_transfer::core::security::identity::SecretBuffer;
  const Bytes secret = {0x10U, 0x20U, 0x30U, 0x40U, 0x50U};
  SecretBuffer source(secret);

  SecretBuffer moved(std::move(source));
  Expect(source.empty(), "secret buffer clears moved-from value");
  Expect(std::equal(moved.bytes().begin(), moved.bytes().end(), secret.begin(),
                    secret.end()),
         "secret buffer preserves move construction value");

  const Bytes overwritten = {0xa5U, 0xa5U, 0xa5U};
  SecretBuffer assigned(overwritten);
  assigned = std::move(moved);
  Expect(moved.empty(), "secret buffer clears move-assignment source");
  Expect(std::equal(assigned.bytes().begin(), assigned.bytes().end(), secret.begin(),
                    secret.end()),
         "secret buffer preserves move-assignment value");

  assigned.clear();
  Expect(assigned.empty(), "secret buffer supports explicit cleansing");
  assigned.clear();
  Expect(assigned.empty(), "secret buffer cleansing is idempotent");
}

}  // namespace

int main() {
  const std::optional<Baseline> baseline = MakeBaseline();
  if (!baseline.has_value()) {
    std::cerr << "Unable to construct accepted security-profile baseline\n";
    return 1;
  }

  TestCanonicalContexts(*baseline);
  TestExporterInputs(*baseline);
  TestSas(*baseline);
  TestConfirmations(*baseline);
  TestDeviceIdentifier(*baseline);
  TestTransportFinishedValues(*baseline);
  TestRotationProofs(*baseline);
  TestNegativeCanonicalAndKeyVectors();
  TestNegativeConfirmationVectors(*baseline);
  TestNegativeDeviceVectors();
  TestNegativeFinishedAndRotationVectors(*baseline);
  TestSecretBufferMoveAndClear();

  if (failures != 0) {
    std::cerr << failures << " security-profile golden test(s) failed\n";
    return 1;
  }
  std::cout << "Validated 17 positive and 37 negative native "
               "security-profile vectors\n";
  return 0;
}
