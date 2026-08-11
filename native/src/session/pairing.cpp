#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

#include "internal.hpp"
#include "xnn_transfer/core/session/session.hpp"

namespace xnn_transfer::core::session {
namespace {

using security::identity::ErrorCode;
using security::identity::PeerCommit;
using security::identity::SecretBuffer;
using security::tls::ConfirmationDecision;
using security::tls::ConfirmationOutcome;
using security::tls::PairingContext;
using security::tls::Role;
using security::tls::SecurityError;
using security::tls::ValidatedEd25519PublicKey;

[[nodiscard]] PairingError MapSecurityError(const SecurityError error) noexcept {
  switch (error) {
    case SecurityError::kNone:
      return PairingError::kNone;
    case SecurityError::kUnsupportedVersion:
      return PairingError::kUnsupportedVersion;
    case SecurityError::kUnsupportedProfile:
      return PairingError::kUnsupportedProfile;
    case SecurityError::kDowngradeDetected:
      return PairingError::kDowngradeDetected;
    case SecurityError::kRoleMismatch:
      return PairingError::kRoleMismatch;
    case SecurityError::kReplayDetected:
      return PairingError::kReplayDetected;
    case SecurityError::kContextMismatch:
    case SecurityError::kConfirmationMismatch:
    case SecurityError::kInvalidDecision:
    case SecurityError::kAuthenticatedReject:
      return PairingError::kConfirmationFailed;
    case SecurityError::kInvalidPublicKey:
    case SecurityError::kPeerCertificateMismatch:
    case SecurityError::kPinMismatch:
      return PairingError::kCertificateRejected;
    case SecurityError::kLimitExceeded:
      return PairingError::kLimitExceeded;
    case SecurityError::kInvalidLength:
    case SecurityError::kMalformedEncoding:
    case SecurityError::kNonCanonicalEncoding:
    case SecurityError::kUnknownKind:
    case SecurityError::kUnknownField:
    case SecurityError::kDuplicateField:
    case SecurityError::kMissingField:
    case SecurityError::kTrailingData:
    case SecurityError::kDomainMismatch:
    case SecurityError::kInvalidNegotiation:
    case SecurityError::kUnsupportedCapability:
    case SecurityError::kMalformedTranscript:
      return PairingError::kMalformed;
    case SecurityError::kCryptoFailure:
    case SecurityError::kLocalReject:
    case SecurityError::kDeviceIdentifierMismatch:
    case SecurityError::kTransportFinishedMismatch:
    case SecurityError::kInvalidRotation:
    case SecurityError::kOutputMismatch:
    case SecurityError::kIdentityUnavailable:
    case SecurityError::kTlsConfigurationFailure:
    case SecurityError::kHandshakeIncomplete:
    case SecurityError::kTlsVersionMismatch:
    case SecurityError::kCipherMismatch:
    case SecurityError::kGroupMismatch:
    case SecurityError::kSignatureMismatch:
    case SecurityError::kAlpnMismatch:
    case SecurityError::kResumptionDetected:
    case SecurityError::kEarlyDataDetected:
    case SecurityError::kExporterFailure:
      return PairingError::kInternalFailure;
  }
  return PairingError::kInternalFailure;
}

[[nodiscard]] bool AllZero(const std::span<const std::uint8_t> bytes) noexcept {
  std::uint8_t accumulator = 0;
  for (const std::uint8_t byte : bytes) {
    accumulator = static_cast<std::uint8_t>(accumulator | byte);
  }
  return accumulator == 0;
}

[[nodiscard]] std::uint64_t CheckedDeadline(const std::uint64_t base,
                                            const std::uint64_t duration) noexcept {
  return base > std::numeric_limits<std::uint64_t>::max() - duration
             ? std::numeric_limits<std::uint64_t>::max()
             : base + duration;
}

[[nodiscard]] std::uint64_t Earlier(const std::uint64_t first,
                                    const std::uint64_t second) noexcept {
  return std::min(first, second);
}

[[nodiscard]] bool IsRole(const Role role) noexcept {
  return role == Role::kInitiator || role == Role::kResponder;
}

}  // namespace

std::string_view PairingErrorName(const PairingError error) noexcept {
  switch (error) {
    case PairingError::kNone:
      return "PAIRING_NONE";
    case PairingError::kMalformed:
      return "PAIRING_MALFORMED";
    case PairingError::kLimitExceeded:
      return "PAIRING_LIMIT_EXCEEDED";
    case PairingError::kSequenceViolation:
      return "PAIRING_SEQUENCE_VIOLATION";
    case PairingError::kUnsupportedVersion:
      return "PAIRING_UNSUPPORTED_VERSION";
    case PairingError::kUnsupportedProfile:
      return "PAIRING_UNSUPPORTED_PROFILE";
    case PairingError::kDowngradeDetected:
      return "PAIRING_DOWNGRADE_DETECTED";
    case PairingError::kRoleMismatch:
      return "PAIRING_ROLE_MISMATCH";
    case PairingError::kStateViolation:
      return "PAIRING_STATE_VIOLATION";
    case PairingError::kReplayDetected:
      return "PAIRING_REPLAY_DETECTED";
    case PairingError::kConfirmationFailed:
      return "PAIRING_CONFIRMATION_FAILED";
    case PairingError::kAuthenticatedReject:
      return "PAIRING_AUTHENTICATED_REJECT";
    case PairingError::kLocalReject:
      return "PAIRING_LOCAL_REJECT";
    case PairingError::kAlreadyDecided:
      return "PAIRING_ALREADY_DECIDED";
    case PairingError::kCancelled:
      return "PAIRING_CANCELLED";
    case PairingError::kTimeout:
      return "PAIRING_TIMEOUT";
    case PairingError::kBusy:
      return "PAIRING_BUSY";
    case PairingError::kCertificateRejected:
      return "PAIRING_CERTIFICATE_REJECTED";
    case PairingError::kInternalFailure:
      return "PAIRING_INTERNAL_FAILURE";
  }
  return "PAIRING_INTERNAL_FAILURE";
}

bool OpenSslSessionEntropy::Fill(const std::span<std::uint8_t> output) {
  if (output.empty() ||
      output.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }
  return RAND_bytes(output.data(), static_cast<int>(output.size())) == 1;
}

namespace detail {

struct PairingReplayState {
  mutable std::mutex mutex{};

