#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "pairing_vectors.hpp"
#include "test_support.hpp"
#include "xnn_transfer/core/session/session.hpp"

namespace {

using namespace session_test;

constexpr std::string_view kInitiatorSeed =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
constexpr std::string_view kResponderSeed =
    "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
constexpr std::uint64_t kStartMs = 1'000;

int failures = 0;

void Expect(const bool condition, const std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

void ExpectError(const session::PairingUpdate& update,
                 const session::PairingError error, const std::string_view message) {
  Expect(update.terminal && update.error == error, message);
}

void ExpectBytes(const session::Bytes& actual, const std::string_view expected_hex,
                 const std::string_view message) {
  const session::Bytes expected = DecodeHex(expected_hex);
  if (actual == expected) {
    return;
  }
  std::cerr << "FAILED: " << message << " (actual " << actual.size()
            << " bytes, expected " << expected.size() << " bytes";
  const std::size_t compared = std::min(actual.size(), expected.size());
  for (std::size_t index = 0; index < compared; ++index) {
    if (actual[index] != expected[index]) {
      std::cerr << ", first mismatch at " << index << ": "
                << static_cast<unsigned>(actual[index])
                << " != " << static_cast<unsigned>(expected[index]);
      break;
    }
  }
  std::cerr << ")\n";
  ++failures;
}

std::unique_ptr<session::PairingAttempt> CreateAttempt(
    IdentityFixture& local, const identity::PublicKey& peer_key, const tls::Role role,
    const session::NegotiationOffer& offer, session::PairingReplayCache& replay_cache,
    const std::uint8_t handle_start, const std::uint8_t nonce_start) {
  const auto validated_peer = tls::ValidateEd25519PublicKey(peer_key);
  Expect(validated_peer.ok(), "test peer key validates");
  if (!validated_peer.ok()) {
    return nullptr;
  }
  const identity::PublicKey* const local_key = local.repository.root_public_key();
  Expect(local_key != nullptr, "test local key is available");
  if (local_key == nullptr) {
    return nullptr;
  }
  auto admission = AdmitForTest(*local_key, peer_key, role, handle_start, kStartMs);
  Expect(admission != nullptr, "test pairing attempt is admitted");
  if (admission == nullptr) {
    return nullptr;
  }
  auto channel = std::make_unique<FakePairingChannel>(*validated_peer.value);
  FixedAttemptEntropy entropy(handle_start, nonce_start);
  std::unique_ptr<session::PairingAttempt> attempt;
  const session::PairingUpdate created = session::PairingAttempt::Create(
      local.repository, std::move(channel), entropy, std::move(admission), replay_cache,
      session::PairingAttemptOptions{
          .offer = offer,
          .peer_display_label = "paired peer",
      },
      attempt);
  Expect(created.error == session::PairingError::kNone && !created.terminal &&
             attempt != nullptr,
         "pairing attempt is created");
  return attempt;
}

struct PromptedInitiator {
  IdentityFixture identity{DecodeArray<32>(kInitiatorSeed)};
  session::PairingReplayCache replay{};
  std::unique_ptr<session::PairingAttempt> attempt{};
  session::AttemptHandle handle{};

  bool Open() {
    Expect(identity.repository.Open().ok(), "prompted initiator identity opens");
    const identity::PublicKey responder =
        DecodeArray<32>(pairing_vectors::kResponderKey);
    attempt = CreateAttempt(identity, responder, tls::Role::kInitiator,
                            InitiatorOffer(), replay, 0xa0U, 0x00U);
    if (!attempt) {
      return false;
    }
    const session::PairingUpdate start = attempt->Start(kStartMs);
    ExpectBytes(start.outbound_frame, pairing_vectors::kIHello,
                "initiator emits the registered HELLO");
    const session::PairingUpdate hello =
        attempt->ReceiveFrame(DecodeHex(pairing_vectors::kRHello), kStartMs + 1);
    Expect(hello.outbound_frame == DecodeHex(pairing_vectors::kISelect),
           "initiator emits deterministic SELECT");
    const session::PairingUpdate ack =
        attempt->ReceiveFrame(DecodeHex(pairing_vectors::kRSelectAck), kStartMs + 2);
    Expect(ack.prompt.has_value() &&
               ack.state == session::PairingState::kAwaitingDecisions,
           "matching ACK exposes one native prompt");
    if (!ack.prompt.has_value()) {
      return false;
    }
    handle = ack.prompt->handle;
    const std::optional<session::Digest256> context = attempt->pair_context();
    Expect(context.has_value() &&
               *context == DecodeArray<32>(pairing_vectors::kPairContext),
           "pairing context matches the normative vector");
    return true;
  }
};

void TestGoldenTwoSidedSuccess() {
  IdentityFixture initiator_identity{DecodeArray<32>(kInitiatorSeed)};
  IdentityFixture responder_identity{DecodeArray<32>(kResponderSeed)};
  Expect(initiator_identity.repository.Open().ok(), "initiator identity opens");
  Expect(responder_identity.repository.Open().ok(), "responder identity opens");

  session::PairingReplayCache initiator_replay;
  session::PairingReplayCache responder_replay;
  auto initiator = CreateAttempt(
      initiator_identity, *responder_identity.repository.root_public_key(),
      tls::Role::kInitiator, InitiatorOffer(), initiator_replay, 0xa0U, 0x00U);
  auto responder = CreateAttempt(
      responder_identity, *initiator_identity.repository.root_public_key(),
      tls::Role::kResponder, ResponderOffer(), responder_replay, 0xb0U, 0x20U);
  if (!initiator || !responder) {
    return;
  }

  const session::PairingUpdate initiator_start = initiator->Start(kStartMs);
  const session::PairingUpdate responder_start = responder->Start(kStartMs);
  ExpectBytes(initiator_start.outbound_frame, pairing_vectors::kIHello,
              "initiator HELLO is byte exact");
  ExpectBytes(responder_start.outbound_frame, pairing_vectors::kRHello,
              "responder HELLO is byte exact");

  const session::PairingUpdate responder_hello =
      responder->ReceiveFrame(initiator_start.outbound_frame, kStartMs + 1);
  Expect(responder_hello.state == session::PairingState::kSelecting &&
             responder_hello.outbound_frame.empty(),
         "responder waits for initiator selection");
  const session::PairingUpdate initiator_hello =
      initiator->ReceiveFrame(responder_start.outbound_frame, kStartMs + 1);
  Expect(initiator_hello.outbound_frame == DecodeHex(pairing_vectors::kISelect),
         "initiator SELECT is byte exact");

  const session::PairingUpdate responder_select =
      responder->ReceiveFrame(initiator_hello.outbound_frame, kStartMs + 2);
  Expect(responder_select.outbound_frame == DecodeHex(pairing_vectors::kRSelectAck),
         "responder SELECT_ACK is byte exact");
  Expect(!responder_select.prompt.has_value() &&
             responder_select.state == session::PairingState::kSelecting,
         "responder withholds SAS until SELECT_ACK write completion");
  const session::PairingUpdate responder_prompt =
      responder->LocalSelectionAckWritten(responder->handle(), kStartMs + 3);
  Expect(responder_prompt.prompt.has_value(),
         "responder exposes SAS after SELECT_ACK write completion");
  const session::PairingUpdate initiator_ack =
      initiator->ReceiveFrame(responder_select.outbound_frame, kStartMs + 3);
  Expect(initiator_ack.prompt.has_value(),
         "initiator exposes SAS only after matching ACK");
  if (!responder_prompt.prompt || !initiator_ack.prompt) {
    return;
  }
  Expect(responder_prompt.prompt->sas_word_indices ==
             initiator_ack.prompt->sas_word_indices,
         "both endpoints derive identical five-word indices");

  const session::AttemptHandle initiator_handle = initiator_ack.prompt->handle;
  const session::AttemptHandle responder_handle = responder_prompt.prompt->handle;
  const session::PairingUpdate initiator_decision = initiator->Decide(
      initiator_handle, tls::ConfirmationDecision::kConfirm, kStartMs + 3);
  const session::PairingUpdate responder_decision = responder->Decide(
      responder_handle, tls::ConfirmationDecision::kConfirm, kStartMs + 3);
  Expect(initiator_decision.outbound_frame == DecodeHex(pairing_vectors::kIConfirm),
         "initiator confirmation is byte exact");
  Expect(responder_decision.outbound_frame == DecodeHex(pairing_vectors::kRConfirm),
         "responder confirmation is byte exact");

  Expect(!initiator->LocalDecisionWritten(initiator_handle, kStartMs + 4).terminal,
         "local write alone cannot commit initiator trust");
  Expect(!responder->LocalDecisionWritten(responder_handle, kStartMs + 4).terminal,
         "local write alone cannot commit responder trust");
  const session::PairingUpdate responder_paired =
      responder->ReceiveFrame(initiator_decision.outbound_frame, kStartMs + 5);
  const session::PairingUpdate initiator_paired =
      initiator->ReceiveFrame(responder_decision.outbound_frame, kStartMs + 5);
  Expect(responder_paired.state == session::PairingState::kPairedLocal &&
             responder_paired.paired_peer.has_value(),
         "responder commits after both confirmations");
  Expect(initiator_paired.state == session::PairingState::kPairedLocal &&
             initiator_paired.paired_peer.has_value(),
         "initiator commits after both confirmations");
  Expect(initiator_identity.repository.peers().size() == 1 &&
             responder_identity.repository.peers().size() == 1,
         "each endpoint persists exactly one active peer");
}

void TestPeerConfirmationFirstAndCommitFailure() {
  PromptedInitiator side;
  if (!side.Open()) {
    return;
  }
  const session::PairingUpdate peer_first =
      side.attempt->ReceiveFrame(DecodeHex(pairing_vectors::kRConfirm), kStartMs + 3);
  Expect(!peer_first.terminal && side.identity.repository.peers().empty(),
         "peer confirmation cannot suppress local ceremony");
  const session::PairingUpdate local = side.attempt->Decide(
      side.handle, tls::ConfirmationDecision::kConfirm, kStartMs + 4);
  Expect(!local.terminal && side.identity.repository.peers().empty(),
         "decision generation is not the trust linearization point");
  const session::PairingUpdate committed =
      side.attempt->LocalDecisionWritten(side.handle, kStartMs + 5);
  Expect(committed.state == session::PairingState::kPairedLocal,
         "successful local decision write permits atomic commit");

  PromptedInitiator failing;
  if (!failing.Open()) {
    return;
  }
  failing.identity.store.FailNextPut();
  static_cast<void>(failing.attempt->ReceiveFrame(DecodeHex(pairing_vectors::kRConfirm),
                                                  kStartMs + 3));
  static_cast<void>(failing.attempt->Decide(
      failing.handle, tls::ConfirmationDecision::kConfirm, kStartMs + 4));
  const session::PairingUpdate failure =
      failing.attempt->LocalDecisionWritten(failing.handle, kStartMs + 5);
  ExpectError(failure, session::PairingError::kInternalFailure,
              "protected-store failure closes unpaired");
  Expect(failing.identity.repository.peers().empty(),
         "failed commit exposes no partial trust");
}

void TestCommitSerializesCancellation() {
  PromptedInitiator side;
  if (!side.Open()) {
    return;
  }
  static_cast<void>(
      side.attempt->ReceiveFrame(DecodeHex(pairing_vectors::kRConfirm), kStartMs + 3));
  static_cast<void>(side.attempt->Decide(
      side.handle, tls::ConfirmationDecision::kConfirm, kStartMs + 4));
  side.identity.store.BlockNextPut();

  session::PairingUpdate committed;
  session::PairingUpdate cancelled;
  std::thread commit_thread([&] {
    committed = side.attempt->LocalDecisionWritten(side.handle, kStartMs + 5);
  });
  const bool commit_blocked = side.identity.store.WaitForBlockedPut();
  std::thread cancel_thread(
      [&] { cancelled = side.attempt->Cancel(side.handle, kStartMs + 6); });
  side.identity.store.ReleaseBlockedPut();
  commit_thread.join();
  cancel_thread.join();

  Expect(commit_blocked && committed.state == session::PairingState::kPairedLocal &&
             cancelled.state == session::PairingState::kPairedLocal &&
             cancelled.terminal && side.identity.repository.peers().size() == 1,
         "commit and cancellation events serialize at the trust write boundary");
}

void TestRejectionCancellationAndDecisionIdempotency() {
  PromptedInitiator rejected;
  if (!rejected.Open()) {
    return;
  }
  const session::PairingUpdate peer_reject = rejected.attempt->ReceiveFrame(
      DecodeHex(pairing_vectors::kRReject), kStartMs + 3);
  ExpectError(peer_reject, session::PairingError::kAuthenticatedReject,
              "authenticated peer rejection is terminal");
  Expect(rejected.identity.repository.peers().empty(),
         "peer rejection never writes trust");

  PromptedInitiator local_reject;
  if (!local_reject.Open()) {
    return;
  }
  const session::PairingUpdate rejected_locally = local_reject.attempt->Decide(
      local_reject.handle, tls::ConfirmationDecision::kReject, kStartMs + 3);
  ExpectError(rejected_locally, session::PairingError::kLocalReject,
              "local SAS mismatch rejects the attempt");
  Expect(local_reject.identity.repository.peers().empty(),
         "local rejection never writes trust");

  PromptedInitiator cancelled;
  if (!cancelled.Open()) {
    return;
  }
  static_cast<void>(cancelled.attempt->ReceiveFrame(
      DecodeHex(pairing_vectors::kRConfirm), kStartMs + 3));
  const session::PairingUpdate first_decision = cancelled.attempt->Decide(
      cancelled.handle, tls::ConfirmationDecision::kConfirm, kStartMs + 4);
  const session::PairingUpdate duplicate = cancelled.attempt->Decide(
      cancelled.handle, tls::ConfirmationDecision::kConfirm, kStartMs + 4);
  const session::PairingUpdate conflict = cancelled.attempt->Decide(
      cancelled.handle, tls::ConfirmationDecision::kReject, kStartMs + 4);
  Expect(!first_decision.outbound_frame.empty() && duplicate.outbound_frame.empty() &&
             duplicate.error == session::PairingError::kNone,
         "identical local decision is idempotent without a second frame");
  Expect(!conflict.terminal && conflict.error == session::PairingError::kAlreadyDecided,
         "conflicting local decision cannot change the first");
  const session::PairingUpdate cancel =
      cancelled.attempt->Cancel(cancelled.handle, kStartMs + 5);
  ExpectError(cancel, session::PairingError::kCancelled,
              "cancellation wins before commit invocation");
  Expect(cancelled.identity.repository.peers().empty(),
         "pre-commit cancellation leaves no trust");
}

void TestHostileGoldenFrames() {
  struct HostileCase {
    std::string_view frame;
    session::PairingError error;
    std::string_view description;
  };
  const std::array<HostileCase, 7> responder_cases = {{
      {pairing_vectors::kIHelloDuplicateField, session::PairingError::kMalformed,
       "duplicate HELLO field"},
      {pairing_vectors::kIHelloMissingField, session::PairingError::kMalformed,
       "missing HELLO field"},
      {pairing_vectors::kIHelloReorderedFields, session::PairingError::kMalformed,
       "reordered HELLO field"},
      {pairing_vectors::kIHelloUnknownField, session::PairingError::kMalformed,
       "unknown HELLO field"},
      {pairing_vectors::kIHelloTrailingOctet, session::PairingError::kMalformed,
       "trailing HELLO octet"},
      {pairing_vectors::kIHelloUnknownProfile,
       session::PairingError::kUnsupportedProfile, "unknown profile"},
      {pairing_vectors::kOversizedDeclaration, session::PairingError::kLimitExceeded,
       "oversized body declaration"},
  }};
  for (const HostileCase& test : responder_cases) {
    IdentityFixture responder{DecodeArray<32>(kResponderSeed)};
    Expect(responder.repository.Open().ok(), "hostile responder identity opens");
    session::PairingReplayCache replay;
    auto attempt =
        CreateAttempt(responder, DecodeArray<32>(pairing_vectors::kInitiatorKey),
                      tls::Role::kResponder, ResponderOffer(), replay, 0xb0U, 0x20U);
    if (!attempt) {
      continue;
    }
    static_cast<void>(attempt->Start(kStartMs));
    const session::PairingUpdate update =
        attempt->ReceiveFrame(DecodeHex(test.frame), kStartMs + 1);
    ExpectError(update, test.error, test.description);
  }

  IdentityFixture malformed_identity{DecodeArray<32>(kResponderSeed)};
  Expect(malformed_identity.repository.Open().ok(), "malformed-frame identity opens");
  session::PairingReplayCache malformed_replay;
  auto malformed = CreateAttempt(
      malformed_identity, DecodeArray<32>(pairing_vectors::kInitiatorKey),
      tls::Role::kResponder, ResponderOffer(), malformed_replay, 0xb0U, 0x20U);
  if (malformed) {
    static_cast<void>(malformed->Start(kStartMs));
    ExpectError(malformed->ReceiveFrame(DecodeHex(pairing_vectors::kMalformedMagic),
                                        kStartMs + 1),
                session::PairingError::kMalformed, "malformed pairing magic");
  }

  IdentityFixture role_identity{DecodeArray<32>(kInitiatorSeed)};
  Expect(role_identity.repository.Open().ok(), "role identity opens");
  session::PairingReplayCache role_replay;
  auto role =
      CreateAttempt(role_identity, DecodeArray<32>(pairing_vectors::kResponderKey),
                    tls::Role::kInitiator, InitiatorOffer(), role_replay, 0xa0U, 0x00U);
  if (role) {
    static_cast<void>(role->Start(kStartMs));
    ExpectError(role->ReceiveFrame(DecodeHex(pairing_vectors::kRHelloRoleSwapped),
                                   kStartMs + 1),
                session::PairingError::kRoleMismatch,
                "responder cannot claim initiator role");
  }

  IdentityFixture sequence_identity{DecodeArray<32>(kResponderSeed)};
  Expect(sequence_identity.repository.Open().ok(), "sequence identity opens");
  session::PairingReplayCache sequence_replay;
  auto sequence = CreateAttempt(
      sequence_identity, DecodeArray<32>(pairing_vectors::kInitiatorKey),
      tls::Role::kResponder, ResponderOffer(), sequence_replay, 0xb0U, 0x20U);
  if (sequence) {
    static_cast<void>(sequence->Start(kStartMs));
    static_cast<void>(
        sequence->ReceiveFrame(DecodeHex(pairing_vectors::kIHello), kStartMs + 1));
    ExpectError(sequence->ReceiveFrame(
                    DecodeHex(pairing_vectors::kIHelloDuplicateSequence), kStartMs + 2),
                session::PairingError::kSequenceViolation,
                "duplicate frame sequence is terminal");
  }

  IdentityFixture semantic_identity{DecodeArray<32>(kResponderSeed)};
  Expect(semantic_identity.repository.Open().ok(), "semantic duplicate identity opens");
  session::PairingReplayCache semantic_replay;
  auto semantic = CreateAttempt(
      semantic_identity, DecodeArray<32>(pairing_vectors::kInitiatorKey),
      tls::Role::kResponder, ResponderOffer(), semantic_replay, 0xb0U, 0x20U);
  if (semantic) {
    static_cast<void>(semantic->Start(kStartMs));
    static_cast<void>(
        semantic->ReceiveFrame(DecodeHex(pairing_vectors::kIHello), kStartMs + 1));
    ExpectError(semantic->ReceiveFrame(
                    DecodeHex(pairing_vectors::kIHelloSemanticDuplicate), kStartMs + 2),
                session::PairingError::kStateViolation,
                "semantic duplicate HELLO is terminal");
  }

  IdentityFixture downgrade_identity{DecodeArray<32>(kResponderSeed)};
  Expect(downgrade_identity.repository.Open().ok(), "downgrade identity opens");
  session::PairingReplayCache downgrade_replay;
  auto downgrade = CreateAttempt(
      downgrade_identity, DecodeArray<32>(pairing_vectors::kInitiatorKey),
      tls::Role::kResponder, ResponderOffer(), downgrade_replay, 0xb0U, 0x20U);
  if (downgrade) {
    static_cast<void>(downgrade->Start(kStartMs));
    static_cast<void>(
        downgrade->ReceiveFrame(DecodeHex(pairing_vectors::kIHello), kStartMs + 1));
    ExpectError(downgrade->ReceiveFrame(DecodeHex(pairing_vectors::kISelectDowngrade),
                                        kStartMs + 2),
                session::PairingError::kDowngradeDetected,
                "lower common version is rejected as downgrade");
  }

  IdentityFixture limits_identity{DecodeArray<32>(kResponderSeed)};
  Expect(limits_identity.repository.Open().ok(), "receive-limit identity opens");
  session::PairingReplayCache limits_replay;
  auto limits = CreateAttempt(
      limits_identity, DecodeArray<32>(pairing_vectors::kInitiatorKey),
      tls::Role::kResponder, ResponderOffer(), limits_replay, 0xb0U, 0x20U);
  if (limits) {
    static_cast<void>(limits->Start(kStartMs));
    session::Bytes below_minimum = DecodeHex(pairing_vectors::kIHello);
    below_minimum[below_minimum.size() - 10] = 0;
    below_minimum[below_minimum.size() - 9] = 0;
    below_minimum[below_minimum.size() - 8] = 0x1fU;
    below_minimum[below_minimum.size() - 7] = 0xffU;
    ExpectError(limits->ReceiveFrame(below_minimum, kStartMs + 1),
                session::PairingError::kMalformed,
                "receive limits below the v1 minimum are rejected");
  }
}

void TestConfirmationReplayTimeoutAndHandles() {
  PromptedInitiator confirmation;
  if (!confirmation.Open()) {
    return;
  }
  session::Bytes tampered = DecodeHex(pairing_vectors::kRConfirm);
  tampered.back() ^= 0x80U;
  ExpectError(confirmation.attempt->ReceiveFrame(tampered, kStartMs + 3),
              session::PairingError::kConfirmationFailed,
              "wrong confirmation fails closed");

  PromptedInitiator stale_handle;
  if (!stale_handle.Open()) {
    return;
  }
  session::AttemptHandle wrong = stale_handle.handle;
  wrong[0] ^= 0x80U;
  const session::PairingUpdate stale = stale_handle.attempt->Decide(
      wrong, tls::ConfirmationDecision::kConfirm, kStartMs + 3);
  Expect(!stale.terminal && stale.error == session::PairingError::kStateViolation &&
             stale_handle.identity.repository.peers().empty(),
         "stale opaque handle has no effect on the live attempt");
  const session::PairingUpdate stale_write =
      stale_handle.attempt->LocalDecisionWritten(wrong, kStartMs + 4);
  Expect(!stale_write.terminal &&
             stale_write.error == session::PairingError::kStateViolation &&
             stale_handle.attempt->state() == session::PairingState::kAwaitingDecisions,
         "stale decision-write callback cannot close the live attempt");

  IdentityFixture responder_identity{DecodeArray<32>(kResponderSeed)};
  Expect(responder_identity.repository.Open().ok(),
         "selection callback responder identity opens");
  session::PairingReplayCache responder_replay;
  auto responder = CreateAttempt(
      responder_identity, DecodeArray<32>(pairing_vectors::kInitiatorKey),
      tls::Role::kResponder, ResponderOffer(), responder_replay, 0xb0U, 0x20U);
  if (responder) {
    static_cast<void>(responder->Start(kStartMs));
    static_cast<void>(
        responder->ReceiveFrame(DecodeHex(pairing_vectors::kIHello), kStartMs + 1));
    const session::PairingUpdate selection =
        responder->ReceiveFrame(DecodeHex(pairing_vectors::kISelect), kStartMs + 2);
    session::AttemptHandle stale_selection_handle = responder->handle();
    stale_selection_handle[0] ^= 0x80U;
    const session::PairingUpdate stale_selection =
        responder->LocalSelectionAckWritten(stale_selection_handle, kStartMs + 3);
    Expect(!selection.outbound_frame.empty() && !selection.prompt.has_value() &&
               !stale_selection.terminal &&
               stale_selection.error == session::PairingError::kStateViolation &&
               responder->state() == session::PairingState::kSelecting,
           "stale SELECT_ACK write callback has no effect");
    const session::PairingUpdate visible =
        responder->LocalSelectionAckWritten(responder->handle(), kStartMs + 4);
    Expect(visible.prompt.has_value(),
           "matching SELECT_ACK write callback exposes the responder prompt");
  }

  IdentityFixture timeout_identity{DecodeArray<32>(kInitiatorSeed)};
  Expect(timeout_identity.repository.Open().ok(), "timeout identity opens");
  session::PairingReplayCache timeout_replay;
  auto timeout = CreateAttempt(
      timeout_identity, DecodeArray<32>(pairing_vectors::kResponderKey),
      tls::Role::kInitiator, InitiatorOffer(), timeout_replay, 0xa0U, 0x00U);
  if (timeout) {
    static_cast<void>(timeout->Start(kStartMs));
    ExpectError(timeout->Advance(kStartMs + session::kFirstPairingFrameTimeoutMs),
                session::PairingError::kTimeout,
                "first-frame absolute timeout closes the attempt");
    Expect(timeout_identity.repository.peers().empty(),
           "timeout leaves durable trust unchanged");
  }
}

void TestReplayCacheAndShutdown() {
  IdentityFixture identity_fixture{DecodeArray<32>(kInitiatorSeed)};
  Expect(identity_fixture.repository.Open().ok(), "replay identity opens");
  session::PairingReplayCache replay;
  auto first =
      CreateAttempt(identity_fixture, DecodeArray<32>(pairing_vectors::kResponderKey),
                    tls::Role::kInitiator, InitiatorOffer(), replay, 0xa0U, 0x00U);
  if (!first) {
    return;
  }
  static_cast<void>(first->Start(kStartMs));
  static_cast<void>(
      first->ReceiveFrame(DecodeHex(pairing_vectors::kRHello), kStartMs + 1));
  const session::PairingUpdate prompt =
      first->ReceiveFrame(DecodeHex(pairing_vectors::kRSelectAck), kStartMs + 2);
  Expect(prompt.prompt.has_value(), "first replay attempt reaches prompt");
  if (!prompt.prompt) {
    return;
  }
  static_cast<void>(first->Cancel(prompt.prompt->handle, kStartMs + 3));
  Expect(!first->pair_context().has_value(),
         "terminal attempt clears its exposed pairing context");

  auto replayed =
      CreateAttempt(identity_fixture, DecodeArray<32>(pairing_vectors::kResponderKey),
                    tls::Role::kInitiator, InitiatorOffer(), replay, 0xc0U, 0x00U);
  if (replayed) {
    static_cast<void>(replayed->Start(kStartMs + 4));
    static_cast<void>(
        replayed->ReceiveFrame(DecodeHex(pairing_vectors::kRHello), kStartMs + 5));
    ExpectError(
        replayed->ReceiveFrame(DecodeHex(pairing_vectors::kRSelectAck), kStartMs + 6),
        session::PairingError::kReplayDetected,
        "terminal pair context cannot be replayed");
  }

  session::PairingReplayCache bounded;
  const identity::PublicKey peer = DecodeArray<32>(pairing_vectors::kResponderKey);
  for (std::size_t index = 0; index < session::kMaxReplayEntriesPerPeer; ++index) {
    session::Digest256 context{};
    context[0] = static_cast<std::uint8_t>(index + 1);
    Expect(bounded.Remember(context, peer, 0),
           "per-peer replay entry fits within the bound");
  }
  session::Digest256 overflow{};
  overflow[0] = 0xffU;
  Expect(!bounded.Remember(overflow, peer, 0),
         "unexpired per-peer replay entries are never evicted");
  Expect(bounded.Remember(overflow, peer, session::kReplayRetentionMs),
         "expired replay entries are pruned before admission");

  session::PairingReplayCache fixed_expiry;
  session::Digest256 fixed_context{};
  fixed_context[0] = 1;
  Expect(
      fixed_expiry.Remember(fixed_context, peer, 0) &&
          fixed_expiry.Remember(fixed_context, peer, session::kReplayRetentionMs - 1) &&
          !fixed_expiry.Contains(fixed_context, peer, session::kReplayRetentionMs),
      "replayed terminals cannot extend the fixed tombstone lifetime");

  session::PairingReplayCache reserved_capacity;
  for (std::size_t index = 0; index < session::kMaxReplayEntries - 1; ++index) {
    session::Digest256 context{};
    context[0] = static_cast<std::uint8_t>(index >> 8U);
    context[1] = static_cast<std::uint8_t>(index);
    identity::PublicKey distinct_peer{};
    distinct_peer[0] = static_cast<std::uint8_t>(index >> 8U);
    distinct_peer[1] = static_cast<std::uint8_t>(index);
    Expect(reserved_capacity.Remember(context, distinct_peer, 0),
           "global replay fixture fills without eviction");
  }
  identity::PublicKey reserved_peer{};
  reserved_peer.fill(0xa5U);
  auto reservation = reserved_capacity.Reserve(reserved_peer, 0);
  Expect(
      reservation != nullptr && reserved_capacity.Reserve(reserved_peer, 0) == nullptr,
      "live replay reservation consumes the final global slot");
  session::Digest256 reserved_context{};
  reserved_context.fill(0x5aU);
  if (reservation != nullptr) {
    Expect(reservation->Bind(reserved_context, 1) == session::PairingError::kNone,
           "reserved replay slot binds without terminal allocation");
    reservation->Commit(2);
    Expect(reserved_capacity.Contains(reserved_context, reserved_peer, 2) &&
               reserved_capacity.size() == session::kMaxReplayEntries,
           "terminal commit retains the preallocated replay tombstone");
  }

  PromptedInitiator shutdown;
  if (shutdown.Open()) {
    ExpectError(shutdown.attempt->Shutdown(), session::PairingError::kCancelled,
                "shutdown closes the live attempt exactly once");
    Expect(shutdown.attempt->Shutdown().terminal, "repeated shutdown is idempotent");
  }
}

void TestConcurrentAdmissionAndReplayState() {
  session::PairingAdmissionController admission;
  Expect(admission.OpenWindow(0, session::kMaximumPairingWindowMs),
         "concurrent admission window opens");
  const identity::PublicKey local = DecodeArray<32>(kInitiatorSeed);
  const identity::PublicKey base_peer = DecodeArray<32>(pairing_vectors::kResponderKey);
  std::array<session::PairingAdmissionResult, 16> results;
  std::array<std::thread, 16> admission_threads;
  for (std::size_t index = 0; index < admission_threads.size(); ++index) {
    admission_threads[index] = std::thread([&, index] {
      identity::PublicKey peer = base_peer;
      peer[0] ^= static_cast<std::uint8_t>(index + 1);
      session::AttemptHandle connection_id{};
      session::SourceToken source{};
      connection_id.fill(static_cast<std::uint8_t>(index + 1));
      source.fill(static_cast<std::uint8_t>(index + 1));
      results[index] = admission.Admit(session::PairingAdmissionRequest{
          .connection_id = connection_id,
          .source = source,
          .local_key = local,
          .peer_key = peer,
          .local_role = tls::Role::kInitiator,
          .user_initiated = true,
          .now_ms = 0,
      });
    });
  }
  for (std::thread& thread : admission_threads) {
    thread.join();
  }
  const std::size_t accepted = static_cast<std::size_t>(std::count_if(
      results.begin(), results.end(),
      [](const session::PairingAdmissionResult& result) { return result.accepted(); }));
  Expect(accepted == session::kMaxIncompletePairingHandshakes &&
             admission.active_connections() == session::kMaxIncompletePairingHandshakes,
         "concurrent admission preserves the process-wide hard bound");

  session::PairingReplayCache replay;
  const identity::PublicKey replay_peer =
      DecodeArray<32>(pairing_vectors::kResponderKey);
  std::unique_ptr<session::PairingReplayReservation> first;
  std::unique_ptr<session::PairingReplayReservation> second;
  std::thread first_reserve([&] { first = replay.Reserve(replay_peer, 0); });
  std::thread second_reserve([&] { second = replay.Reserve(replay_peer, 0); });
  first_reserve.join();
  second_reserve.join();
  if (first == nullptr || second == nullptr) {
    Expect(false, "concurrent replay reservations allocate");
    return;
  }
  session::Digest256 context{};
  context.fill(0xa5U);
  session::PairingError first_error = session::PairingError::kInternalFailure;
  session::PairingError second_error = session::PairingError::kInternalFailure;
  std::thread first_bind([&] { first_error = first->Bind(context, 1); });
  std::thread second_bind([&] { second_error = second->Bind(context, 1); });
  first_bind.join();
  second_bind.join();
  const bool one_bound = (first_error == session::PairingError::kNone &&
                          second_error == session::PairingError::kReplayDetected) ||
                         (second_error == session::PairingError::kNone &&
                          first_error == session::PairingError::kReplayDetected);
  Expect(one_bound, "concurrent replay binding accepts exactly one duplicate context");

  session::PairingReplayCache commit_race_cache;
  auto commit_race = commit_race_cache.Reserve(replay_peer, 0);
  if (commit_race == nullptr) {
    Expect(false, "commit-race replay reservation allocates");
    return;
  }
  session::PairingError bind_race_error = session::PairingError::kInternalFailure;
  std::thread racing_bind([&] { bind_race_error = commit_race->Bind(context, 1); });
  std::thread racing_commit([&] { commit_race->Commit(1); });
  racing_bind.join();
  racing_commit.join();
  Expect((bind_race_error == session::PairingError::kNone ||
          bind_race_error == session::PairingError::kStateViolation) &&
             !commit_race->active(),
         "Bind and Commit serialize on one replay reservation");
}

void TestAttemptRequiresLiveAdmission() {
  IdentityFixture local{DecodeArray<32>(kInitiatorSeed)};
  Expect(local.repository.Open().ok(), "admission-required identity opens");
  const identity::PublicKey peer = DecodeArray<32>(pairing_vectors::kResponderKey);
  const auto validated_peer = tls::ValidateEd25519PublicKey(peer);
  if (!validated_peer.ok()) {
    Expect(false, "admission-required peer key validates");
    return;
  }
  FixedAttemptEntropy entropy(0xa0U, 0x00U);
  session::PairingReplayCache replay;
  std::unique_ptr<session::PairingAttempt> attempt;
  const session::PairingUpdate bypass = session::PairingAttempt::Create(
      local.repository, std::make_unique<FakePairingChannel>(*validated_peer.value),
      entropy, nullptr, replay,
      session::PairingAttemptOptions{
          .offer = InitiatorOffer(),
          .peer_display_label = "bypass",
      },
      attempt);
  ExpectError(bypass, session::PairingError::kBusy,
              "pairing attempt cannot bypass admission");

  const identity::PublicKey* const local_key = local.repository.root_public_key();
  if (local_key == nullptr) {
    return;
  }
  session::PairingAdmissionController controller;
  Expect(controller.OpenWindow(kStartMs, session::kMaximumPairingWindowMs),
         "admission-required window opens");
  session::AttemptHandle connection{};
  connection.fill(0x42U);
  session::SourceToken source{};
  source.fill(0x42U);
  session::PairingAdmissionResult admitted =
      controller.Admit(session::PairingAdmissionRequest{
          .connection_id = connection,
          .source = source,
          .local_key = *local_key,
          .peer_key = peer,
          .local_role = tls::Role::kInitiator,
          .user_initiated = true,
          .now_ms = kStartMs,
      });
  controller.CloseWindow();
  FixedAttemptEntropy closed_entropy(0xa0U, 0x00U);
  const session::PairingUpdate closed = session::PairingAttempt::Create(
      local.repository, std::make_unique<FakePairingChannel>(*validated_peer.value),
      closed_entropy, std::move(admitted.lease), replay,
      session::PairingAttemptOptions{
          .offer = InitiatorOffer(),
          .peer_display_label = "closed window",
      },
      attempt);
  ExpectError(closed, session::PairingError::kBusy,
              "closing the local window invalidates unused admission leases");
}

void TestSameIdentityRejectedBeforePrompt() {
  IdentityFixture local{DecodeArray<32>(kInitiatorSeed)};
  Expect(local.repository.Open().ok(), "same-key identity opens");
  const identity::PublicKey* const local_key = local.repository.root_public_key();
  if (local_key == nullptr) {
    Expect(false, "same-key local identity is available");
    return;
  }
  session::AttemptHandle connection{};
  connection.fill(0xa0U);
  session::SourceToken source{};
  source.fill(1);
  session::PairingAdmissionController admission;
  Expect(admission.OpenWindow(kStartMs, session::kMaximumPairingWindowMs),
         "same-key pairing window opens");
  const session::PairingAdmissionResult rejected =
      admission.Admit(session::PairingAdmissionRequest{
          .connection_id = connection,
          .source = source,
          .local_key = *local_key,
          .peer_key = *local_key,
          .local_role = tls::Role::kInitiator,
          .user_initiated = true,
          .now_ms = kStartMs,
      });
  Expect(!rejected.accepted() &&
             rejected.error == session::PairingError::kCertificateRejected,
         "equal local and remote identities fail before SAS");
}

session::AttemptHandle Handle(const std::uint8_t value) {
  session::AttemptHandle handle{};
  handle.fill(value);
  return handle;
}

session::SourceToken Source(const std::uint8_t value) {
  session::SourceToken source{};
  source.fill(value);
  return source;
}

void TestAdmissionBoundsAndDuplicateWinner() {
  const identity::PublicKey local = DecodeArray<32>(pairing_vectors::kInitiatorKey);
  const identity::PublicKey peer = DecodeArray<32>(pairing_vectors::kResponderKey);

  session::PairingAdmissionController duplicates;
  Expect(!duplicates
              .Admit(session::PairingAdmissionRequest{
                  .connection_id = Handle(1),
                  .source = Source(1),
                  .local_key = local,
                  .peer_key = peer,
                  .local_role = tls::Role::kInitiator,
                  .now_ms = 0,
              })
              .accepted(),
         "closed pairing window rejects admission");
  Expect(!duplicates.OpenWindow(0, session::kMaximumPairingWindowMs + 1),
         "pairing window cannot exceed 120 seconds");
  Expect(duplicates.OpenWindow(0, session::kMaximumPairingWindowMs),
         "explicit bounded pairing window opens");
  session::PairingAdmissionResult first =
      duplicates.Admit(session::PairingAdmissionRequest{
          .connection_id = Handle(1),
          .source = Source(1),
          .local_key = local,
          .peer_key = peer,
          .local_role = tls::Role::kInitiator,
          .user_initiated = true,
          .now_ms = 0,
      });
  Expect(first.accepted(), "first crossed connection is admitted");
  std::unique_ptr<session::PairingAdmissionLease> first_lease = std::move(first.lease);
  session::PairingAdmissionResult canonical =
      duplicates.Admit(session::PairingAdmissionRequest{
          .connection_id = Handle(2),
          .source = Source(2),
          .local_key = local,
          .peer_key = peer,
          .local_role = tls::Role::kResponder,
          .now_ms = 0,
      });
  Expect(canonical.accepted() && canonical.displaced_connection == Handle(1) &&
             duplicates.active_connections() == 1,
         "lexicographically smaller initiator key wins crossed pairing");
  std::unique_ptr<session::PairingAdmissionLease> canonical_lease =
      std::move(canonical.lease);
  Expect(first_lease != nullptr && !first_lease->active() &&
             canonical_lease != nullptr && canonical_lease->active(),
         "displaced lease is invalid while the winner remains active");
  Expect(!duplicates
              .Admit(session::PairingAdmissionRequest{
                  .connection_id = Handle(3),
                  .source = Source(3),
                  .local_key = local,
                  .peer_key = peer,
                  .local_role = tls::Role::kResponder,
                  .now_ms = 0,
              })
              .accepted(),
         "later duplicate in the same direction loses");
  Expect(canonical_lease->MarkVisible() == session::PairingError::kNone,
         "retained winner reaches the only visible prompt");
  Expect(!duplicates
              .Admit(session::PairingAdmissionRequest{
                  .connection_id = Handle(4),
                  .source = Source(4),
                  .local_key = local,
                  .peer_key = peer,
                  .local_role = tls::Role::kInitiator,
                  .now_ms = 0,
              })
              .accepted(),
         "visible winner is frozen against later displacement");

  session::PairingAdmissionController reused_id;
  Expect(reused_id.OpenWindow(0, session::kMaximumPairingWindowMs),
         "reused-ID window opens");
  session::PairingAdmissionResult stale_result =
      reused_id.Admit(session::PairingAdmissionRequest{
          .connection_id = Handle(7),
          .source = Source(7),
          .local_key = local,
          .peer_key = peer,
          .local_role = tls::Role::kInitiator,
          .user_initiated = true,
          .now_ms = 0,
      });
  std::unique_ptr<session::PairingAdmissionLease> stale_lease =
      std::move(stale_result.lease);
  reused_id.CloseWindow();
  Expect(reused_id.OpenWindow(1, session::kMaximumPairingWindowMs),
         "reused-ID replacement window opens");
  identity::PublicKey replacement_peer = peer;
  replacement_peer[5] ^= 0x40U;
  session::PairingAdmissionResult replacement_result =
      reused_id.Admit(session::PairingAdmissionRequest{
          .connection_id = Handle(7),
          .source = Source(8),
          .local_key = local,
          .peer_key = replacement_peer,
          .local_role = tls::Role::kInitiator,
          .user_initiated = true,
          .now_ms = 1,
      });
  Expect(stale_lease != nullptr && !stale_lease->active() &&
             replacement_result.accepted() && replacement_result.lease->active(),
         "stale lease cannot revive when its connection ID is reused");
  stale_lease.reset();
  if (replacement_result.lease != nullptr) {
    Expect(replacement_result.lease->active() && reused_id.active_connections() == 1,
           "destroying stale lease cannot release replacement admission");
  }

  session::PairingAdmissionController source_bound;
  std::vector<std::unique_ptr<session::PairingAdmissionLease>> source_leases;
  Expect(source_bound.OpenWindow(0, session::kMaximumPairingWindowMs),
         "source-bound window opens");
  for (std::size_t index = 0; index < session::kMaxIncompletePairingHandshakesPerSource;
       ++index) {
    identity::PublicKey distinct_peer = peer;
    distinct_peer[0] = static_cast<std::uint8_t>(distinct_peer[0] + index);
    session::PairingAdmissionResult admitted =
        source_bound.Admit(session::PairingAdmissionRequest{
            .connection_id = Handle(static_cast<std::uint8_t>(index + 1)),
            .source = Source(9),
            .local_key = local,
            .peer_key = distinct_peer,
            .local_role = tls::Role::kInitiator,
            .now_ms = 0,
        });
    Expect(admitted.accepted(), "per-source incomplete handshake fits within bound");
    if (admitted.lease != nullptr) {
      source_leases.push_back(std::move(admitted.lease));
    }
  }
  identity::PublicKey third_peer = peer;
  third_peer[0] ^= 0x40U;
  Expect(!source_bound
              .Admit(session::PairingAdmissionRequest{
                  .connection_id = Handle(9),
                  .source = Source(9),
                  .local_key = local,
                  .peer_key = third_peer,
                  .local_role = tls::Role::kInitiator,
                  .now_ms = 0,
              })
              .accepted(),
         "third incomplete handshake from one source is busy");

  session::PairingAdmissionController global_bound;
  std::vector<std::unique_ptr<session::PairingAdmissionLease>> global_leases;
  Expect(global_bound.OpenWindow(0, session::kMaximumPairingWindowMs),
         "global-bound window opens");
  for (std::size_t index = 0; index < session::kMaxIncompletePairingHandshakes -
                                          session::kReservedUserInitiatedPairingSlots;
       ++index) {
    identity::PublicKey distinct_peer = peer;
    distinct_peer[1] = static_cast<std::uint8_t>(index + 1);
    session::PairingAdmissionResult admitted =
        global_bound.Admit(session::PairingAdmissionRequest{
            .connection_id = Handle(static_cast<std::uint8_t>(index + 1)),
            .source = Source(static_cast<std::uint8_t>(index + 1)),
            .local_key = local,
            .peer_key = distinct_peer,
            .local_role = tls::Role::kInitiator,
            .now_ms = 0,
        });
    Expect(admitted.accepted(), "non-user handshake fits below reserved capacity");
    if (admitted.lease != nullptr) {
      global_leases.push_back(std::move(admitted.lease));
    }
  }
  identity::PublicKey reserved_peer = peer;
  reserved_peer[1] = 0xe0U;
  Expect(!global_bound
              .Admit(session::PairingAdmissionRequest{
                  .connection_id = Handle(15),
                  .source = Source(15),
                  .local_key = local,
                  .peer_key = reserved_peer,
                  .local_role = tls::Role::kInitiator,
                  .now_ms = 0,
              })
              .accepted(),
         "untrusted inbound cannot consume user-initiated reserved capacity");
  session::PairingAdmissionResult user_initiated =
      global_bound.Admit(session::PairingAdmissionRequest{
          .connection_id = Handle(16),
          .source = Source(16),
          .local_key = local,
          .peer_key = reserved_peer,
          .local_role = tls::Role::kInitiator,
          .user_initiated = true,
          .now_ms = 0,
      });
  Expect(user_initiated.accepted() && global_bound.active_connections() ==
                                          session::kMaxIncompletePairingHandshakes,
         "user-initiated attempt consumes the reserved final slot");
  if (user_initiated.lease != nullptr) {
    global_leases.push_back(std::move(user_initiated.lease));
  }
  identity::PublicKey overflow_peer = peer;
  overflow_peer[1] = 0xf0U;
  Expect(!global_bound
              .Admit(session::PairingAdmissionRequest{
                  .connection_id = Handle(17),
                  .source = Source(17),
                  .local_key = local,
                  .peer_key = overflow_peer,
                  .local_role = tls::Role::kInitiator,
                  .user_initiated = true,
                  .now_ms = 0,
              })
              .accepted(),
         "user-initiated attempt cannot exceed the global hard bound");

  session::PairingAdmissionController prompt_bound;
  Expect(prompt_bound.OpenWindow(0, session::kMaximumPairingWindowMs),
         "prompt-bound window opens");
  identity::PublicKey other_peer = peer;
  other_peer[2] ^= 0x20U;
  session::PairingAdmissionResult first_prompt =
      prompt_bound.Admit(session::PairingAdmissionRequest{
          .connection_id = Handle(1),
          .source = Source(1),
          .local_key = local,
          .peer_key = peer,
          .local_role = tls::Role::kInitiator,
          .now_ms = 0,
      });
  session::PairingAdmissionResult second_prompt =
      prompt_bound.Admit(session::PairingAdmissionRequest{
          .connection_id = Handle(2),
          .source = Source(2),
          .local_key = local,
          .peer_key = other_peer,
          .local_role = tls::Role::kInitiator,
          .now_ms = 0,
      });
  Expect(first_prompt.accepted() && second_prompt.accepted(),
         "different peer pairs may coexist before prompt");
  Expect(first_prompt.lease->MarkVisible() == session::PairingError::kNone &&
             second_prompt.lease->MarkVisible() == session::PairingError::kBusy &&
             prompt_bound.visible_attempts() == 1,
         "only one process-wide SAS prompt becomes visible");
  Expect(!prompt_bound.window_open(session::kMaximumPairingWindowMs),
         "pairing window expires at its absolute deadline");

  session::PairingAdmissionController global_rate;
  Expect(global_rate.OpenWindow(0, session::kMaximumPairingWindowMs),
         "global rate window opens");
  for (std::size_t index = 0; index < session::kGlobalAdmissionBucketCapacity;
       ++index) {
    identity::PublicKey distinct_peer = peer;
    distinct_peer[3] = static_cast<std::uint8_t>(index + 1);
    const session::AttemptHandle connection =
        Handle(static_cast<std::uint8_t>(index + 1));
    Expect(global_rate
               .Admit(session::PairingAdmissionRequest{
                   .connection_id = connection,
                   .source = Source(static_cast<std::uint8_t>(index + 1)),
                   .local_key = local,
                   .peer_key = distinct_peer,
                   .local_role = tls::Role::kInitiator,
                   .now_ms = 0,
               })
               .accepted(),
           "global admission token is consumed within capacity");
  }
  identity::PublicKey rate_peer = peer;
  rate_peer[3] = 0xf0U;
  const auto global_probe = [&global_rate, &local,
                             &rate_peer](const std::uint64_t now_ms) {
    return global_rate.Admit(session::PairingAdmissionRequest{
        .connection_id = Handle(31),
        .source = Source(31),
        .local_key = local,
        .peer_key = rate_peer,
        .local_role = tls::Role::kInitiator,
        .now_ms = now_ms,
    });
  };
  Expect(!global_probe(session::kGlobalAdmissionRefillMs - 1).accepted() &&
             global_probe(session::kGlobalAdmissionRefillMs).accepted(),
         "global token bucket refills exactly once per second");

  session::PairingAdmissionController source_rate;
  Expect(source_rate.OpenWindow(0, session::kMaximumPairingWindowMs),
         "source rate window opens");
  for (std::size_t index = 0; index < session::kSourceAdmissionBucketCapacity;
       ++index) {
    identity::PublicKey distinct_peer = peer;
    distinct_peer[4] = static_cast<std::uint8_t>(index + 1);
    const session::AttemptHandle connection =
        Handle(static_cast<std::uint8_t>(index + 1));
    Expect(source_rate
               .Admit(session::PairingAdmissionRequest{
                   .connection_id = connection,
                   .source = Source(1),
                   .local_key = local,
                   .peer_key = distinct_peer,
                   .local_role = tls::Role::kInitiator,
                   .now_ms = 0,
               })
               .accepted(),
           "source admission token is consumed within capacity");
  }
  identity::PublicKey source_rate_peer = peer;
  source_rate_peer[4] = 0xf0U;
  const auto source_probe = [&source_rate, &local,
                             &source_rate_peer](const std::uint64_t now_ms) {
    return source_rate.Admit(session::PairingAdmissionRequest{
        .connection_id = Handle(32),
        .source = Source(1),
        .local_key = local,
        .peer_key = source_rate_peer,
        .local_role = tls::Role::kInitiator,
        .now_ms = now_ms,
    });
  };
  Expect(!source_probe(session::kSourceAdmissionRefillMs - 1).accepted() &&
             source_probe(session::kSourceAdmissionRefillMs).accepted(),
         "source token bucket refills exactly once per fifteen seconds");
}

}  // namespace

int main() {
  TestGoldenTwoSidedSuccess();
  TestPeerConfirmationFirstAndCommitFailure();
  TestCommitSerializesCancellation();
  TestRejectionCancellationAndDecisionIdempotency();
  TestHostileGoldenFrames();
  TestConfirmationReplayTimeoutAndHandles();
  TestReplayCacheAndShutdown();
  TestConcurrentAdmissionAndReplayState();
  TestSameIdentityRejectedBeforePrompt();
  TestAdmissionBoundsAndDuplicateWinner();
  TestAttemptRequiresLiveAdmission();

  if (failures != 0) {
    std::cerr << failures << " pairing session test(s) failed\n";
    return 1;
  }
  std::cout << "Authenticated pairing state and hostile vectors passed\n";
  return 0;
}
