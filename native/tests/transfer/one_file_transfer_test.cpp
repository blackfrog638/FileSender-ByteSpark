#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "test_support.hpp"

namespace {

using namespace transfer_test;

int failures = 0;

void Expect(const bool condition, const std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

void AppendU16(transfer::Bytes& output, const std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value));
}

void AppendU32(transfer::Bytes& output, const std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 24U));
  output.push_back(static_cast<std::uint8_t>(value >> 16U));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value));
}

void AddField(transfer::Bytes& body, const std::uint16_t id,
              const protocol::WireType type,
              const std::span<const std::uint8_t> value) {
  AppendU16(body, id);
  body.push_back(static_cast<std::uint8_t>(type));
  body.push_back(1);
  AppendU32(body, static_cast<std::uint32_t>(value.size()));
  body.insert(body.end(), value.begin(), value.end());
}

void AddU32(transfer::Bytes& body, const std::uint16_t id, const std::uint32_t value) {
  const std::array<std::uint8_t, 4> encoded{
      static_cast<std::uint8_t>(value >> 24U),
      static_cast<std::uint8_t>(value >> 16U),
      static_cast<std::uint8_t>(value >> 8U),
      static_cast<std::uint8_t>(value),
  };
  AddField(body, id, protocol::WireType::kU32, encoded);
}

void AddU16(transfer::Bytes& body, const std::uint16_t id, const std::uint16_t value) {
  const std::array<std::uint8_t, 2> encoded{
      static_cast<std::uint8_t>(value >> 8U),
      static_cast<std::uint8_t>(value),
  };
  AddField(body, id, protocol::WireType::kU16, encoded);
}

void AddU64(transfer::Bytes& body, const std::uint16_t id, const std::uint64_t value) {
  std::array<std::uint8_t, 8> encoded{};
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    const std::size_t shift = (encoded.size() - index - 1U) * 8U;
    encoded[index] = static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift));
  }
  AddField(body, id, protocol::WireType::kU64, encoded);
}

transfer::Bytes Frame(const protocol::MessageType type, const std::uint32_t stream_id,
                      const std::uint64_t message_id, const transfer::Bytes& body) {
  transfer::Bytes output;
  output.reserve(protocol::kFixedHeaderLength + body.size());
  output.insert(output.end(), {'X', 'N', 'N', 'T'});
  AppendU16(output, static_cast<std::uint16_t>(protocol::kFixedHeaderLength));
  output.push_back(1);
  output.push_back(0);
  AppendU16(output, static_cast<std::uint16_t>(type));
  AppendU16(output, 0);
  AppendU32(output, stream_id);
  AppendU64(output, message_id);
  AppendU32(output, static_cast<std::uint32_t>(body.size()));
  output.insert(output.end(), body.begin(), body.end());
  return output;
}

transfer::Bytes FileChunkFrame(const transfer::OneFileManifest& manifest,
                               const std::uint32_t stream_id,
                               const std::uint64_t message_id,
                               const std::uint64_t offset,
                               const std::span<const std::uint8_t> data) {
  transfer::Bytes body;
  AddField(body, 1, protocol::WireType::kBytes, manifest.transfer_id);
  AddU32(body, 2, 0);
  AddU64(body, 3, offset);
  AddField(body, 4, protocol::WireType::kBytes, data);
  const transfer::Bytes commitment =
      ChunkCommitment(manifest.transfer_id, offset, data);
  AddField(body, 5, protocol::WireType::kBytes, commitment);
  return Frame(protocol::MessageType::kFileChunk, stream_id, message_id, body);
}

transfer::Bytes CancelFrame(const transfer::OneFileManifest& manifest,
                            const std::uint32_t stream_id,
                            const std::uint64_t message_id,
                            const transfer::WireErrorCode code) {
  transfer::Bytes body;
  AddField(body, 1, protocol::WireType::kBytes, manifest.transfer_id);
  AddU16(body, 2, static_cast<std::uint16_t>(code));
  return Frame(protocol::MessageType::kCancel, stream_id, message_id, body);
}

transfer::Bytes CancelAckFrame(const transfer::OneFileManifest& manifest,
                               const std::uint32_t stream_id,
                               const std::uint64_t message_id,
                               const transfer::WireErrorCode code) {
  transfer::Bytes body;
  AddField(body, 1, protocol::WireType::kBytes, manifest.transfer_id);
  AddU16(body, 2, static_cast<std::uint16_t>(code));
  return Frame(protocol::MessageType::kCancelAck, stream_id, message_id, body);
}

std::optional<protocol::MessageType> FrameType(
    const std::span<const std::uint8_t> encoded) {
  const protocol::ParseResult parsed = protocol::ParseFrame(encoded);
  return parsed.ok() ? std::optional(parsed.frame.header.message_type) : std::nullopt;
}

std::optional<transfer::WireErrorCode> CancelAckCode(
    const std::span<const std::uint8_t> encoded) {
  const protocol::ParseResult parsed = protocol::ParseFrame(encoded);
  if (!parsed.ok() ||
      parsed.frame.header.message_type != protocol::MessageType::kCancelAck) {
    return std::nullopt;
  }
  const protocol::FieldView* const field = parsed.frame.body_fields.FindFirst(2);
  if (field == nullptr || field->value.size() != 2) {
    return std::nullopt;
  }
  return static_cast<transfer::WireErrorCode>(static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(field->value[0]) << 8U) | field->value[1]));
}

bool MutateFieldByte(transfer::Bytes& encoded, const std::uint16_t id,
                     const std::size_t byte_index, const std::uint8_t value) {
  const protocol::ParseResult parsed = protocol::ParseFrame(encoded);
  if (!parsed.ok()) {
    return false;
  }
  const protocol::FieldView* const field = parsed.frame.body_fields.FindFirst(id);
  if (field == nullptr || byte_index >= field->value.size()) {
    return false;
  }
  const std::ptrdiff_t offset = field->value.data() - encoded.data();
  if (offset < 0 || static_cast<std::size_t>(offset) >= encoded.size()) {
    return false;
  }
  encoded[static_cast<std::size_t>(offset) + byte_index] = value;
  return true;
}

std::optional<std::uint64_t> ChunkAckOffset(
    const std::span<const std::uint8_t> encoded) {
  const protocol::ParseResult parsed = protocol::ParseFrame(encoded);
  if (!parsed.ok() ||
      parsed.frame.header.message_type != protocol::MessageType::kChunkAck) {
    return std::nullopt;
  }
  const protocol::FieldView* const field = parsed.frame.body_fields.FindFirst(3);
  if (field == nullptr || field->value.size() != 8) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const std::uint8_t byte : field->value) {
    value = (value << 8U) | byte;
  }
  return value;
}

struct EnginePair {
  EnginePair(transfer::OneFileManifest input_manifest, transfer::Bytes input_data)
      : manifest(std::move(input_manifest)),
        data(std::move(input_data)),
        temporary_budget(std::make_shared<storage::TemporaryBudget>(
            transfer::kMaximumTransferWindow)) {}

  [[nodiscard]] bool Create(SessionFixture& sessions, const std::uint32_t stream_id,
                            MessageIds& initiator_ids, MessageIds& responder_ids) {
    stream = stream_id;
    sender_credit = &sessions.InitiatorBudget();
    credit = &sessions.ResponderBudget();
    transfer::TransferUpdate sender_created = transfer::OneFileSender::Create(
        sessions.InitiatorContext(stream_id), manifest,
        std::make_unique<MemorySource>(data), initiator_ids, integrity, *sender_credit,
        sender);
    transfer::TransferUpdate receiver_created = transfer::OneFileReceiver::Create(
        sessions.ResponderContext(stream_id), responder_ids, integrity,
        temporary_budget, *credit, platform, receiver);
    return sender_created.error == transfer::TransferError::kNone &&
           receiver_created.error == transfer::TransferError::kNone &&
           sender != nullptr && receiver != nullptr;
  }

  [[nodiscard]] transfer::TransferUpdate ExchangeManifest(std::uint64_t& now_ms) {
    transfer::TransferUpdate outbound = sender->Start(++now_ms);
    if (outbound.outbound_frame.empty()) {
      return outbound;
    }
    transfer::TransferUpdate received =
        receiver->ReceiveFrame(outbound.outbound_frame, ++now_ms);
    if (received.error != transfer::TransferError::kNone) {
      return received;
    }
    for (std::size_t index = 0; index < 2; ++index) {
      outbound = sender->NextOutbound(++now_ms);
      if (outbound.outbound_frame.empty()) {
        return outbound;
      }
      received = receiver->ReceiveFrame(outbound.outbound_frame, ++now_ms);
      if (received.error != transfer::TransferError::kNone) {
        return received;
      }
    }
    return received;
  }

  [[nodiscard]] bool Accept(std::uint64_t& now_ms) {
    const transfer::TransferUpdate accepted = receiver->Accept(4'096, 8'192, ++now_ms);
    platform.verifier_state = integrity.last_verifier;
    if (accepted.error != transfer::TransferError::kNone ||
        accepted.outbound_frame.empty()) {
      return false;
    }
    const transfer::TransferUpdate observed =
        sender->ReceiveFrame(accepted.outbound_frame, ++now_ms);
    return observed.error == transfer::TransferError::kNone &&
           observed.state == transfer::TransferState::kSendingFile;
  }

  [[nodiscard]] bool BeginFile(std::uint64_t& now_ms) {
    const transfer::TransferUpdate begin = sender->NextOutbound(++now_ms);
    if (begin.outbound_frame.empty()) {
      return false;
    }
    const transfer::TransferUpdate observed =
        receiver->ReceiveFrame(begin.outbound_frame, ++now_ms);
    return observed.error == transfer::TransferError::kNone;
  }

  transfer::OneFileManifest manifest;
  transfer::Bytes data;
  TestIntegrityProvider integrity;
  MemoryPlatform platform;
  std::shared_ptr<storage::TemporaryBudget> temporary_budget;
  transfer::ConnectionCreditBudget* sender_credit{};
  transfer::ConnectionCreditBudget* credit{};
  std::unique_ptr<transfer::OneFileSender> sender;
  std::unique_ptr<transfer::OneFileReceiver> receiver;
  std::uint32_t stream{};
};

class TrackingSource final : public transfer::FileSource {
 public:
  TrackingSource(transfer::Bytes bytes,
                 std::shared_ptr<std::atomic<std::size_t>> destructions)
      : bytes_(std::move(bytes)), destructions_(std::move(destructions)) {}

  ~TrackingSource() override { destructions_->fetch_add(1, std::memory_order_relaxed); }

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
  std::shared_ptr<std::atomic<std::size_t>> destructions_;
};