  struct Entry {
    std::uint64_t reservation_id{};
    Digest256 context{};
    PublicKey peer_key{};
    std::uint64_t expires_at_ms{};
    bool bound{};
    bool reserved{};
  };

  void Prune(const std::uint64_t now_ms) {
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [now_ms](const Entry& entry) {
                                   return !entry.reserved &&
                                          entry.expires_at_ms <= now_ms;
                                 }),
                  entries.end());
  }

  [[nodiscard]] std::size_t CountPeer(const PublicKey& peer_key) const noexcept {
    return static_cast<std::size_t>(std::count_if(
        entries.begin(), entries.end(),
        [&peer_key](const Entry& entry) { return entry.peer_key == peer_key; }));
  }

  [[nodiscard]] Entry* Find(const std::uint64_t reservation_id) noexcept {
    const auto iterator = std::find_if(entries.begin(), entries.end(),
                                       [reservation_id](const Entry& entry) {
                                         return entry.reservation_id == reservation_id;
                                       });
    return iterator == entries.end() ? nullptr : &*iterator;
  }

  [[nodiscard]] const Entry* Find(const std::uint64_t reservation_id) const noexcept {
    const auto iterator = std::find_if(entries.begin(), entries.end(),
                                       [reservation_id](const Entry& entry) {
                                         return entry.reservation_id == reservation_id;
                                       });
    return iterator == entries.end() ? nullptr : &*iterator;
  }

  void Release(const std::uint64_t reservation_id) noexcept {
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [reservation_id](const Entry& entry) {
                                   return entry.reserved &&
                                          entry.reservation_id == reservation_id;
                                 }),
                  entries.end());
  }

  std::uint64_t next_reservation_id{1};
  std::vector<Entry> entries{};
};

}  // namespace detail

PairingReplayReservation::PairingReplayReservation(
    std::shared_ptr<detail::PairingReplayState> state,
    const std::uint64_t reservation_id)
    : state_(std::move(state)), reservation_id_(reservation_id) {}

PairingReplayReservation::~PairingReplayReservation() {
  if (state_ != nullptr && reservation_id_ != 0) {
    const std::lock_guard lock(state_->mutex);
    state_->Release(reservation_id_);
  }
}

PairingReplayReservation::PairingReplayReservation(
    PairingReplayReservation&&) noexcept = default;

PairingReplayReservation& PairingReplayReservation::operator=(
    PairingReplayReservation&& other) noexcept {
  if (this != &other) {
    if (state_ != nullptr && reservation_id_ != 0) {
      const std::lock_guard lock(state_->mutex);
      state_->Release(reservation_id_);
    }
    state_ = std::move(other.state_);
    reservation_id_ = std::exchange(other.reservation_id_, 0);
  }
  return *this;
}

PairingError PairingReplayReservation::Bind(const Digest256& context,
                                            const std::uint64_t now_ms) {
  if (state_ == nullptr) {
    return PairingError::kStateViolation;
  }
  const std::lock_guard lock(state_->mutex);
  if (reservation_id_ == 0) {
    return PairingError::kStateViolation;
  }
  state_->Prune(now_ms);
  detail::PairingReplayState::Entry* const entry = state_->Find(reservation_id_);
  if (entry == nullptr || !entry->reserved || entry->bound) {
    return PairingError::kStateViolation;
  }
  const bool duplicate = std::any_of(
      state_->entries.begin(), state_->entries.end(),
      [this, &context, entry](const detail::PairingReplayState::Entry& candidate) {
        return candidate.reservation_id != reservation_id_ && candidate.bound &&
               candidate.peer_key == entry->peer_key && candidate.context == context;
      });
  if (duplicate) {
    return PairingError::kReplayDetected;
  }
  entry->context = context;
  entry->bound = true;
  return PairingError::kNone;
}

void PairingReplayReservation::Commit(const std::uint64_t now_ms) noexcept {
  if (state_ == nullptr) {
    return;
  }
  const std::lock_guard lock(state_->mutex);
  if (reservation_id_ == 0) {
    return;
  }
  detail::PairingReplayState::Entry* const entry = state_->Find(reservation_id_);
  if (entry == nullptr) {
    reservation_id_ = 0;
    return;
  }
  if (!entry->bound) {
    state_->Release(reservation_id_);
    reservation_id_ = 0;
    return;
  }
  entry->expires_at_ms = CheckedDeadline(now_ms, kReplayRetentionMs);
  entry->reserved = false;
  reservation_id_ = 0;
}

bool PairingReplayReservation::active() const noexcept {
  if (state_ == nullptr) {
    return false;
  }
  const std::lock_guard lock(state_->mutex);
  const detail::PairingReplayState::Entry* const entry = state_->Find(reservation_id_);
  return entry != nullptr && entry->reserved;
}

bool PairingReplayReservation::bound() const noexcept {
  if (state_ == nullptr) {
    return false;
  }
  const std::lock_guard lock(state_->mutex);
  const detail::PairingReplayState::Entry* const entry = state_->Find(reservation_id_);
  return entry != nullptr && entry->reserved && entry->bound;
}

PairingReplayCache::PairingReplayCache()
    : state_(std::make_shared<detail::PairingReplayState>()) {}
PairingReplayCache::~PairingReplayCache() = default;
PairingReplayCache::PairingReplayCache(PairingReplayCache&&) noexcept = default;
PairingReplayCache& PairingReplayCache::operator=(PairingReplayCache&&) noexcept =
    default;

bool PairingReplayCache::Contains(const Digest256& context, const PublicKey& peer_key,
                                  const std::uint64_t now_ms) {
  const std::lock_guard lock(state_->mutex);
  state_->Prune(now_ms);
  return std::any_of(
      state_->entries.begin(), state_->entries.end(),
      [&context, &peer_key](const detail::PairingReplayState::Entry& entry) {
        return entry.bound && entry.context == context && entry.peer_key == peer_key;
      });
}

std::unique_ptr<PairingReplayReservation> PairingReplayCache::Reserve(
    const PublicKey& peer_key, const std::uint64_t now_ms) {
  const std::lock_guard lock(state_->mutex);
  state_->Prune(now_ms);
  if (state_->entries.size() >= kMaxReplayEntries ||
      state_->CountPeer(peer_key) >= kMaxReplayEntriesPerPeer) {
    return nullptr;
  }
  std::uint64_t reservation_id = state_->next_reservation_id;
  for (std::size_t attempt = 0; attempt <= kMaxReplayEntries; ++attempt) {
    if (reservation_id == 0) {
      reservation_id = 1;
    }
    if (state_->Find(reservation_id) == nullptr) {
      break;
    }
    ++reservation_id;
  }
  if (reservation_id == 0 || state_->Find(reservation_id) != nullptr) {
    return nullptr;
  }

  try {
    state_->entries.push_back(detail::PairingReplayState::Entry{
        .reservation_id = reservation_id,
        .peer_key = peer_key,
        .reserved = true,
    });
  } catch (const std::bad_alloc&) {
    return nullptr;
  }

  std::unique_ptr<PairingReplayReservation> reservation;
  try {
    reservation.reset(new PairingReplayReservation(state_, reservation_id));
  } catch (const std::bad_alloc&) {
    state_->Release(reservation_id);
    return nullptr;
  }
  state_->next_reservation_id = reservation_id + 1;
  return reservation;
}

bool PairingReplayCache::Remember(const Digest256& context, const PublicKey& peer_key,
                                  const std::uint64_t now_ms) {
  const std::lock_guard lock(state_->mutex);
  state_->Prune(now_ms);
  const auto duplicate = std::find_if(
      state_->entries.begin(), state_->entries.end(),
      [&context, &peer_key](const detail::PairingReplayState::Entry& entry) {
        return entry.bound && entry.context == context && entry.peer_key == peer_key;
      });
  if (duplicate != state_->entries.end()) {
    return true;
  }
  if (state_->entries.size() == kMaxReplayEntries ||
      state_->CountPeer(peer_key) == kMaxReplayEntriesPerPeer) {
    return false;
  }
  try {
    state_->entries.push_back(detail::PairingReplayState::Entry{
        .context = context,
        .peer_key = peer_key,
        .expires_at_ms = CheckedDeadline(now_ms, kReplayRetentionMs),
        .bound = true,
    });
    return true;
  } catch (const std::bad_alloc&) {
    return false;
  }
}

std::size_t PairingReplayCache::size() const noexcept {
  if (state_ == nullptr) {
    return 0;
  }
  const std::lock_guard lock(state_->mutex);
  return state_->entries.size();
}

struct PairingAttempt::Implementation {
  mutable std::mutex mutex{};
  security::identity::IdentityRepository* repository{};
  std::unique_ptr<PairingChannel> channel{};
  std::unique_ptr<PairingAdmissionLease> admission{};
  std::unique_ptr<PairingReplayReservation> replay_reservation{};
  PairingAttemptOptions options{};
  Role local_role{Role::kInitiator};
  AttemptHandle handle{};
  security::tls::Nonce256 local_nonce{};
  std::optional<ValidatedEd25519PublicKey> local_key{};
  std::optional<internal::Hello> local_hello{};
  std::optional<internal::Hello> remote_hello{};
  std::optional<internal::Selection> selection{};
  std::optional<PairingContext> context{};
  SecretBuffer confirmation_exporter{};
  std::array<std::uint16_t, 5> sas_indices{};
  PairingState state{PairingState::kPairingTls};
  std::optional<ConfirmationDecision> local_decision{};
  bool selection_ack_pending{};
  bool local_decision_written{};
  bool peer_confirmed{};
  std::uint32_t next_outbound_sequence{1};
  std::uint32_t next_inbound_sequence{1};
  std::size_t inbound_frames{};
  std::size_t inbound_bytes{};
  std::uint64_t absolute_deadline_ms{};
  std::uint64_t first_frame_deadline_ms{};
  std::uint64_t selection_deadline_ms{};
  std::uint64_t decision_deadline_ms{};
  std::uint64_t idle_deadline_ms{};
  std::uint64_t last_now_ms{};