transfer::TransferUpdate DeliverFailure(EnginePair& pair,
                                        const transfer::TransferUpdate& failure,
                                        std::uint64_t& now_ms) {
  if (!failure.outbound_frame.empty()) {
    return pair.sender->ReceiveFrame(failure.outbound_frame, ++now_ms);
  }
  return {};
}

void TestLoopbackAndRejection(SessionFixture& sessions, MessageIds& initiator_ids,
                              MessageIds& responder_ids) {
  transfer::Bytes data(10'000);
  for (std::size_t index = 0; index < data.size(); ++index) {
    data[index] = static_cast<std::uint8_t>(index);
  }
  EnginePair pair(Manifest(0x10, "incoming/report.bin", data), data);
  Expect(pair.Create(sessions, 1, initiator_ids, responder_ids),
         "loopback engines create on authenticated session");
  std::uint64_t now_ms = 0;
  const transfer::TransferUpdate offered = pair.ExchangeManifest(now_ms);
  Expect(offered.offer.has_value() &&
             offered.state == transfer::TransferState::kAwaitingDecision,
         "one-file manifest requests an explicit receiver decision");
  Expect(pair.platform.create_calls == 0 && pair.platform.commit_calls == 0,
         "offer and manifest do not create storage");
  Expect(pair.Accept(now_ms), "explicit acceptance creates a receiving transfer");
  Expect(pair.platform.create_calls == 1,
         "temporary file is created only after acceptance");
  const transfer::TransferUpdate duplicate_accept =
      pair.receiver->Accept(4'096, 8'192, ++now_ms);
  Expect(duplicate_accept.state == transfer::TransferState::kReceivingFile &&
             duplicate_accept.error == transfer::TransferError::kNone &&
             duplicate_accept.outbound_frame.empty() &&
             pair.platform.create_calls == 1 && pair.platform.cleanup_calls == 0,
         "duplicate identical acceptance returns stable accepted progress");

  for (std::size_t iteration = 0; iteration < 32; ++iteration) {
    const transfer::TransferUpdate outbound = pair.sender->NextOutbound(++now_ms);
    Expect(!outbound.outbound_frame.empty(),
           "sender emits the next bounded file frame");
    if (outbound.outbound_frame.empty()) {
      break;
    }
    transfer::TransferUpdate receiver_update =
        pair.receiver->ReceiveFrame(outbound.outbound_frame, ++now_ms);
    Expect(receiver_update.error == transfer::TransferError::kNone,
           "receiver accepts loopback frame");
    if (!receiver_update.outbound_frame.empty()) {
      const std::optional<std::uint64_t> ack_offset =
          ChunkAckOffset(receiver_update.outbound_frame);
      transfer::TransferUpdate sender_update =
          pair.sender->ReceiveFrame(receiver_update.outbound_frame, ++now_ms);
      Expect(sender_update.error == transfer::TransferError::kNone,
             "sender accepts receiver acknowledgement");
      if (ack_offset.has_value()) {
        const transfer::TransferUpdate confirmed =
            pair.receiver->ConfirmChunkAckWritten(*ack_offset, ++now_ms);
        Expect(confirmed.error == transfer::TransferError::kNone,
               "receiver restores credit only after ACK write completion");
      }
      if (!sender_update.outbound_frame.empty()) {
        receiver_update =
            pair.receiver->ReceiveFrame(sender_update.outbound_frame, ++now_ms);
        Expect(receiver_update.error == transfer::TransferError::kNone &&
                   !receiver_update.outbound_frame.empty(),
               "receiver acknowledges committed transfer completion");
        sender_update =
            pair.sender->ReceiveFrame(receiver_update.outbound_frame, ++now_ms);
        Expect(sender_update.error == transfer::TransferError::kNone,
               "sender observes durable completion acknowledgement");
      }
    }
    if (pair.sender->state() == transfer::TransferState::kCompleted &&
        pair.receiver->state() == transfer::TransferState::kCompleted) {
      break;
    }
  }
  Expect(pair.sender->state() == transfer::TransferState::kCompleted &&
             pair.receiver->state() == transfer::TransferState::kCompleted,
         "both loopback state machines reach completed");
  Expect(pair.platform.destination == data,
         "atomic destination contains the exact source bytes");
  Expect(pair.platform.commit_calls == 1 && pair.platform.observed_sealed_before_commit,
         "file integrity seals before the atomic commit");
  Expect(pair.credit->reserved_bytes(transfer::CreditDirection::kInbound) == 0,
         "terminal completion releases connection credit");
  Expect(
      pair.sender_credit->reserved_bytes(transfer::CreditDirection::kOutbound) == 0 &&
          pair.sender_credit->active_streams() == 0 &&
          pair.credit->active_streams() == 0,
      "terminal completion releases both endpoint stream budgets");

  EnginePair rejected(Manifest(0x30, "incoming/rejected.bin", data), data);
  Expect(rejected.Create(sessions, 3, initiator_ids, responder_ids),
         "rejection engines create");
  now_ms += 10;
  const transfer::TransferUpdate reject_offer = rejected.ExchangeManifest(now_ms);
  Expect(reject_offer.offer.has_value(), "rejection case reaches explicit decision");
  const transfer::TransferUpdate rejection =
      rejected.receiver->Reject(transfer::WireErrorCode::kBusy, true, ++now_ms);
  Expect(rejection.terminal && rejection.retryable && !rejection.outbound_frame.empty(),
         "receiver emits terminal retryable busy rejection");
  const transfer::TransferUpdate rejected_sender =
      rejected.sender->ReceiveFrame(rejection.outbound_frame, ++now_ms);
  Expect(rejected_sender.state == transfer::TransferState::kRejected &&
             rejected_sender.error == transfer::TransferError::kBusy &&
             rejected_sender.retryable,
         "sender preserves receiver rejection and retry semantics");
  const transfer::TransferUpdate stable_rejection = rejected.sender->Advance(++now_ms);
  Expect(stable_rejection.state == transfer::TransferState::kRejected &&
             stable_rejection.error == transfer::TransferError::kBusy &&
             stable_rejection.wire_error == transfer::WireErrorCode::kBusy &&
             stable_rejection.retryable,
         "later sender events preserve terminal retry semantics");
  const protocol::ParseResult parsed_rejection =
      protocol::ParseFrame(rejection.outbound_frame);
  std::uint64_t late_message_id = 0;
  Expect(responder_ids.NextOutbound(late_message_id),
         "terminal-body test allocates the connection-wide message ID");
  const transfer::Bytes late_malformed =
      Frame(protocol::MessageType::kFileChunk, rejected.stream, late_message_id, {});
  const transfer::TransferUpdate late_result =
      rejected.sender->ReceiveFrame(late_malformed, ++now_ms);
  Expect(parsed_rejection.ok() &&
             late_result.error == transfer::TransferError::kMalformedMessage &&
             rejected.sender->Advance(++now_ms).error == transfer::TransferError::kBusy,
         "terminal sender validates late bodies without erasing terminal fact");
  Expect(rejected.platform.create_calls == 0 && rejected.platform.commit_calls == 0 &&
             rejected.platform.cleanup_calls == 0,
         "rejected offer has no temporary or destination side effect");
}

void TestHostileFrames(SessionFixture& sessions, MessageIds& initiator_ids,
                       MessageIds& responder_ids) {
  transfer::Bytes data(5'000, 0x5a);
  std::uint64_t now_ms = 1'000;

  EnginePair corrupt(Manifest(0x40, "incoming/corrupt.bin", data), data);
  Expect(corrupt.Create(sessions, 5, initiator_ids, responder_ids) &&
             corrupt.ExchangeManifest(now_ms).offer.has_value() &&
             corrupt.Accept(now_ms) && corrupt.BeginFile(now_ms),
         "corrupt-chunk case reaches file content");
  transfer::TransferUpdate chunk = corrupt.sender->NextOutbound(++now_ms);
  Expect(MutateFieldByte(chunk.outbound_frame, 4, 0, 0xff),
         "test mutates chunk data without changing its commitment");
  const transfer::TransferUpdate corrupt_result =
      corrupt.receiver->ReceiveFrame(chunk.outbound_frame, ++now_ms);
  Expect(corrupt_result.error == transfer::TransferError::kIntegrityFailed &&
             corrupt.platform.commit_calls == 0 && corrupt.platform.cleanup_calls == 1,
         "corrupt chunk fails closed and cleans temporary state");
  DeliverFailure(corrupt, corrupt_result, now_ms);

  SessionFixture replay_sessions;
  MessageIds replay_initiator_ids;
  MessageIds replay_responder_ids;
  EnginePair replay(Manifest(0x50, "incoming/replay.bin", data), data);
  Expect(replay_sessions.Open() &&
             replay.Create(replay_sessions, 1, replay_initiator_ids,
                           replay_responder_ids) &&
             replay.ExchangeManifest(now_ms).offer.has_value() &&
             replay.Accept(now_ms) && replay.BeginFile(now_ms),
         "replay case reaches file content");
  const transfer::TransferUpdate replay_chunk = replay.sender->NextOutbound(++now_ms);
  const transfer::TransferUpdate first =
      replay.receiver->ReceiveFrame(replay_chunk.outbound_frame, ++now_ms);
  Expect(first.error == transfer::TransferError::kNone,
         "first chunk instance is accepted");
  const transfer::TransferUpdate duplicate =
      replay.receiver->ReceiveFrame(replay_chunk.outbound_frame, ++now_ms);
  Expect(duplicate.error == transfer::TransferError::kMessageIdViolation &&
             duplicate.connection_fatal,
         "replayed message ID is connection-fatal");

  EnginePair reordered(Manifest(0x60, "incoming/reordered.bin", data), data);
  Expect(reordered.Create(sessions, 7, initiator_ids, responder_ids) &&
             reordered.ExchangeManifest(now_ms).offer.has_value() &&
             reordered.Accept(now_ms) && reordered.BeginFile(now_ms),
         "reordered case reaches file content");
  transfer::TransferUpdate reordered_chunk = reordered.sender->NextOutbound(++now_ms);
  Expect(MutateFieldByte(reordered_chunk.outbound_frame, 3, 7, 1),
         "test changes first chunk offset to a noncontiguous value");
  const transfer::TransferUpdate reordered_result =
      reordered.receiver->ReceiveFrame(reordered_chunk.outbound_frame, ++now_ms);
  Expect(reordered_result.error == transfer::TransferError::kStateViolation &&
             !reordered_result.connection_fatal,
         "noncontiguous chunk offset terminates only its stream");
  DeliverFailure(reordered, reordered_result, now_ms);

  EnginePair wrong(Manifest(0x70, "incoming/wrong-id.bin", data), data);
  Expect(wrong.Create(sessions, 9, initiator_ids, responder_ids) &&
             wrong.ExchangeManifest(now_ms).offer.has_value() && wrong.Accept(now_ms) &&
             wrong.BeginFile(now_ms),
         "wrong-transfer case reaches file content");
  transfer::TransferUpdate wrong_chunk = wrong.sender->NextOutbound(++now_ms);
  Expect(MutateFieldByte(wrong_chunk.outbound_frame, 1, 0, 0xee),
         "test substitutes a different transfer ID");
  const transfer::TransferUpdate wrong_result =
      wrong.receiver->ReceiveFrame(wrong_chunk.outbound_frame, ++now_ms);
  Expect(wrong_result.error == transfer::TransferError::kIdempotencyConflict,
         "frame from another transfer fails at stream scope");
  DeliverFailure(wrong, wrong_result, now_ms);

  EnginePair oversized(Manifest(0x80, "incoming/oversized.bin", data), data);
  Expect(oversized.Create(sessions, 11, initiator_ids, responder_ids) &&
             oversized.ExchangeManifest(now_ms).offer.has_value() &&
             oversized.Accept(now_ms) && oversized.BeginFile(now_ms),
         "oversized case reaches file content");
  std::uint64_t oversized_message_id = 0;
  Expect(initiator_ids.NextOutbound(oversized_message_id),
         "manual hostile frame obtains a connection message ID");
  const transfer::Bytes too_large(4'097, 0x41);
  const transfer::Bytes oversized_frame = FileChunkFrame(
      oversized.manifest, oversized.stream, oversized_message_id, 0, too_large);
  const transfer::TransferUpdate oversized_result =
      oversized.receiver->ReceiveFrame(oversized_frame, ++now_ms);
  Expect(oversized_result.error == transfer::TransferError::kStateViolation &&
             oversized.platform.commit_calls == 0,
         "chunk above selected size fails before storage commit");
  DeliverFailure(oversized, oversized_result, now_ms);

  EnginePair early(Manifest(0x90, "incoming/early.bin", data), data);
  Expect(early.Create(sessions, 13, initiator_ids, responder_ids),
         "early-frame receiver creates");
  std::uint64_t early_message_id = 0;
  Expect(initiator_ids.NextOutbound(early_message_id),
         "early frame obtains a message ID");
  transfer::Bytes begin_body;
  AddField(begin_body, 1, protocol::WireType::kBytes, early.manifest.transfer_id);
  AddU32(begin_body, 2, 0);
  AddU64(begin_body, 3, early.manifest.file_size);
  AddU32(begin_body, 4, 4'096);
  const transfer::TransferUpdate early_result =
      early.receiver->ReceiveFrame(Frame(protocol::MessageType::kFileBegin,
                                         early.stream, early_message_id, begin_body),
                                   ++now_ms);
  Expect(early_result.error == transfer::TransferError::kStateViolation &&
             early.platform.create_calls == 0,
         "FILE_BEGIN before offer is rejected without storage");
  DeliverFailure(early, early_result, now_ms);
}

void TestFileHashAndTimeout(SessionFixture& sessions, MessageIds& initiator_ids,
                            MessageIds& responder_ids) {
  transfer::Bytes data(5'000, 0x7c);
  transfer::OneFileManifest wrong_hash = Manifest(0xa0, "incoming/hash.bin", data);
  wrong_hash.file_commitment[0] ^= 0xffU;
  wrong_hash.manifest_commitment = ManifestCommitment(wrong_hash);

  EnginePair pair(wrong_hash, data);
  std::uint64_t now_ms = 2'000;
  Expect(pair.Create(sessions, 15, initiator_ids, responder_ids) &&
             pair.ExchangeManifest(now_ms).offer.has_value() && pair.Accept(now_ms),
         "wrong-file-hash case passes the committed manifest");
  bool integrity_failed = false;
  for (std::size_t iteration = 0; iteration < 16; ++iteration) {
    const transfer::TransferUpdate outbound = pair.sender->NextOutbound(++now_ms);
    if (outbound.outbound_frame.empty()) {
      break;
    }
    const transfer::TransferUpdate received =
        pair.receiver->ReceiveFrame(outbound.outbound_frame, ++now_ms);
    if (received.error == transfer::TransferError::kIntegrityFailed) {
      integrity_failed = true;
      DeliverFailure(pair, received, now_ms);
      break;
    }
    if (!received.outbound_frame.empty()) {
      const transfer::TransferUpdate acknowledged =
          pair.sender->ReceiveFrame(received.outbound_frame, ++now_ms);
      Expect(acknowledged.error == transfer::TransferError::kNone,
             "sender consumes hash-case chunk acknowledgement");
    }
  }
  Expect(integrity_failed && pair.platform.commit_calls == 0 &&
             pair.platform.cleanup_calls == 1,
         "file hash mismatch is detected before atomic commit");

  EnginePair timeout(Manifest(0xb0, "incoming/timeout.bin", data), data);
  Expect(timeout.Create(sessions, 17, initiator_ids, responder_ids),
         "timeout receiver creates");
  const transfer::TransferUpdate offer = timeout.sender->Start(++now_ms);
  const transfer::TransferUpdate accepted_offer =
      timeout.receiver->ReceiveFrame(offer.outbound_frame, ++now_ms);
  Expect(accepted_offer.error == transfer::TransferError::kNone,
         "timeout case accepts initial offer");
  const transfer::TransferUpdate expired =
      timeout.receiver->Advance(now_ms + transfer::kManifestProgressTimeoutMs);
  Expect(expired.error == transfer::TransferError::kTimeout && expired.terminal &&
             timeout.platform.create_calls == 0,
         "manifest progress timeout fails without temporary state");
  const transfer::TransferUpdate peer_timeout =
      DeliverFailure(timeout, expired, now_ms);
  Expect(expired.retryable && peer_timeout.retryable &&
             peer_timeout.error == transfer::TransferError::kTimeout,
         "retryable timeout disposition is identical on both endpoints");

  EnginePair decision_timeout(Manifest(0xc0, "incoming/decision-timeout.bin", data),
                              data);
  Expect(decision_timeout.Create(sessions, 19, initiator_ids, responder_ids),
         "decision-timeout receiver creates");
  const transfer::TransferUpdate decision_offer =
      decision_timeout.ExchangeManifest(now_ms);
  Expect(decision_offer.offer.has_value(),
         "decision-timeout case reaches explicit decision");
  const transfer::TransferUpdate late_accept = decision_timeout.receiver->Accept(
      4'096, 8'192, now_ms + transfer::kDecisionTimeoutMs);
  Expect(late_accept.error == transfer::TransferError::kTimeout &&
             late_accept.terminal && decision_timeout.platform.create_calls == 0,
         "late acceptance loses to the decision deadline");

  EnginePair sender_timeout(Manifest(0xc8, "incoming/sender-timeout.bin", data), data);
  Expect(sender_timeout.Create(sessions, 21, initiator_ids, responder_ids),
         "manifest-progress sender creates");
  const std::uint64_t offer_at = ++now_ms;
  Expect(!sender_timeout.sender->Start(offer_at).outbound_frame.empty(),
         "manifest-progress sender emits its offer");
  const transfer::TransferUpdate sender_expired = sender_timeout.sender->NextOutbound(
      offer_at + transfer::kManifestProgressTimeoutMs);
  Expect(sender_expired.error == transfer::TransferError::kTimeout &&
             sender_expired.terminal,
         "sender enforces manifest progress before the first entry");
}

void TestAcknowledgementDeadline() {
  SessionFixture sessions;
  MessageIds initiator_ids;
  MessageIds responder_ids;
  transfer::Bytes data(10'000, 0x33);
  EnginePair pair(Manifest(0xd0, "incoming/ack-timeout.bin", data), data);
  std::uint64_t now_ms = 0;
  Expect(sessions.Open() && pair.Create(sessions, 1, initiator_ids, responder_ids) &&
             pair.ExchangeManifest(now_ms).offer.has_value() && pair.Accept(now_ms) &&
             pair.BeginFile(now_ms),
         "ACK-timeout case reaches file content");

  const transfer::TransferUpdate first = pair.sender->NextOutbound(++now_ms);
  const std::uint64_t first_chunk_at = now_ms;
  const transfer::TransferUpdate second = pair.sender->NextOutbound(
      first_chunk_at + transfer::kAcknowledgementTimeoutMs - 1);
  Expect(!first.outbound_frame.empty() && !second.outbound_frame.empty(),
         "sender may fill its granted window before an ACK");
  const transfer::TransferUpdate expired =
      pair.sender->Advance(first_chunk_at + transfer::kAcknowledgementTimeoutMs);
  Expect(expired.error == transfer::TransferError::kTimeout && expired.terminal,
         "sending another chunk does not postpone ACK progress timeout");
}

void TestFileBeginProgressDeadline() {
  SessionFixture sessions;
  MessageIds initiator_ids;
  MessageIds responder_ids;
  transfer::Bytes data(5'000, 0x35);
  EnginePair pair(Manifest(0xd5, "incoming/file-begin-timeout.bin", data), data);
  std::uint64_t now_ms = 0;
  Expect(sessions.Open() && pair.Create(sessions, 1, initiator_ids, responder_ids) &&
             pair.ExchangeManifest(now_ms).offer.has_value() && pair.Accept(now_ms),
         "FILE_BEGIN progress case reaches accepted state");
  const std::uint64_t accepted_at = now_ms;
  const transfer::TransferUpdate file_begin =
      pair.sender->NextOutbound(accepted_at + transfer::kDataProgressTimeoutMs - 1);
  const transfer::TransferUpdate expired =
      pair.sender->NextOutbound(accepted_at + transfer::kDataProgressTimeoutMs);
  Expect(!file_begin.outbound_frame.empty() &&
             expired.error == transfer::TransferError::kTimeout && expired.terminal,
         "FILE_BEGIN does not extend the first data-progress deadline");
}

void TestReceiverWindowEnforcement() {
  SessionFixture sessions;
  MessageIds initiator_ids;
  MessageIds responder_ids;
  transfer::Bytes data(9'000, 0x38);
  EnginePair pair(Manifest(0xd8, "incoming/window-limit.bin", data), data);
  std::uint64_t now_ms = 0;
  Expect(sessions.Open() && pair.Create(sessions, 1, initiator_ids, responder_ids) &&
             pair.ExchangeManifest(now_ms).offer.has_value(),
         "receiver-window case reaches explicit decision");
  transfer::TransferUpdate accepted = pair.receiver->Accept(4'096, 4'096, ++now_ms);
  Expect(accepted.error == transfer::TransferError::kNone &&
             pair.sender->ReceiveFrame(accepted.outbound_frame, ++now_ms).error ==
                 transfer::TransferError::kNone &&
             pair.BeginFile(now_ms),
         "receiver-window case starts with a one-chunk grant");

  const transfer::TransferUpdate first = pair.sender->NextOutbound(++now_ms);
  const protocol::ParseResult parsed_first = protocol::ParseFrame(first.outbound_frame);
  const transfer::TransferUpdate pending_ack =
      pair.receiver->ReceiveFrame(first.outbound_frame, ++now_ms);
  Expect(parsed_first.ok() && !pending_ack.outbound_frame.empty(),
         "first granted chunk is accepted but its ACK remains unpublished");

  const transfer::Bytes second = FileChunkFrame(
      pair.manifest, pair.stream, parsed_first.frame.header.message_id + 1, 4'096,
      std::span<const std::uint8_t>(data).subspan(4'096, 4'096));
  const transfer::TransferUpdate exceeded =
      pair.receiver->ReceiveFrame(second, ++now_ms);
  Expect(exceeded.error == transfer::TransferError::kLimitExceeded && exceeded.terminal,
         "content beyond the unpublished receive grant fails closed");
}

void TestCreditOnlyAcknowledgement() {
  SessionFixture sessions;
  MessageIds initiator_ids;
  MessageIds responder_ids;
  transfer::Bytes data(9'000, 0x3c);
  EnginePair pair(Manifest(0xdc, "incoming/credit-only.bin", data), data);
  std::uint64_t now_ms = 0;
  Expect(sessions.Open() && pair.Create(sessions, 1, initiator_ids, responder_ids) &&
             pair.ExchangeManifest(now_ms).offer.has_value(),
         "credit-only ACK fixture reaches explicit decision");
  transfer::TransferUpdate accepted = pair.receiver->Accept(4'096, 4'096, ++now_ms);
  Expect(pair.sender->ReceiveFrame(accepted.outbound_frame, ++now_ms).error ==
                 transfer::TransferError::kNone &&
             pair.BeginFile(now_ms),
         "credit-only ACK fixture starts with one chunk of credit");
  const transfer::TransferUpdate chunk = pair.sender->NextOutbound(++now_ms);
  transfer::TransferUpdate ack =
      pair.receiver->ReceiveFrame(chunk.outbound_frame, ++now_ms);
  Expect(MutateFieldByte(ack.outbound_frame, 4, 2, 0) &&
             pair.sender->ReceiveFrame(ack.outbound_frame, ++now_ms).error ==
                 transfer::TransferError::kNone,
         "first cumulative ACK advances offset without replenishing credit");

  std::uint64_t credit_message_id = 0;
  Expect(responder_ids.NextOutbound(credit_message_id),
         "credit-only ACK allocates the next connection message ID");
  transfer::Bytes body;
  AddField(body, 1, protocol::WireType::kBytes, pair.manifest.transfer_id);
  AddU32(body, 2, 0);
  AddU64(body, 3, 4'096);
  AddU32(body, 4, 4'096);
  const transfer::TransferUpdate replenished = pair.sender->ReceiveFrame(
      Frame(protocol::MessageType::kChunkAck, pair.stream, credit_message_id, body),
      ++now_ms);
  Expect(replenished.error == transfer::TransferError::kNone &&
             !pair.sender->NextOutbound(++now_ms).outbound_frame.empty(),
         "credit-only cumulative ACK replenishes an exhausted sender window");
}

void TestDurableCommitOutbox() {
  SessionFixture sessions;
  const std::uint64_t near_exhaustion = std::numeric_limits<std::uint64_t>::max() - 2U;
  MessageIds initiator_ids(1, near_exhaustion);
  MessageIds responder_ids(near_exhaustion, 1);
  transfer::Bytes data(5'000, 0x44);
  EnginePair pair(Manifest(0xe0, "incoming/commit-outbox.bin", data), data);
  std::uint64_t now_ms = 0;
  Expect(sessions.Open() && pair.Create(sessions, 1, initiator_ids, responder_ids) &&
             pair.ExchangeManifest(now_ms).offer.has_value() && pair.Accept(now_ms) &&
             pair.BeginFile(now_ms),
         "commit-outbox case reaches file content");

  for (std::size_t index = 0; index < 2; ++index) {
    const transfer::TransferUpdate chunk = pair.sender->NextOutbound(++now_ms);
    const transfer::TransferUpdate ack =
        pair.receiver->ReceiveFrame(chunk.outbound_frame, ++now_ms);
    Expect(!ack.outbound_frame.empty() &&
               pair.sender->ReceiveFrame(ack.outbound_frame, ++now_ms).error ==
                   transfer::TransferError::kNone,
           "commit-outbox case acknowledges content");
  }
  const transfer::TransferUpdate file_end = pair.sender->NextOutbound(++now_ms);
  const transfer::TransferUpdate committed =
      pair.receiver->ReceiveFrame(file_end.outbound_frame, ++now_ms);
  Expect(committed.state == transfer::TransferState::kCommitted &&
             committed.error == transfer::TransferError::kMessageIdViolation &&
             committed.terminal && committed.connection_fatal &&
             committed.outbound_frame.empty() && pair.platform.commit_calls == 1 &&
             pair.platform.destination == data,
         "FILE_COMMIT sequence exhaustion preserves durable committed state");
  const transfer::TransferUpdate stable = pair.receiver->NextOutbound(++now_ms);
  Expect(stable.state == transfer::TransferState::kCommitted &&
             stable.error == transfer::TransferError::kMessageIdViolation &&
             stable.connection_fatal && pair.platform.destination == data &&
             pair.credit->reserved_bytes(transfer::CreditDirection::kInbound) == 0,
         "permanent committed outbox failure is stable and releases credit");
}

void TestCompletionAckOutbox() {
  SessionFixture sessions;
  const std::uint64_t near_exhaustion = std::numeric_limits<std::uint64_t>::max() - 3U;
  MessageIds initiator_ids(1, near_exhaustion);
  MessageIds responder_ids(near_exhaustion, 1);
  transfer::Bytes data(5'000, 0x48);
  EnginePair pair(Manifest(0xe8, "incoming/completion-outbox.bin", data), data);
  std::uint64_t now_ms = 0;
  Expect(sessions.Open() && pair.Create(sessions, 1, initiator_ids, responder_ids) &&
             pair.ExchangeManifest(now_ms).offer.has_value() && pair.Accept(now_ms) &&
             pair.BeginFile(now_ms),
         "completion-outbox case reaches file content");
  for (std::size_t index = 0; index < 2; ++index) {
    const transfer::TransferUpdate chunk = pair.sender->NextOutbound(++now_ms);
    const transfer::TransferUpdate ack =
        pair.receiver->ReceiveFrame(chunk.outbound_frame, ++now_ms);
    Expect(pair.sender->ReceiveFrame(ack.outbound_frame, ++now_ms).error ==
               transfer::TransferError::kNone,
           "completion-outbox case acknowledges content");
  }
  const transfer::TransferUpdate file_end = pair.sender->NextOutbound(++now_ms);
  const transfer::TransferUpdate file_commit =
      pair.receiver->ReceiveFrame(file_end.outbound_frame, ++now_ms);
  const transfer::TransferUpdate complete =
      pair.sender->ReceiveFrame(file_commit.outbound_frame, ++now_ms);
  const transfer::TransferUpdate result =
      pair.receiver->ReceiveFrame(complete.outbound_frame, ++now_ms);
  Expect(!file_commit.outbound_frame.empty() && !complete.outbound_frame.empty() &&
             result.state == transfer::TransferState::kCommitted &&
             result.error == transfer::TransferError::kMessageIdViolation &&
             result.connection_fatal && pair.platform.destination == data,
         "completion ACK exhaustion preserves committed outcome and closes connection");
}

void TestRejectOutboxExhaustion() {
  SessionFixture sessions;
  MessageIds initiator_ids;
  MessageIds responder_ids(0, 1);
  transfer::Bytes data(1, 0x4c);
  EnginePair pair(Manifest(0xec, "incoming/reject-outbox.bin", data), data);
  std::uint64_t now_ms = 0;
  Expect(sessions.Open() && pair.Create(sessions, 1, initiator_ids, responder_ids) &&
             pair.ExchangeManifest(now_ms).offer.has_value(),
         "reject-outbox case reaches explicit decision");
  pair.platform.create_error = storage::PlatformError::kNoSpace;
  const transfer::TransferUpdate rejected =
      pair.receiver->Accept(4'096, 4'096, ++now_ms);
  Expect(rejected.state == transfer::TransferState::kFailed &&
             rejected.error == transfer::TransferError::kMessageIdViolation &&
             rejected.connection_fatal && rejected.outbound_frame.empty() &&
             pair.platform.commit_calls == 0,
         "rejection outbox exhaustion closes instead of stranding a decision");
}

void TestConcurrentDecision() {
  SessionFixture sessions;
  MessageIds initiator_ids;
  MessageIds responder_ids;
  transfer::Bytes data(5'000, 0x55);
  EnginePair pair(Manifest(0xf0, "incoming/concurrent.bin", data), data);
  std::uint64_t now_ms = 0;
  Expect(sessions.Open() && pair.Create(sessions, 1, initiator_ids, responder_ids) &&
             pair.ExchangeManifest(now_ms).offer.has_value(),
         "concurrent-decision case reaches explicit decision");

  std::array<transfer::TransferUpdate, 2> results{};
  std::thread first([&] { results[0] = pair.receiver->Accept(4'096, 8'192, 10); });
  std::thread second([&] { results[1] = pair.receiver->Accept(4'096, 8'192, 10); });
  first.join();
  second.join();

  const std::size_t accepted = static_cast<std::size_t>(std::count_if(
      results.begin(), results.end(), [](const transfer::TransferUpdate& update) {
        return update.error == transfer::TransferError::kNone &&
               !update.outbound_frame.empty();
      }));
  const std::size_t duplicates = static_cast<std::size_t>(std::count_if(
      results.begin(), results.end(), [](const transfer::TransferUpdate& update) {
        return update.error == transfer::TransferError::kNone &&
               update.state == transfer::TransferState::kReceivingFile &&
               update.outbound_frame.empty();
      }));
  Expect(accepted == 1 && duplicates == 1 && pair.platform.create_calls == 1 &&
             pair.platform.cleanup_calls == 0,
         "concurrent decisions preserve exactly one accepted outcome");
}

void TestConnectionCreditBudget() {
  transfer::ConnectionMessageSequence sequence;
  std::uint64_t first_outbound = 0;
  Expect(sequence.NextOutbound(first_outbound) && first_outbound == 1,
         "connection sequence allocates exact outbound IDs");
  Expect(sequence.ObserveInbound(1) && !sequence.ObserveInbound(3) &&
             !sequence.ObserveInbound(2),
         "inbound gap is terminal for the connection sequence");

  transfer::ConnectionCreditBudget budget(transfer::kMaximumConnectionWindow);
  for (std::size_t index = 0; index < 32; ++index) {
    Expect(budget.TryOpenStream(static_cast<std::uint32_t>(index * 2U + 1U), true,
                                tls::Role::kInitiator),
           "thirty-two ordered streams fit the connection hard limit");
  }
  Expect(!budget.TryOpenStream(65, true, tls::Role::kInitiator),
         "thirty-third stream is rejected");
  for (std::size_t index = 0; index < 4; ++index) {
    Expect(budget.TryReserve(transfer::CreditDirection::kOutbound,
                             transfer::kMaximumTransferWindow),
           "four transfer windows fit the connection hard limit");
    Expect(budget.TryReserve(transfer::CreditDirection::kInbound,
                             transfer::kMaximumTransferWindow),
           "opposite-direction windows have an independent hard limit");
  }
  Expect(!budget.TryReserve(transfer::CreditDirection::kOutbound, 1),
         "connection credit rejects one byte above its hard limit");
  Expect(budget.reserved_bytes(transfer::CreditDirection::kOutbound) ==
                 transfer::kMaximumConnectionWindow &&
             budget.reserved_bytes(transfer::CreditDirection::kInbound) ==
                 transfer::kMaximumConnectionWindow,
         "connection credit reports the exact reservation");
  budget.Release(transfer::CreditDirection::kOutbound,
                 transfer::kMaximumTransferWindow);
  Expect(budget.TryReserve(transfer::CreditDirection::kOutbound, 1),
         "released terminal credit is reusable");
  budget.CloseStream(1);
  Expect(budget.active_streams() == 31,
         "terminal stream token is released exactly once");
  Expect(!budget.TryOpenStream(1, true, tls::Role::kInitiator),
         "released stream ID is never reused");

  transfer::ConnectionCreditBudget peer_budget(transfer::kMaximumConnectionWindow, 1);
  Expect(peer_budget.TryOpenStream(2, false, tls::Role::kInitiator) &&
             !peer_budget.TryOpenStream(4, false, tls::Role::kInitiator),
         "peer capacity rejection observes the exact stream identifier");
  peer_budget.CloseStream(2);
  Expect(!peer_budget.TryOpenStream(4, false, tls::Role::kInitiator) &&
             peer_budget.TryOpenStream(6, false, tls::Role::kInitiator),
         "a rejected peer stream ID remains consumed without desynchronizing");
}

void TestCapacityAdmissionSequence() {
  SessionFixture sessions;
  Expect(sessions.Open(), "capacity-admission fixture opens");
  MessageIds initiator_ids;
  MessageIds responder_ids;
  TestIntegrityProvider integrity;
  transfer::ConnectionCreditBudget responder_credit(transfer::kMaximumConnectionWindow,
                                                    1);
  transfer::Bytes data(1, 0x70);

  const auto create_endpoint = [&](const std::uint32_t stream_id,
                                   const std::uint8_t seed,
                                   std::unique_ptr<transfer::OneFileSender>& sender,
                                   std::unique_ptr<transfer::OneFileReceiver>& receiver,
                                   MemoryPlatform& platform) {
    const transfer::OneFileManifest manifest =
        Manifest(seed, "incoming/capacity.bin", data);
    auto temporary_budget =
        std::make_shared<storage::TemporaryBudget>(transfer::kMaximumTransferWindow);
    const transfer::TransferUpdate sender_created = transfer::OneFileSender::Create(
        sessions.InitiatorContext(stream_id), manifest,
        std::make_unique<MemorySource>(data), initiator_ids, integrity,
        sessions.InitiatorBudget(), sender);
    const transfer::TransferUpdate receiver_created = transfer::OneFileReceiver::Create(
        sessions.ResponderContext(stream_id), responder_ids, integrity,
        std::move(temporary_budget), responder_credit, platform, receiver);
    return sender_created.error == transfer::TransferError::kNone &&
           receiver_created.error == transfer::TransferError::kNone;
  };

  std::unique_ptr<transfer::OneFileSender> first_sender;
  std::unique_ptr<transfer::OneFileReceiver> first_receiver;
  MemoryPlatform first_platform;
  Expect(create_endpoint(1, 0x71, first_sender, first_receiver, first_platform),
         "first capacity endpoint creates");
  const transfer::TransferUpdate first_offer = first_sender->Start(1);
  Expect(first_receiver->ReceiveFrame(first_offer.outbound_frame, 2).error ==
             transfer::TransferError::kNone,
         "first peer stream occupies the sole active slot");

  std::unique_ptr<transfer::OneFileSender> rejected_sender;
  std::unique_ptr<transfer::OneFileReceiver> rejected_receiver;
  MemoryPlatform rejected_platform;
  Expect(
      create_endpoint(3, 0x72, rejected_sender, rejected_receiver, rejected_platform),
      "capacity-rejected endpoint object creates before its first frame");
  const transfer::TransferUpdate rejected_offer = rejected_sender->Start(3);
  const transfer::TransferUpdate rejected =
      rejected_receiver->ReceiveFrame(rejected_offer.outbound_frame, 4);
  Expect(rejected.error == transfer::TransferError::kBusy && rejected.terminal,
         "full connection consumes and rejects the peer stream offer");

  static_cast<void>(first_receiver->Shutdown());
  std::unique_ptr<transfer::OneFileSender> next_sender;
  std::unique_ptr<transfer::OneFileReceiver> next_receiver;
  MemoryPlatform next_platform;
  Expect(create_endpoint(5, 0x73, next_sender, next_receiver, next_platform),
         "next capacity endpoint creates after release");
  const transfer::TransferUpdate next_offer = next_sender->Start(5);
  const transfer::TransferUpdate next =
      next_receiver->ReceiveFrame(next_offer.outbound_frame, 6);
  Expect(next.error == transfer::TransferError::kNone &&
             next.state == transfer::TransferState::kReceivingManifest,
         "capacity rejection consumes both stream and message IDs");
}

void TestAuthorizationBeforeParsing() {
  SessionFixture sessions;
  Expect(sessions.Open(), "authorization-order fixture opens");
  MessageIds receiver_ids;
  TestIntegrityProvider integrity;
  auto temporary_budget =
      std::make_shared<storage::TemporaryBudget>(transfer::kMaximumTransferWindow);
  transfer::ConnectionCreditBudget credit(transfer::kMaximumConnectionWindow);
  MemoryPlatform platform;
  std::unique_ptr<transfer::OneFileReceiver> receiver;
  const transfer::TransferUpdate created = transfer::OneFileReceiver::Create(
      sessions.ResponderContext(1), receiver_ids, integrity, temporary_budget, credit,
      platform, receiver);
  Expect(created.error == transfer::TransferError::kNone,
         "authorized receiver creates before revocation");
  sessions.DeactivateResponder();
  const std::array<std::uint8_t, 1> malformed{0xff};
  const transfer::TransferUpdate rejected = receiver->ReceiveFrame(malformed, 1);
  Expect(rejected.error == transfer::TransferError::kUnauthenticated &&
             rejected.connection_fatal && platform.create_calls == 0,
         "authorization failure wins before malformed body parsing");
}

void TestInvalidLocalManifest() {
  SessionFixture sessions;
  Expect(sessions.Open(), "local-manifest fixture opens");
  transfer::Bytes data(1, 0x61);
  transfer::OneFileManifest manifest =
      Manifest(0xfa, "incoming/local-invalid.bin", data);
  manifest.display_name = std::string("\xc0\x80", 2);
  MessageIds message_ids;
  TestIntegrityProvider integrity;
  transfer::ConnectionCreditBudget credit(transfer::kMaximumConnectionWindow);
  std::unique_ptr<transfer::OneFileSender> sender;
  const transfer::TransferUpdate rejected = transfer::OneFileSender::Create(
      sessions.InitiatorContext(1), manifest, std::make_unique<MemorySource>(data),
      message_ids, integrity, credit, sender);
  Expect(rejected.error == transfer::TransferError::kInvalidArgument &&
             sender == nullptr && credit.active_streams() == 0,
         "malformed local display-name UTF-8 is rejected before stream admission");
}

void TestInvalidRejectionReason() {
  SessionFixture sessions;
  MessageIds initiator_ids;
  MessageIds responder_ids;
  transfer::Bytes data(1, 0x62);
  EnginePair pair(Manifest(0xfb, "incoming/invalid-reject.bin", data), data);
  std::uint64_t now_ms = 0;
  Expect(sessions.Open() && pair.Create(sessions, 1, initiator_ids, responder_ids) &&
             pair.ExchangeManifest(now_ms).offer.has_value(),
         "invalid-rejection fixture reaches sender decision state");
  transfer::Bytes body;
  AddField(body, 1, protocol::WireType::kBytes, pair.manifest.transfer_id);
  const std::array<std::uint8_t, 2> code{
      0x00, static_cast<std::uint8_t>(transfer::WireErrorCode::kMessageIdViolation)};
  const std::array<std::uint8_t, 1> retryable{0};
  AddField(body, 2, protocol::WireType::kU16, code);
  AddField(body, 3, protocol::WireType::kBool, retryable);
  const transfer::TransferUpdate rejected = pair.sender->ReceiveFrame(
      Frame(protocol::MessageType::kTransferReject, pair.stream, 1, body), ++now_ms);
  Expect(rejected.error == transfer::TransferError::kMalformedMessage &&
             rejected.state == transfer::TransferState::kFailed,
         "protocol errors cannot masquerade as transfer rejection reasons");
}

void TestRetryDelayLimit() {
  SessionFixture sessions;
  MessageIds initiator_ids;
  MessageIds responder_ids;
  transfer::Bytes data(1, 0x63);
  EnginePair pair(Manifest(0xfc, "incoming/retry-delay.bin", data), data);
  std::uint64_t now_ms = 0;
  Expect(sessions.Open() && pair.Create(sessions, 1, initiator_ids, responder_ids) &&
             pair.ExchangeManifest(now_ms).offer.has_value(),
         "retry-delay fixture reaches sender decision state");
  transfer::Bytes body;
  AddField(body, 1, protocol::WireType::kBytes, pair.manifest.transfer_id);
  const std::array<std::uint8_t, 2> code{
      0x01, static_cast<std::uint8_t>(transfer::WireErrorCode::kBusy)};
  const std::array<std::uint8_t, 1> retryable{1};
  AddField(body, 2, protocol::WireType::kU16, code);
  AddField(body, 3, protocol::WireType::kBool, retryable);
  AddU32(body, 4, 60'001);
  const transfer::TransferUpdate rejected = pair.sender->ReceiveFrame(
      Frame(protocol::MessageType::kTransferReject, pair.stream, 1, body), ++now_ms);
  Expect(rejected.error == transfer::TransferError::kMalformedMessage,
         "retry delay above 60 seconds is rejected");
}

void TestStreamZeroFatality() {
  SessionFixture sessions;
  Expect(sessions.Open(), "stream-scope fixture opens");
  MessageIds receiver_ids;
  TestIntegrityProvider integrity;
  auto temporary_budget =
      std::make_shared<storage::TemporaryBudget>(transfer::kMaximumTransferWindow);
  transfer::ConnectionCreditBudget credit(transfer::kMaximumConnectionWindow);
  MemoryPlatform platform;
  std::unique_ptr<transfer::OneFileReceiver> receiver;
  Expect(transfer::OneFileReceiver::Create(sessions.ResponderContext(1), receiver_ids,
                                           integrity, temporary_budget, credit,
                                           platform, receiver)
                 .error == transfer::TransferError::kNone,
         "stream-scope receiver creates");
  const transfer::Bytes invalid =
      Frame(protocol::MessageType::kTransferOffer, 0, 1, {});
  const transfer::TransferUpdate rejected = receiver->ReceiveFrame(invalid, 1);
  Expect(rejected.error == transfer::TransferError::kStateViolation &&
             rejected.connection_fatal,
         "transfer message on stream zero is connection-fatal");
}

void TestCancellationLifecycle() {
  SessionFixture sessions;
  Expect(sessions.Open(), "cancellation fixture opens");
  MessageIds initiator_ids;
  MessageIds responder_ids;
  transfer::Bytes data(4'096, 0x5c);
  std::uint64_t now_ms = 10'000;

  {
    EnginePair before_accept(Manifest(0xb0, "incoming/cancel-before-accept.bin", data),
                             data);
    Expect(before_accept.Create(sessions, 1, initiator_ids, responder_ids) &&
               before_accept.ExchangeManifest(now_ms).offer.has_value(),
           "cancel-before-accept reaches receiver decision");
    const transfer::TransferUpdate cancel = before_accept.sender->Cancel(++now_ms);
    Expect(cancel.state == transfer::TransferState::kCancelling &&
               FrameType(cancel.outbound_frame) == protocol::MessageType::kCancel &&
               before_accept.sender_credit->active_streams() == 1,
           "sender emits CANCEL before acceptance");
    const transfer::TransferUpdate ack =
        before_accept.receiver->ReceiveFrame(cancel.outbound_frame, ++now_ms);
    Expect(ack.state == transfer::TransferState::kCancelled && ack.terminal &&
               CancelAckCode(ack.outbound_frame) == transfer::WireErrorCode::kCancelled,
           "receiver cleans up and acknowledges cancellation");
    const transfer::TransferUpdate terminal =
        before_accept.sender->ReceiveFrame(ack.outbound_frame, ++now_ms);
    Expect(terminal.state == transfer::TransferState::kCancelled &&
               terminal.error == transfer::TransferError::kCancelled &&
               terminal.terminal && !terminal.retryable,
           "sender caches authenticated cancelled terminal result");
    Expect(before_accept.platform.create_calls == 0 &&
               before_accept.platform.cleanup_calls == 0,
           "cancellation before acceptance has no storage side effects");

    std::uint64_t repeated_id = 0;
    Expect(initiator_ids.NextOutbound(repeated_id),
           "repeated cancellation allocates the next connection message ID");
    const transfer::TransferUpdate repeated_ack = before_accept.receiver->ReceiveFrame(
        CancelFrame(before_accept.manifest, before_accept.stream, repeated_id,
                    transfer::WireErrorCode::kCancelled),
        ++now_ms);
    Expect(CancelAckCode(repeated_ack.outbound_frame) ==
                   transfer::WireErrorCode::kCancelled &&
               before_accept.receiver->state() == transfer::TransferState::kCancelled,
           "terminal receiver replays its cached cancellation fact");
    const transfer::TransferUpdate repeated_terminal =
        before_accept.sender->ReceiveFrame(repeated_ack.outbound_frame, ++now_ms);
    Expect(repeated_terminal.state == transfer::TransferState::kCancelled &&
               before_accept.sender->Cancel(++now_ms).outbound_frame.empty(),
           "repeated local and remote cancellation are idempotent");
  }

  {
    EnginePair during_file(Manifest(0xc0, "incoming/cancel-during-file.bin", data),
                           data);
    Expect(during_file.Create(sessions, 3, initiator_ids, responder_ids) &&
               during_file.ExchangeManifest(now_ms).offer.has_value() &&
               during_file.Accept(now_ms) && during_file.BeginFile(now_ms),
           "during-file cancellation reaches the receiving phase");
    const transfer::TransferUpdate chunk = during_file.sender->NextOutbound(++now_ms);
    transfer::TransferUpdate chunk_ack =
        during_file.receiver->ReceiveFrame(chunk.outbound_frame, ++now_ms);
    const std::optional<std::uint64_t> ack_offset =
        ChunkAckOffset(chunk_ack.outbound_frame);
    Expect(ack_offset.has_value(), "during-file fixture writes one chunk");
    static_cast<void>(
        during_file.sender->ReceiveFrame(chunk_ack.outbound_frame, ++now_ms));
    if (ack_offset.has_value()) {
      static_cast<void>(
          during_file.receiver->ConfirmChunkAckWritten(*ack_offset, ++now_ms));
    }

    const transfer::TransferUpdate cancel = during_file.receiver->Cancel(++now_ms);
    Expect(
        FrameType(cancel.outbound_frame) == protocol::MessageType::kCancel &&
            !during_file.platform.open && during_file.platform.cleanup_calls == 1 &&
            during_file.credit->active_streams() == 1 &&
            during_file.credit->reserved_bytes(transfer::CreditDirection::kInbound) ==
                8'192,
        "receiver cancellation cleans storage but retains transport budget until ACK");
    const transfer::TransferUpdate ack =
        during_file.sender->ReceiveFrame(cancel.outbound_frame, ++now_ms);
    const transfer::TransferUpdate terminal =
        during_file.receiver->ReceiveFrame(ack.outbound_frame, ++now_ms);
    Expect(terminal.state == transfer::TransferState::kCancelled &&
               during_file.sender->state() == transfer::TransferState::kCancelled &&
               during_file.temporary_budget->reserved_bytes() == 0,
           "mid-file cancellation releases temporary and both terminal states");
    static_cast<void>(during_file.receiver->Shutdown());
    static_cast<void>(during_file.receiver->Cancel(++now_ms));
    Expect(during_file.platform.cleanup_calls == 1 &&
               during_file.credit->reserved_bytes(
                   transfer::CreditDirection::kInbound) == 0 &&
               during_file.sender_credit->reserved_bytes(
                   transfer::CreditDirection::kOutbound) == 0,
           "repeated stop and cancel cannot double-clean resources");
  }

  {
    EnginePair simultaneous(Manifest(0xd0, "incoming/cancel-simultaneous.bin", data),
                            data);
    Expect(simultaneous.Create(sessions, 5, initiator_ids, responder_ids) &&
               simultaneous.ExchangeManifest(now_ms).offer.has_value(),
           "simultaneous cancellation reaches the decision phase");
    transfer::TransferUpdate sender_cancel;
    transfer::TransferUpdate receiver_cancel;
    std::atomic<bool> start{false};
    std::thread sender_thread([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      sender_cancel = simultaneous.sender->Cancel(now_ms + 1);
    });
    std::thread receiver_thread([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      receiver_cancel = simultaneous.receiver->Cancel(now_ms + 1);
    });
    start.store(true, std::memory_order_release);
    sender_thread.join();
    receiver_thread.join();
    now_ms += 2;

    const transfer::TransferUpdate sender_ack =
        simultaneous.sender->ReceiveFrame(receiver_cancel.outbound_frame, ++now_ms);
    const transfer::TransferUpdate receiver_ack =
        simultaneous.receiver->ReceiveFrame(sender_cancel.outbound_frame, ++now_ms);
    static_cast<void>(
        simultaneous.receiver->ReceiveFrame(sender_ack.outbound_frame, ++now_ms));
    static_cast<void>(
        simultaneous.sender->ReceiveFrame(receiver_ack.outbound_frame, ++now_ms));
    Expect(simultaneous.sender->state() == transfer::TransferState::kCancelled &&
               simultaneous.receiver->state() == transfer::TransferState::kCancelled &&
               simultaneous.sender_credit->active_streams() == 0 &&
               simultaneous.credit->active_streams() == 0,
           "simultaneous peer cancellation converges and closes each stream once");
  }

  {
    EnginePair invalid(Manifest(0xe0, "incoming/cancel-invalid.bin", data), data);
    Expect(invalid.Create(sessions, 7, initiator_ids, responder_ids) &&
               invalid.ExchangeManifest(now_ms).offer.has_value(),
           "invalid cancellation fixture reaches decision");
    std::uint64_t invalid_id = 0;
    Expect(initiator_ids.NextOutbound(invalid_id),
           "invalid cancellation allocates the next message ID");
    const transfer::TransferUpdate rejected = invalid.receiver->ReceiveFrame(
        CancelFrame(invalid.manifest, invalid.stream, invalid_id,
                    transfer::WireErrorCode::kBusy),
        ++now_ms);
    Expect(rejected.error == transfer::TransferError::kMalformedMessage &&
               rejected.state == transfer::TransferState::kFailed,
           "CANCEL rejects a non-cancellation wire code");
  }
}

void TestCancellationPhaseBoundaries() {
  SessionFixture sessions;
  Expect(sessions.Open(), "cancellation phase-boundary fixture opens");
  MessageIds initiator_ids;
  MessageIds responder_ids;
  transfer::Bytes data(4'096, 0xa4);
  std::uint64_t now_ms = 15'000;

  {
    EnginePair manifest_phase(
        Manifest(0x41, "incoming/cancel-during-manifest.bin", data), data);
    Expect(manifest_phase.Create(sessions, 1, initiator_ids, responder_ids),
           "manifest cancellation engines create");
    const transfer::TransferUpdate offer = manifest_phase.sender->Start(++now_ms);
    const transfer::TransferUpdate observed =
        manifest_phase.receiver->ReceiveFrame(offer.outbound_frame, ++now_ms);
    const transfer::TransferUpdate cancel = manifest_phase.sender->Cancel(++now_ms);
    const transfer::TransferUpdate ack =
        manifest_phase.receiver->ReceiveFrame(cancel.outbound_frame, ++now_ms);
    const transfer::TransferUpdate terminal =
        manifest_phase.sender->ReceiveFrame(ack.outbound_frame, ++now_ms);
    Expect(observed.state == transfer::TransferState::kReceivingManifest &&
               terminal.state == transfer::TransferState::kCancelled &&
               manifest_phase.platform.create_calls == 0,
           "cancellation during manifest stops remaining manifest frames");
  }

  {
    EnginePair cleanup_failure(
        Manifest(0x51, "incoming/cancel-cleanup-failure.bin", data), data);
    Expect(cleanup_failure.Create(sessions, 3, initiator_ids, responder_ids) &&
               cleanup_failure.ExchangeManifest(now_ms).offer.has_value() &&
               cleanup_failure.Accept(now_ms),
           "cleanup-failure cancellation opens one temporary");
    cleanup_failure.platform.cleanup_error = storage::PlatformError::kIoFailure;
    const transfer::TransferUpdate cancel = cleanup_failure.receiver->Cancel(++now_ms);
    const transfer::TransferUpdate ack =
        cleanup_failure.sender->ReceiveFrame(cancel.outbound_frame, ++now_ms);
    const transfer::TransferUpdate terminal =
        cleanup_failure.receiver->ReceiveFrame(ack.outbound_frame, ++now_ms);
    Expect(terminal.state == transfer::TransferState::kCancelled &&
               cleanup_failure.platform.cleanup_calls == 1 &&
               cleanup_failure.temporary_budget->unresolved_files() == 1 &&
               cleanup_failure.temporary_budget->unresolved_bytes() == data.size() &&
               cleanup_failure.temporary_budget->reserved_bytes() == data.size() &&
               cleanup_failure.platform.destination.empty(),
           "cleanup failure is consumed once and retained in fail-closed accounting");
    static_cast<void>(cleanup_failure.receiver->Shutdown());
    Expect(cleanup_failure.platform.cleanup_calls == 1,
           "shutdown cannot retry or delete an inaccessible cleanup orphan");
  }

  {
    EnginePair completed(Manifest(0x61, "incoming/cancel-after-complete.bin", data),
                         data);
    Expect(completed.Create(sessions, 5, initiator_ids, responder_ids) &&
               completed.ExchangeManifest(now_ms).offer.has_value() &&
               completed.Accept(now_ms),
           "post-completion cancellation fixture starts");
    for (std::size_t iteration = 0; iteration < 16; ++iteration) {
      const transfer::TransferUpdate outbound =
          completed.sender->NextOutbound(++now_ms);
      if (outbound.outbound_frame.empty()) {
        break;
      }
      transfer::TransferUpdate receiver_update =
          completed.receiver->ReceiveFrame(outbound.outbound_frame, ++now_ms);
      if (!receiver_update.outbound_frame.empty()) {
        const std::optional<std::uint64_t> offset =
            ChunkAckOffset(receiver_update.outbound_frame);
        transfer::TransferUpdate sender_update =
            completed.sender->ReceiveFrame(receiver_update.outbound_frame, ++now_ms);
        if (offset.has_value()) {
          static_cast<void>(
              completed.receiver->ConfirmChunkAckWritten(*offset, ++now_ms));
        }
        if (!sender_update.outbound_frame.empty()) {
          receiver_update =
              completed.receiver->ReceiveFrame(sender_update.outbound_frame, ++now_ms);
          static_cast<void>(
              completed.sender->ReceiveFrame(receiver_update.outbound_frame, ++now_ms));
        }
      }
      if (completed.sender->state() == transfer::TransferState::kCompleted &&
          completed.receiver->state() == transfer::TransferState::kCompleted) {
        break;
      }
    }
    std::uint64_t cancel_id = 0;
    Expect(initiator_ids.NextOutbound(cancel_id),
           "post-completion cancellation allocates a new message ID");
    const transfer::TransferUpdate completed_ack = completed.receiver->ReceiveFrame(
        CancelFrame(completed.manifest, completed.stream, cancel_id,
                    transfer::WireErrorCode::kCancelled),
        ++now_ms);
    Expect(completed.sender->state() == transfer::TransferState::kCompleted &&
               completed.receiver->state() == transfer::TransferState::kCompleted &&
               CancelAckCode(completed_ack.outbound_frame) ==
                   transfer::WireErrorCode::kCompleted &&
               completed.platform.destination == data &&
               completed.platform.cleanup_calls == 0,
           "cancellation after completion replays COMPLETED and preserves destination");
  }
}

void TestCancellationMessageIdExhaustion() {
  SessionFixture sessions;
  Expect(sessions.Open(), "cancel message-ID exhaustion fixture opens");
  MessageIds initiator_ids;
  MessageIds exhausted_responder_ids(0, 1);
  TestIntegrityProvider integrity;
  transfer::ConnectionCreditBudget sender_credit(transfer::kMaximumConnectionWindow);
  transfer::ConnectionCreditBudget receiver_credit(transfer::kMaximumConnectionWindow);
  auto temporary_budget =
      std::make_shared<storage::TemporaryBudget>(transfer::kMaximumTransferWindow);
  MemoryPlatform platform;
  transfer::Bytes data(4'096, 0xb5);
  const transfer::OneFileManifest manifest =
      Manifest(0x71, "incoming/cancel-id-exhaustion.bin", data);
  std::unique_ptr<transfer::OneFileSender> sender;
  std::unique_ptr<transfer::OneFileReceiver> receiver;
  Expect(
      transfer::OneFileSender::Create(sessions.InitiatorContext(1), manifest,
                                      std::make_unique<MemorySource>(data),
                                      initiator_ids, integrity, sender_credit, sender)
                  .error == transfer::TransferError::kNone &&
          transfer::OneFileReceiver::Create(
              sessions.ResponderContext(1), exhausted_responder_ids, integrity,
              temporary_budget, receiver_credit, platform, receiver)
                  .error == transfer::TransferError::kNone,
      "cancel message-ID exhaustion engines create");
  const transfer::TransferUpdate offer = sender->Start(1);
  static_cast<void>(receiver->ReceiveFrame(offer.outbound_frame, 2));
  const transfer::TransferUpdate cancel = sender->Cancel(3);
  const transfer::TransferUpdate exhausted =
      receiver->ReceiveFrame(cancel.outbound_frame, 4);
  const transfer::TransferUpdate stable = receiver->Advance(5);
  static_cast<void>(receiver->Shutdown());
  std::uint64_t post_shutdown_id = 0;
  Expect(initiator_ids.NextOutbound(post_shutdown_id),
         "post-shutdown cancellation allocates a test message ID");
  const transfer::TransferUpdate post_shutdown = receiver->ReceiveFrame(
      CancelFrame(manifest, 1, post_shutdown_id, transfer::WireErrorCode::kCancelled),
      6);
  Expect(
      exhausted.state == transfer::TransferState::kCancelled &&
          exhausted.error == transfer::TransferError::kMessageIdViolation &&
          stable.error == transfer::TransferError::kMessageIdViolation &&
          stable.connection_fatal && exhausted.connection_fatal &&
          exhausted.outbound_frame.empty() && post_shutdown.outbound_frame.empty() &&
          receiver_credit.active_streams() == 0,
      "CANCEL_ACK message-ID exhaustion preserves cancellation and closes connection");
}

void TestCancelAcknowledgementTimeout() {
  SessionFixture sessions;
  Expect(sessions.Open(), "cancel timeout fixture opens");
  MessageIds initiator_ids;
  TestIntegrityProvider integrity;
  transfer::ConnectionCreditBudget credit(transfer::kMaximumConnectionWindow);
  transfer::Bytes data(4'096, 0x71);
  const transfer::OneFileManifest manifest =
      Manifest(0xf0, "incoming/cancel-timeout.bin", data);
  auto destructions = std::make_shared<std::atomic<std::size_t>>(0);
  std::unique_ptr<transfer::OneFileSender> sender;
  Expect(transfer::OneFileSender::Create(
             sessions.InitiatorContext(1), manifest,
             std::make_unique<TrackingSource>(data, destructions), initiator_ids,
             integrity, credit, sender)
                 .error == transfer::TransferError::kNone,
         "cancel timeout sender creates");
  static_cast<void>(sender->Start(1));
  const transfer::TransferUpdate cancel = sender->Cancel(2);
  const transfer::TransferUpdate timeout =
      sender->Advance(2 + transfer::kCancelAcknowledgementTimeoutMs);
  Expect(FrameType(cancel.outbound_frame) == protocol::MessageType::kCancel &&
             timeout.state == transfer::TransferState::kFailed &&
             timeout.error == transfer::TransferError::kTimeout && !timeout.retryable &&
             destructions->load(std::memory_order_relaxed) == 1 &&
             credit.active_streams() == 0,
         "missing CANCEL_ACK times out after cleanup and source release");
  const transfer::TransferUpdate stable =
      sender->Cancel(3 + transfer::kCancelAcknowledgementTimeoutMs);
  const transfer::TransferUpdate late_ack = sender->ReceiveFrame(
      CancelAckFrame(manifest, 1, 1, transfer::WireErrorCode::kCancelled),
      4 + transfer::kCancelAcknowledgementTimeoutMs);
  const transfer::TransferUpdate after_late_ack =
      sender->Advance(5 + transfer::kCancelAcknowledgementTimeoutMs);
  Expect(stable.state == transfer::TransferState::kFailed &&
             stable.error == transfer::TransferError::kTimeout &&
             late_ack.state == transfer::TransferState::kFailed &&
             late_ack.error == transfer::TransferError::kStateViolation &&
             after_late_ack.error == transfer::TransferError::kTimeout,
         "late CANCEL_ACK cannot rewrite a cached timeout result");
}

void TestCommitCancelRace() {
  SessionFixture sessions;
  Expect(sessions.Open(), "commit-cancel fixture opens");
  MessageIds initiator_ids;
  MessageIds responder_ids;
  transfer::Bytes data(4'096, 0x82);
  EnginePair pair(Manifest(0x21, "incoming/commit-cancel.bin", data), data);
  std::uint64_t now_ms = 20'000;
  Expect(pair.Create(sessions, 1, initiator_ids, responder_ids) &&
             pair.ExchangeManifest(now_ms).offer.has_value() && pair.Accept(now_ms) &&
             pair.BeginFile(now_ms),
         "commit-cancel fixture reaches file transfer");

  const transfer::TransferUpdate chunk = pair.sender->NextOutbound(++now_ms);
  const transfer::TransferUpdate chunk_ack =
      pair.receiver->ReceiveFrame(chunk.outbound_frame, ++now_ms);
  const std::optional<std::uint64_t> ack_offset =
      ChunkAckOffset(chunk_ack.outbound_frame);
  static_cast<void>(pair.sender->ReceiveFrame(chunk_ack.outbound_frame, ++now_ms));
  if (ack_offset.has_value()) {
    static_cast<void>(pair.receiver->ConfirmChunkAckWritten(*ack_offset, ++now_ms));
  }
  const transfer::TransferUpdate file_end = pair.sender->NextOutbound(++now_ms);
  Expect(file_end.state == transfer::TransferState::kAwaitingFileCommit &&
             FrameType(file_end.outbound_frame) == protocol::MessageType::kFileEnd,
         "commit-cancel fixture queues FILE_END");

  pair.platform.BlockCommit();
  transfer::TransferUpdate committed;
  transfer::TransferUpdate cancelled;
  std::thread commit_thread([&] {
    committed = pair.receiver->ReceiveFrame(file_end.outbound_frame, now_ms + 1);
  });
  Expect(pair.platform.WaitForCommit(), "commit fault point becomes observable");
  std::atomic<bool> cancel_started{false};
  std::thread cancel_thread([&] {
    cancel_started.store(true, std::memory_order_release);
    cancelled = pair.receiver->Cancel(now_ms + 2);
  });
  while (!cancel_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  pair.platform.ReleaseCommit();
  commit_thread.join();
  cancel_thread.join();
  now_ms += 3;

  Expect(FrameType(committed.outbound_frame) == protocol::MessageType::kFileCommit &&
             cancelled.state == transfer::TransferState::kCommitted &&
             cancelled.outbound_frame.empty() && pair.platform.commit_calls == 1 &&
             pair.platform.cleanup_calls == 0 && pair.platform.destination == data,
         "commit that linearizes first survives concurrent local cancellation");

  const transfer::TransferUpdate completing =
      pair.sender->ReceiveFrame(committed.outbound_frame, ++now_ms);
  const transfer::TransferUpdate sender_cancel = pair.sender->Cancel(++now_ms);
  Expect(FrameType(completing.outbound_frame) ==
                 protocol::MessageType::kTransferComplete &&
             FrameType(sender_cancel.outbound_frame) == protocol::MessageType::kCancel,
         "sender cancellation remains ordered after queued completion");
  const transfer::TransferUpdate queued_completion =
      pair.receiver->ReceiveFrame(completing.outbound_frame, ++now_ms);
  const transfer::TransferUpdate completed_ack =
      pair.receiver->ReceiveFrame(sender_cancel.outbound_frame, ++now_ms);
  const transfer::TransferUpdate sender_terminal =
      pair.sender->ReceiveFrame(completed_ack.outbound_frame, ++now_ms);
  Expect(queued_completion.error == transfer::TransferError::kStateViolation &&
             CancelAckCode(completed_ack.outbound_frame) ==
                 transfer::WireErrorCode::kCompleted &&
             sender_terminal.state == transfer::TransferState::kCompleted &&
             pair.receiver->state() == transfer::TransferState::kCommitted &&
             pair.platform.cleanup_calls == 0 && pair.platform.destination == data,
         "remote cancellation after durable commit reports COMPLETED without cleanup");
}

void TestQueuedCompletionWinsCancellation() {
  SessionFixture sessions;
  Expect(sessions.Open(), "queued completion cancellation fixture opens");
  MessageIds initiator_ids;
  MessageIds responder_ids;
  transfer::Bytes data(4'096, 0x8f);
  EnginePair pair(Manifest(0x2a, "incoming/queued-completion.bin", data), data);
  std::uint64_t now_ms = 25'000;
  Expect(pair.Create(sessions, 1, initiator_ids, responder_ids) &&
             pair.ExchangeManifest(now_ms).offer.has_value() && pair.Accept(now_ms) &&
             pair.BeginFile(now_ms),
         "queued completion fixture reaches file transfer");

  const transfer::TransferUpdate chunk = pair.sender->NextOutbound(++now_ms);
  const transfer::TransferUpdate chunk_ack =
      pair.receiver->ReceiveFrame(chunk.outbound_frame, ++now_ms);
  const std::optional<std::uint64_t> ack_offset =
      ChunkAckOffset(chunk_ack.outbound_frame);
  static_cast<void>(pair.sender->ReceiveFrame(chunk_ack.outbound_frame, ++now_ms));
  if (ack_offset.has_value()) {
    static_cast<void>(pair.receiver->ConfirmChunkAckWritten(*ack_offset, ++now_ms));
  }
  const transfer::TransferUpdate file_end = pair.sender->NextOutbound(++now_ms);
  const transfer::TransferUpdate file_commit =
      pair.receiver->ReceiveFrame(file_end.outbound_frame, ++now_ms);
  const transfer::TransferUpdate completing =
      pair.sender->ReceiveFrame(file_commit.outbound_frame, ++now_ms);
  const transfer::TransferUpdate cancel = pair.sender->Cancel(++now_ms);

  const transfer::TransferUpdate completion_ack =
      pair.receiver->ReceiveFrame(completing.outbound_frame, ++now_ms);
  const transfer::TransferUpdate completed =
      pair.sender->ReceiveFrame(completion_ack.outbound_frame, ++now_ms);
  const transfer::TransferUpdate cancel_ack =
      pair.receiver->ReceiveFrame(cancel.outbound_frame, ++now_ms);
  const transfer::TransferUpdate late_cancel_ack =
      pair.sender->ReceiveFrame(cancel_ack.outbound_frame, ++now_ms);

  Expect(FrameType(completing.outbound_frame) ==
                 protocol::MessageType::kTransferComplete &&
             FrameType(cancel.outbound_frame) == protocol::MessageType::kCancel &&
             completed.state == transfer::TransferState::kCompleted &&
             late_cancel_ack.state == transfer::TransferState::kCompleted &&
             late_cancel_ack.error == transfer::TransferError::kStateViolation &&
             pair.sender_credit->reserved_bytes(transfer::CreditDirection::kOutbound) ==
                 0 &&
             pair.sender_credit->active_streams() == 0 &&
             pair.credit->active_streams() == 0 && pair.platform.destination == data,
         "queued completion ACK wins cancellation and releases all stream credit");
}

void TestShutdownBarrierAndSourceRelease() {
  SessionFixture sessions;
  Expect(sessions.Open(), "shutdown barrier fixture opens");
  MessageIds initiator_ids;
  TestIntegrityProvider integrity;
  transfer::ConnectionCreditBudget credit(transfer::kMaximumConnectionWindow);
  transfer::Bytes data(8'192, 0x93);
  const transfer::OneFileManifest manifest =
      Manifest(0x31, "incoming/shutdown.bin", data);
  auto destructions = std::make_shared<std::atomic<std::size_t>>(0);
  std::unique_ptr<transfer::OneFileSender> sender;
  Expect(transfer::OneFileSender::Create(
             sessions.InitiatorContext(1), manifest,
             std::make_unique<TrackingSource>(data, destructions), initiator_ids,
             integrity, credit, sender)
                 .error == transfer::TransferError::kNone,
         "shutdown barrier sender creates");
  static_cast<void>(sender->Start(1));

  std::atomic<bool> start{false};
  transfer::TransferUpdate queued;
  transfer::TransferUpdate cancelled;
  transfer::TransferUpdate stopped;
  std::thread outbound_thread([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    queued = sender->NextOutbound(2);
  });
  std::thread cancel_thread([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    cancelled = sender->Cancel(2);
  });
  std::thread stop_thread([&] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    stopped = sender->Shutdown();
  });
  start.store(true, std::memory_order_release);
  outbound_thread.join();
  cancel_thread.join();
  stop_thread.join();

  const transfer::TransferUpdate after_stop = sender->NextOutbound(3);
  const transfer::TransferUpdate inbound_after_stop = sender->ReceiveFrame(
      CancelFrame(manifest, 1, 1, transfer::WireErrorCode::kCancelled), 4);
  Expect(
      sender->state() == transfer::TransferState::kCancelled &&
          after_stop.outbound_frame.empty() &&
          inbound_after_stop.outbound_frame.empty() &&
          destructions->load(std::memory_order_relaxed) == 1 &&
          credit.active_streams() == 0 &&
          (queued.outbound_frame.empty() ||
           FrameType(queued.outbound_frame) == protocol::MessageType::kManifestEntry) &&
          (cancelled.outbound_frame.empty() ||
           FrameType(cancelled.outbound_frame) == protocol::MessageType::kCancel) &&
          stopped.terminal,
      "shutdown is a barrier and releases source, buffers, and stream once");
  sessions.ShutdownInitiator();
  Expect(sessions.InitiatorActiveSessions() == 0 &&
             sender->NextOutbound(4).outbound_frame.empty(),
         "session shutdown leaves no authorized callback path after transfer stop");
}

}  // namespace

int main() {
  SessionFixture sessions;
  Expect(sessions.Open(), "mutually authenticated transfer fixture opens");
  if (failures == 0) {
    MessageIds initiator_ids;
    MessageIds responder_ids;
    TestLoopbackAndRejection(sessions, initiator_ids, responder_ids);
    TestHostileFrames(sessions, initiator_ids, responder_ids);
    TestFileHashAndTimeout(sessions, initiator_ids, responder_ids);
  }
  TestAcknowledgementDeadline();
  TestFileBeginProgressDeadline();
  TestReceiverWindowEnforcement();
  TestCreditOnlyAcknowledgement();
  TestDurableCommitOutbox();
  TestCompletionAckOutbox();
  TestRejectOutboxExhaustion();
  TestConcurrentDecision();
  TestConnectionCreditBudget();
  TestCapacityAdmissionSequence();
  TestAuthorizationBeforeParsing();
  TestInvalidLocalManifest();
  TestInvalidRejectionReason();
  TestRetryDelayLimit();
  TestStreamZeroFatality();
  TestCancellationLifecycle();
  TestCancellationPhaseBoundaries();
  TestCancellationMessageIdExhaustion();
  TestCancelAcknowledgementTimeout();
  TestCommitCancelRace();
  TestQueuedCompletionWinsCancellation();
  TestShutdownBarrierAndSourceRelease();

  if (failures != 0) {
    std::cerr << failures << " transfer test(s) failed\n";
    return 1;
  }
  std::cout << "one-file transfer tests passed\n";
  return 0;
}