  [[nodiscard]] PairingUpdate Update() const {
    return PairingUpdate{
        .state = state,
        .terminal =
            state == PairingState::kClosed || state == PairingState::kPairedLocal,
    };
  }

  void FinalizeResources() noexcept {
    if (replay_reservation != nullptr) {
      replay_reservation->Commit(last_now_ms);
      replay_reservation.reset();
    }
    std::fill(local_nonce.bytes.begin(), local_nonce.bytes.end(), 0);
    if (local_hello.has_value()) {
      std::fill(local_hello->nonce.bytes.begin(), local_hello->nonce.bytes.end(), 0);
    }
    if (remote_hello.has_value()) {
      std::fill(remote_hello->nonce.bytes.begin(), remote_hello->nonce.bytes.end(), 0);
    }
    std::fill(sas_indices.begin(), sas_indices.end(), 0);
    local_hello.reset();
    remote_hello.reset();
    selection.reset();
    context.reset();
    local_decision.reset();
    selection_ack_pending = false;
    local_decision_written = false;
    peer_confirmed = false;
    admission.reset();
    channel.reset();
  }

  [[nodiscard]] PairingUpdate Close(const PairingError error) {
    confirmation_exporter.clear();
    std::fill(handle.begin(), handle.end(), 0);
    state = PairingState::kClosed;
    FinalizeResources();
    return PairingUpdate{
        .state = state,
        .error = error,
        .terminal = true,
    };
  }

  [[nodiscard]] PairingUpdate Paired(const DeviceId& peer_device_id) {
    confirmation_exporter.clear();
    std::fill(handle.begin(), handle.end(), 0);
    state = PairingState::kPairedLocal;
    FinalizeResources();
    return PairingUpdate{
        .state = state,
        .paired_peer = peer_device_id,
        .terminal = true,
    };
  }

  [[nodiscard]] bool HandleMatches(const AttemptHandle& candidate) const noexcept {
    return candidate == handle && !AllZero(handle);
  }

  [[nodiscard]] PairingUpdate CheckDeadline(const std::uint64_t now_ms) {
    last_now_ms = std::max(last_now_ms, now_ms);
    if (state == PairingState::kClosed || state == PairingState::kPairedLocal) {
      return Update();
    }
    if (admission == nullptr || !admission->active()) {
      return Close(PairingError::kBusy);
    }
    std::uint64_t deadline = absolute_deadline_ms;
    if (state == PairingState::kExchangingHellos) {
      deadline = Earlier(deadline, first_frame_deadline_ms);
      deadline = Earlier(deadline, selection_deadline_ms);
      deadline = Earlier(deadline, idle_deadline_ms);
    } else if (state == PairingState::kSelecting) {
      deadline = Earlier(deadline, selection_deadline_ms);
      deadline = Earlier(deadline, idle_deadline_ms);
    } else if (state == PairingState::kAwaitingDecisions) {
      deadline = Earlier(deadline, decision_deadline_ms);
      deadline = Earlier(deadline, idle_deadline_ms);
    }
    if (now_ms < deadline) {
      return Update();
    }
    PairingUpdate update = Close(PairingError::kTimeout);
    try {
      update.outbound_frame = internal::EncodeAbort(next_outbound_sequence, 4);
    } catch (const std::bad_alloc&) {
      update.outbound_frame.clear();
    }
    return update;
  }

  [[nodiscard]] PairingUpdate MaybeCommit() {
    if (state != PairingState::kAwaitingDecisions ||
        local_decision != ConfirmationDecision::kConfirm || !local_decision_written ||
        !peer_confirmed || !context.has_value()) {
      return Update();
    }
    state = PairingState::kReadyToCommit;
    state = PairingState::kCommitting;
    const auto commit = repository->CommitPeer(PeerCommit{
        .public_key = channel->peer_public_key().bytes(),
        .security_profile = security::tls::kSecurityProfileV1,
        .display_label = options.peer_display_label,
    });
    if (!commit.ok()) {
      return Close(PairingError::kInternalFailure);
    }
    return Paired(commit.value());
  }

  [[nodiscard]] PairingUpdate CompleteSelection(const std::uint64_t now_ms) {
    if (!local_hello.has_value() || !remote_hello.has_value() ||
        !selection.has_value()) {
      return Close(PairingError::kInternalFailure);
    }
    const internal::Hello& initiator =
        local_role == Role::kInitiator ? *local_hello : *remote_hello;
    const internal::Hello& responder =
        local_role == Role::kResponder ? *local_hello : *remote_hello;
    const auto negotiation =
        internal::BuildNormalizedNegotiation(initiator, responder, *selection);
    if (!negotiation.ok() || !initiator.key.has_value() || !responder.key.has_value()) {
      return Close(MapSecurityError(negotiation.error));
    }
    const auto pairing_context =
        security::tls::BuildPairingContext(security::tls::PairingContextInput{
            .initiator_nonce = initiator.nonce,
            .responder_nonce = responder.nonce,
            .initiator_key = *initiator.key,
            .responder_key = *responder.key,
            .negotiation = *negotiation.value,
        });
    if (!pairing_context.ok()) {
      return Close(MapSecurityError(pairing_context.error));
    }
    context = *pairing_context.value;
    if (replay_reservation == nullptr) {
      return Close(PairingError::kInternalFailure);
    }
    const PairingError replay_error =
        replay_reservation->Bind(context->digest(), now_ms);
    if (replay_error != PairingError::kNone) {
      return Close(replay_error);
    }
    if (admission == nullptr || admission->MarkVisible() != PairingError::kNone) {
      return Close(PairingError::kBusy);
    }

    auto pairing_exporter = channel->ExportPairing(*context);
    if (!pairing_exporter.ok() ||
        pairing_exporter.value().size() != security::tls::kSha256Size) {
      return Close(PairingError::kInternalFailure);
    }
    auto sas =
        security::tls::DeriveSasWords(pairing_exporter.value().bytes(), *context);
    pairing_exporter.value().clear();
    if (!sas.ok()) {
      return Close(MapSecurityError(sas.error));
    }
    sas_indices = sas.value->indices;
    sas.value->expanded.clear();

    auto confirmation = channel->ExportConfirmation(*context);
    if (!confirmation.ok() ||
        confirmation.value().size() != security::tls::kSha256Size) {
      return Close(PairingError::kInternalFailure);
    }
    confirmation_exporter = std::move(confirmation).value();
    state = PairingState::kAwaitingDecisions;
    decision_deadline_ms =
        Earlier(absolute_deadline_ms, CheckedDeadline(now_ms, kDecisionTimeoutMs));
    idle_deadline_ms =
        Earlier(absolute_deadline_ms, CheckedDeadline(now_ms, kPairingIdleTimeoutMs));
    last_now_ms = now_ms;
    PairingUpdate update = Update();
    update.prompt = PairingPrompt{
        .handle = handle,
        .sas_word_indices = sas_indices,
        .deadline_ms = decision_deadline_ms,
    };
    return update;
  }

  [[nodiscard]] PairingUpdate ReceiveHello(const internal::Frame& frame,
                                           const std::uint64_t now_ms) {
    if (state != PairingState::kExchangingHellos || remote_hello.has_value()) {
      return Close(PairingError::kStateViolation);
    }
    internal::Hello hello{};
    const PairingError error = internal::DecodeHello(
        frame, internal::OppositeRole(local_role), channel->peer_public_key(), hello);
    if (error != PairingError::kNone) {
      return Close(error);
    }
    if (local_key->bytes() == hello.key->bytes()) {
      return Close(PairingError::kCertificateRejected);
    }
    remote_hello = std::move(hello);
    internal::Selection selected{};
    const NegotiationOffer& initiator =
        local_role == Role::kInitiator ? options.offer : remote_hello->offer;
    const NegotiationOffer& responder =
        local_role == Role::kResponder ? options.offer : remote_hello->offer;
    const PairingError selection_error =
        internal::Select(initiator, responder, selected);
    if (selection_error != PairingError::kNone) {
      return Close(selection_error);
    }
    selection = std::move(selected);
    state = PairingState::kSelecting;
    idle_deadline_ms =
        Earlier(absolute_deadline_ms, CheckedDeadline(now_ms, kPairingIdleTimeoutMs));
    last_now_ms = now_ms;
    PairingUpdate update = Update();
    if (local_role == Role::kInitiator) {
      update.outbound_frame = internal::EncodeSelection(
          internal::PairingMessageType::kSelect, next_outbound_sequence++, *selection);
      if (update.outbound_frame.empty()) {
        return Close(PairingError::kInternalFailure);
      }
    }
    return update;
  }

  [[nodiscard]] PairingUpdate ReceiveSelection(const internal::Frame& frame,
                                               const std::uint64_t now_ms) {
    if (state != PairingState::kSelecting || !selection.has_value()) {
      return Close(PairingError::kStateViolation);
    }
    const bool responder_select = local_role == Role::kResponder &&
                                  frame.type == internal::PairingMessageType::kSelect;
    const bool initiator_ack = local_role == Role::kInitiator &&
                               frame.type == internal::PairingMessageType::kSelectAck;
    if (!responder_select && !initiator_ack) {
      return Close(PairingError::kStateViolation);
    }
    internal::Selection received{};
    const PairingError error = internal::DecodeSelection(frame, received);
    if (error != PairingError::kNone) {
      return Close(error);
    }
    if (!(received == *selection)) {
      if (internal::VersionLess(received.selected_version,
                                selection->selected_version)) {
        return Close(PairingError::kDowngradeDetected);
      }
      return Close(PairingError::kMalformed);
    }
    if (local_role == Role::kResponder) {
      PairingUpdate update = Update();
      update.outbound_frame =
          internal::EncodeSelection(internal::PairingMessageType::kSelectAck,
                                    next_outbound_sequence++, *selection);
      if (update.outbound_frame.empty()) {
        return Close(PairingError::kInternalFailure);
      }
      selection_ack_pending = true;
      last_now_ms = now_ms;
      return update;
    }
    return CompleteSelection(now_ms);
  }

  [[nodiscard]] PairingUpdate ReceiveDecision(const internal::Frame& frame,
                                              const std::uint64_t now_ms) {
    if (state != PairingState::kAwaitingDecisions || peer_confirmed ||
        !context.has_value()) {
      return Close(PairingError::kStateViolation);
    }
    internal::Decision decision{};
    const PairingError decode_error = internal::DecodeDecision(frame, decision);
    if (decode_error != PairingError::kNone) {
      return Close(decode_error);
    }
    const auto verification = security::tls::VerifyConfirmation(
        confirmation_exporter.bytes(), decision.message, decision.authenticator,
        *context, internal::OppositeRole(local_role));
    if (!verification.ok()) {
      return Close(MapSecurityError(verification.error));
    }
    if (verification.value->outcome == ConfirmationOutcome::kAuthenticatedReject) {
      return Close(PairingError::kAuthenticatedReject);
    }
    peer_confirmed = true;
    idle_deadline_ms =
        Earlier(absolute_deadline_ms, CheckedDeadline(now_ms, kPairingIdleTimeoutMs));
    last_now_ms = now_ms;
    return MaybeCommit();
  }
};

PairingAttempt::PairingAttempt(std::unique_ptr<Implementation> implementation)
    : implementation_(std::move(implementation)) {}
PairingAttempt::~PairingAttempt() {
  if (implementation_ == nullptr) {
    return;
  }
  const std::lock_guard lock(implementation_->mutex);
  if (implementation_->state != PairingState::kClosed &&
      implementation_->state != PairingState::kPairedLocal) {
    static_cast<void>(implementation_->Close(PairingError::kCancelled));
  }
}

PairingUpdate PairingAttempt::Create(security::identity::IdentityRepository& repository,
                                     std::unique_ptr<PairingChannel> channel,
                                     SessionEntropy& entropy,
                                     std::unique_ptr<PairingAdmissionLease> admission,
                                     PairingReplayCache& replay_cache,
                                     PairingAttemptOptions options,
                                     std::unique_ptr<PairingAttempt>& output) {
  output.reset();
  const PublicKey* const local_public_key = repository.root_public_key();
  if (!repository.ready() || local_public_key == nullptr || channel == nullptr ||
      admission == nullptr || !admission->active() || !admission->bound()) {
    return PairingUpdate{
        .state = PairingState::kClosed,
        .error = PairingError::kBusy,
        .terminal = true,
    };
  }
  const Role local_role = admission->local_role();
  const std::uint64_t admitted_at_ms = admission->admitted_at_ms();
  const std::uint64_t window_deadline_ms = admission->window_deadline_ms();
  if (!IsRole(local_role) ||
      options.peer_display_label.size() > security::identity::kMaxDisplayLabelBytes ||
      window_deadline_ms <= admitted_at_ms ||
      window_deadline_ms > CheckedDeadline(admitted_at_ms, kMaximumPairingWindowMs) ||
      internal::ValidateOffer(options.offer) != PairingError::kNone) {
    return PairingUpdate{
        .state = PairingState::kClosed,
        .error = PairingError::kInternalFailure,
        .terminal = true,
    };
  }
  const auto validated_local =
      security::tls::ValidateEd25519PublicKey(*local_public_key);
  if (!validated_local.ok() || *local_public_key != admission->local_key() ||
      channel->peer_public_key().bytes() != admission->peer_key() ||
      *validated_local.value == channel->peer_public_key()) {
    return PairingUpdate{
        .state = PairingState::kClosed,
        .error = PairingError::kCertificateRejected,
        .terminal = true,
    };
  }
  std::unique_ptr<PairingReplayReservation> replay_reservation =
      replay_cache.Reserve(admission->peer_key(), admitted_at_ms);
  if (replay_reservation == nullptr) {
    return PairingUpdate{
        .state = PairingState::kClosed,
        .error = PairingError::kBusy,
        .terminal = true,
    };
  }

  try {
    auto implementation = std::make_unique<Implementation>();
    implementation->repository = &repository;
    implementation->channel = std::move(channel);
    implementation->admission = std::move(admission);
    implementation->replay_reservation = std::move(replay_reservation);
    implementation->options = std::move(options);
    implementation->local_role = local_role;
    implementation->local_key = *validated_local.value;
    implementation->absolute_deadline_ms = Earlier(
        window_deadline_ms, CheckedDeadline(admitted_at_ms, kCompletePairingTimeoutMs));
    implementation->last_now_ms = admitted_at_ms;
    if (!entropy.Fill(implementation->handle) ||
        !entropy.Fill(implementation->local_nonce.bytes) ||
        AllZero(implementation->handle) || AllZero(implementation->local_nonce.bytes)) {
      return PairingUpdate{
          .state = PairingState::kClosed,
          .error = PairingError::kInternalFailure,
          .terminal = true,
      };
    }
    output =
        std::unique_ptr<PairingAttempt>(new PairingAttempt(std::move(implementation)));
    return PairingUpdate{.state = PairingState::kPairingTls};
  } catch (const std::bad_alloc&) {
    return PairingUpdate{
        .state = PairingState::kClosed,
        .error = PairingError::kLimitExceeded,
        .terminal = true,
    };
  }
}

PairingUpdate PairingAttempt::Start(const std::uint64_t now_ms) {
  const std::lock_guard lock(implementation_->mutex);
  PairingUpdate deadline = implementation_->CheckDeadline(now_ms);
  if (deadline.terminal) {
    return deadline;
  }
  if (implementation_->state != PairingState::kPairingTls ||
      !implementation_->local_key.has_value()) {
    return implementation_->Close(PairingError::kStateViolation);
  }
  try {
    internal::Hello hello{};
    hello.role = implementation_->local_role;
    hello.nonce = implementation_->local_nonce;
    hello.key = *implementation_->local_key;
    hello.offer = implementation_->options.offer;
    implementation_->local_hello = hello;

    PairingUpdate update{};
    update.outbound_frame = internal::EncodeHello(
        implementation_->next_outbound_sequence++, implementation_->local_role,
        *implementation_->local_key, implementation_->local_nonce,
        implementation_->options.offer);
    if (update.outbound_frame.empty()) {
      return implementation_->Close(PairingError::kInternalFailure);
    }
    implementation_->state = PairingState::kExchangingHellos;
    implementation_->first_frame_deadline_ms =
        Earlier(implementation_->absolute_deadline_ms,
                CheckedDeadline(now_ms, kFirstPairingFrameTimeoutMs));
    implementation_->selection_deadline_ms =
        Earlier(implementation_->absolute_deadline_ms,
                CheckedDeadline(now_ms, kSelectionTimeoutMs));
    implementation_->idle_deadline_ms =
        Earlier(implementation_->absolute_deadline_ms,
                CheckedDeadline(now_ms, kPairingIdleTimeoutMs));
    implementation_->last_now_ms = now_ms;
    update.state = implementation_->state;
    return update;
  } catch (const std::bad_alloc&) {
    return implementation_->Close(PairingError::kLimitExceeded);
  }
}

PairingUpdate PairingAttempt::ReceiveFrame(const std::span<const std::uint8_t> encoded,
                                           const std::uint64_t now_ms) {
  const std::lock_guard lock(implementation_->mutex);
  PairingUpdate deadline = implementation_->CheckDeadline(now_ms);
  if (deadline.terminal) {
    return deadline;
  }
  if (implementation_->state == PairingState::kPairingTls) {
    return implementation_->Close(PairingError::kStateViolation);
  }
  if (implementation_->inbound_frames == kMaxInboundPairingFrames ||
      encoded.size() > kMaxInboundPairingBytes - implementation_->inbound_bytes) {
    return implementation_->Close(PairingError::kLimitExceeded);
  }
  ++implementation_->inbound_frames;
  implementation_->inbound_bytes += encoded.size();

  internal::ParseResult parsed = internal::ParseFrame(encoded);
  if (!parsed.ok()) {
    return implementation_->Close(parsed.error);
  }
  if (parsed.frame.sequence != implementation_->next_inbound_sequence ||
      implementation_->next_inbound_sequence ==
          std::numeric_limits<std::uint32_t>::max()) {
    return implementation_->Close(PairingError::kSequenceViolation);
  }
  ++implementation_->next_inbound_sequence;

  try {
    if (parsed.frame.type == internal::PairingMessageType::kAbort) {
      const PairingError abort_error = internal::DecodeAbort(parsed.frame);
      if (abort_error != PairingError::kNone) {
        return implementation_->Close(abort_error);
      }
      const std::uint16_t code = static_cast<std::uint16_t>(
          (static_cast<std::uint16_t>(parsed.frame.fields[0].value[0]) << 8U) |
          parsed.frame.fields[0].value[1]);
      switch (code) {
        case 2:
          return implementation_->Close(PairingError::kBusy);
        case 3:
          return implementation_->Close(PairingError::kCancelled);
        case 4:
          return implementation_->Close(PairingError::kTimeout);
        default:
          return implementation_->Close(PairingError::kInternalFailure);
      }
    }
    if (parsed.frame.type == internal::PairingMessageType::kHello) {
      return implementation_->ReceiveHello(parsed.frame, now_ms);
    }
    if (parsed.frame.type == internal::PairingMessageType::kSelect ||
        parsed.frame.type == internal::PairingMessageType::kSelectAck) {
      return implementation_->ReceiveSelection(parsed.frame, now_ms);
    }
    if (parsed.frame.type == internal::PairingMessageType::kDecision) {
      return implementation_->ReceiveDecision(parsed.frame, now_ms);
    }
    return implementation_->Close(PairingError::kStateViolation);
  } catch (const std::bad_alloc&) {
    return implementation_->Close(PairingError::kLimitExceeded);
  }
}

PairingUpdate PairingAttempt::LocalSelectionAckWritten(const AttemptHandle& handle,
                                                       const std::uint64_t now_ms) {
  const std::lock_guard lock(implementation_->mutex);
  PairingUpdate deadline = implementation_->CheckDeadline(now_ms);
  if (deadline.terminal) {
    return deadline;
  }
  if (!implementation_->HandleMatches(handle)) {
    PairingUpdate update = implementation_->Update();
    update.error = PairingError::kStateViolation;
    return update;
  }
  if (implementation_->local_role != Role::kResponder ||
      implementation_->state != PairingState::kSelecting ||
      !implementation_->selection_ack_pending) {
    return implementation_->Close(PairingError::kStateViolation);
  }
  implementation_->selection_ack_pending = false;
  implementation_->last_now_ms = now_ms;
  return implementation_->CompleteSelection(now_ms);
}

PairingUpdate PairingAttempt::Decide(const AttemptHandle& handle,
                                     const ConfirmationDecision decision,
                                     const std::uint64_t now_ms) {
  const std::lock_guard lock(implementation_->mutex);
  PairingUpdate deadline = implementation_->CheckDeadline(now_ms);
  if (deadline.terminal) {
    return deadline;
  }
  if (!implementation_->HandleMatches(handle)) {
    PairingUpdate update = implementation_->Update();
    update.error = PairingError::kStateViolation;
    return update;
  }
  if (implementation_->state != PairingState::kAwaitingDecisions ||
      !implementation_->context.has_value()) {
    return implementation_->Close(PairingError::kStateViolation);
  }
  if (implementation_->local_decision.has_value()) {
    PairingUpdate update = implementation_->Update();
    if (*implementation_->local_decision != decision) {
      update.error = PairingError::kAlreadyDecided;
    }
    return update;
  }
  if (decision != ConfirmationDecision::kReject &&
      decision != ConfirmationDecision::kConfirm) {
    return implementation_->Close(PairingError::kStateViolation);
  }
  try {
    const auto confirmation = security::tls::BuildConfirmation(
        implementation_->confirmation_exporter.bytes(), *implementation_->context,
        implementation_->local_role, decision);
    if (!confirmation.ok()) {
      return implementation_->Close(MapSecurityError(confirmation.error));
    }
    implementation_->local_decision = decision;
    PairingUpdate update = implementation_->Update();
    update.outbound_frame = internal::EncodeDecision(
        implementation_->next_outbound_sequence++, *confirmation.value);
    if (update.outbound_frame.empty()) {
      return implementation_->Close(PairingError::kInternalFailure);
    }
    if (decision == ConfirmationDecision::kReject) {
      PairingUpdate closed = implementation_->Close(PairingError::kLocalReject);
      closed.outbound_frame = std::move(update.outbound_frame);
      return closed;
    }
    implementation_->last_now_ms = now_ms;
    return update;
  } catch (const std::bad_alloc&) {
    return implementation_->Close(PairingError::kLimitExceeded);
  }
}

PairingUpdate PairingAttempt::LocalDecisionWritten(const AttemptHandle& handle,
                                                   const std::uint64_t now_ms) {
  const std::lock_guard lock(implementation_->mutex);
  PairingUpdate deadline = implementation_->CheckDeadline(now_ms);
  if (deadline.terminal) {
    return deadline;
  }
  if (!implementation_->HandleMatches(handle)) {
    PairingUpdate update = implementation_->Update();
    update.error = PairingError::kStateViolation;
    return update;
  }
  if (implementation_->state != PairingState::kAwaitingDecisions ||
      implementation_->local_decision != ConfirmationDecision::kConfirm) {
    return implementation_->Close(PairingError::kStateViolation);
  }
  implementation_->local_decision_written = true;
  implementation_->last_now_ms = now_ms;
  return implementation_->MaybeCommit();
}

PairingUpdate PairingAttempt::Cancel(const AttemptHandle& handle,
                                     const std::uint64_t now_ms) {
  const std::lock_guard lock(implementation_->mutex);
  PairingUpdate deadline = implementation_->CheckDeadline(now_ms);
  if (deadline.terminal) {
    return deadline;
  }
  if (!implementation_->HandleMatches(handle)) {
    PairingUpdate update = implementation_->Update();
    update.error = PairingError::kStateViolation;
    return update;
  }
  PairingUpdate update = implementation_->Close(PairingError::kCancelled);
  try {
    update.outbound_frame =
        internal::EncodeAbort(implementation_->next_outbound_sequence, 3);
  } catch (const std::bad_alloc&) {
    update.outbound_frame.clear();
  }
  return update;
}

PairingUpdate PairingAttempt::Advance(const std::uint64_t now_ms) {
  const std::lock_guard lock(implementation_->mutex);
  return implementation_->CheckDeadline(now_ms);
}

PairingUpdate PairingAttempt::Shutdown() {
  const std::lock_guard lock(implementation_->mutex);
  if (implementation_->state == PairingState::kClosed ||
      implementation_->state == PairingState::kPairedLocal) {
    return implementation_->Update();
  }
  return implementation_->Close(PairingError::kCancelled);
}

PairingState PairingAttempt::state() const {
  const std::lock_guard lock(implementation_->mutex);
  return implementation_->state;
}

AttemptHandle PairingAttempt::handle() const {
  const std::lock_guard lock(implementation_->mutex);
  return implementation_->handle;
}

std::optional<Digest256> PairingAttempt::pair_context() const {
  const std::lock_guard lock(implementation_->mutex);
  if (!implementation_->context.has_value()) {
    return std::nullopt;
  }
  return implementation_->context->digest();
}

}  // namespace xnn_transfer::core::session
