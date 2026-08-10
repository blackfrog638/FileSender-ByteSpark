import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:xnn_transfer/features/transfer/application/transfer_controller.dart';
import 'package:xnn_transfer/features/transfer/application/transfer_state.dart';
import 'package:xnn_transfer/features/transfer/application/vertical_slice_state.dart';
import 'package:xnn_transfer/features/transfer/domain/vertical_slice_gateway.dart';
import 'package:xnn_transfer/features/transfer/domain/vertical_slice_models.dart';

enum VerticalSliceCommandOutcome {
  applied,
  cancelled,
  invalidState,
  gatewayError,
}

class VerticalSliceController extends ChangeNotifier {
  VerticalSliceController(this._gateway)
      : _transfers = TransferController(_gateway);

  final VerticalSliceGateway _gateway;
  final TransferController _transfers;
  final List<VerticalSliceGatewayEvent> _initializationEvents =
      <VerticalSliceGatewayEvent>[];
  final Set<String> _pendingPairingCommands = <String>{};
  final Set<String> _pendingSendCommands = <String>{};

  VerticalSliceState _state = const VerticalSliceInitializing();
  StreamSubscription<VerticalSliceGatewayEvent>? _eventSubscription;
  String? _requestedPairPeerId;
  String? _activeAttemptId;
  String? _clearedPairPeerId;
  bool _initializationStarted = false;
  bool _disposed = false;

  VerticalSliceState get state => _state;

  Future<void> initialize() async {
    if (_initializationStarted || _disposed) {
      return;
    }
    _initializationStarted = true;
    _eventSubscription = _gateway.verticalSliceEvents.listen(
      _onGatewayEvent,
      onError: (Object error, StackTrace stackTrace) {
        _markUnavailable('The native peer service stopped unexpectedly.');
      },
    );
    _transfers.addListener(_onTransferStateChanged);

    await _transfers.initialize();
    if (_disposed || _state is VerticalSliceUnavailable) {
      return;
    }
    switch (_transfers.state) {
      case TransferInitializing():
        _markUnavailable('The native transfer service did not start.');
      case TransferUnavailable():
        _markUnavailable('The native transfer service is unavailable.');
      case final TransferReady transfers:
        _setState(VerticalSliceReady(transfers: transfers));
        final List<VerticalSliceGatewayEvent> buffered =
            List<VerticalSliceGatewayEvent>.of(_initializationEvents);
        _initializationEvents.clear();
        for (final VerticalSliceGatewayEvent event in buffered) {
          if (_disposed || _state is VerticalSliceUnavailable) {
            return;
          }
          _applyGatewayEvent(event);
        }
    }
  }

  Future<VerticalSliceCommandOutcome> pairPeer(String peerId) async {
    final VerticalSliceReady? ready = _readyState;
    final NearbyPeer? peer = ready?.peerById(peerId);
    if (peer == null ||
        !peer.isAvailable ||
        peer.trust != PeerTrust.untrusted ||
        _activeAttemptId != null ||
        _requestedPairPeerId != null ||
        !_pendingPairingCommands.add(peerId)) {
      return VerticalSliceCommandOutcome.invalidState;
    }

    _requestedPairPeerId = peerId;
    _clearedPairPeerId = null;
    _replaceReady(
      ready!.copyWith(pairingMessage: 'Starting a secure pairing check...'),
    );
    try {
      await _gateway.startPairing(peerId);
      return VerticalSliceCommandOutcome.applied;
    } on Object {
      if (_requestedPairPeerId == peerId && _activeAttemptId == null) {
        _requestedPairPeerId = null;
      }
      _setPairingMessage('Pairing could not be started.');
      return VerticalSliceCommandOutcome.gatewayError;
    } finally {
      _pendingPairingCommands.remove(peerId);
    }
  }

  Future<VerticalSliceCommandOutcome> confirmPairing() {
    return _decidePairing(confirm: true);
  }

  Future<VerticalSliceCommandOutcome> rejectPairing() {
    return _decidePairing(confirm: false);
  }

  Future<VerticalSliceCommandOutcome> _decidePairing({
    required bool confirm,
  }) async {
    final PairingCeremony? pairing = _readyState?.pairing;
    if (pairing == null || !_pendingPairingCommands.add(pairing.attemptId)) {
      return VerticalSliceCommandOutcome.invalidState;
    }

    try {
      if (confirm) {
        await _gateway.confirmPairing(pairing.attemptId);
        if (_isCurrentPairingAttempt(pairing.attemptId)) {
          _setPairingMessage('Waiting for the other device...');
        }
      } else {
        await _gateway.rejectPairing(pairing.attemptId);
        final VerticalSliceReady? ready = _readyState;
        if (ready != null && _isCurrentPairingAttempt(pairing.attemptId)) {
          _replaceReady(
            ready.copyWith(
              pairing: null,
              pairingMessage: 'Pairing rejection is being applied...',
            ),
          );
        }
      }
      return VerticalSliceCommandOutcome.applied;
    } on Object {
      if (_isCurrentPairingAttempt(pairing.attemptId)) {
        _setPairingMessage('The pairing decision could not be applied.');
      }
      return VerticalSliceCommandOutcome.gatewayError;
    } finally {
      _pendingPairingCommands.remove(pairing.attemptId);
    }
  }

  Future<VerticalSliceCommandOutcome> selectAndSendFile(String peerId) async {
    final NearbyPeer? peer = _readyState?.peerById(peerId);
    if (peer == null ||
        !peer.isAvailable ||
        peer.trust != PeerTrust.authenticated ||
        !_pendingSendCommands.add(peerId)) {
      return VerticalSliceCommandOutcome.invalidState;
    }

    try {
      final SendSelectionOutcome outcome = await _gateway.selectAndSendFile(
        peerId,
      );
      if (outcome == SendSelectionOutcome.cancelled) {
        return VerticalSliceCommandOutcome.cancelled;
      }
      _setNotice('The selected file was offered to ${peer.displayName}.');
      return VerticalSliceCommandOutcome.applied;
    } on Object {
      _setNotice('The file could not be offered.');
      return VerticalSliceCommandOutcome.gatewayError;
    } finally {
      _pendingSendCommands.remove(peerId);
    }
  }

  Future<TransferCommandOutcome> acceptOffer(String offerId) async {
    final TransferCommandOutcome outcome = await _transfers.acceptOffer(
      offerId,
    );
    if (outcome == TransferCommandOutcome.gatewayError) {
      _setNotice('The incoming file could not be accepted.');
    }
    return outcome;
  }

  Future<TransferCommandOutcome> rejectOffer(String offerId) async {
    final TransferCommandOutcome outcome = await _transfers.rejectOffer(
      offerId,
    );
    if (outcome == TransferCommandOutcome.gatewayError) {
      _setNotice('The incoming file could not be rejected.');
    }
    return outcome;
  }

  Future<TransferCommandOutcome> cancelTransfer(String transferId) async {
    final TransferCommandOutcome outcome = await _transfers.cancelTransfer(
      transferId,
    );
    if (outcome == TransferCommandOutcome.gatewayError) {
      _setNotice('The transfer could not be cancelled.');
    }
    return outcome;
  }

  void _onGatewayEvent(VerticalSliceGatewayEvent event) {
    if (_disposed) {
      return;
    }
    if (_state is VerticalSliceInitializing) {
      _initializationEvents.add(event);
      return;
    }
    _applyGatewayEvent(event);
  }

  void _applyGatewayEvent(VerticalSliceGatewayEvent event) {
    final VerticalSliceReady? ready = _readyState;
    if (ready == null) {
      return;
    }
    switch (event) {
      case NearbyPeerUpserted():
        _upsertPeer(ready, event.peer);
      case NearbyPeerExpired():
        _expirePeer(ready, event.peerId);
      case PairingAttemptUpdated():
        _applyPairingAttempt(ready, event.attempt);
      case PairingAttemptCleared():
        _applyPairingCleared(ready, event.attemptId);
      case PeerTrustUpdated():
        _applyTrustUpdate(ready, event.peerId, event.isActive);
      case VerticalSliceGatewayNotice():
        _replaceReady(ready.copyWith(notice: event.message));
    }
  }

  void _upsertPeer(VerticalSliceReady ready, NearbyPeer replacement) {
    final NearbyPeer? current = ready.peerById(replacement.id);
    final NearbyPeer merged = replacement.copyWith(
      trust: current?.trust == PeerTrust.authenticated
          ? PeerTrust.authenticated
          : replacement.trust,
      isAvailable: true,
    );
    final List<NearbyPeer> peers = <NearbyPeer>[
      for (final NearbyPeer peer in ready.peers)
        if (peer.id != replacement.id) peer,
      merged,
    ]..sort(
        (NearbyPeer left, NearbyPeer right) =>
            left.displayName.compareTo(right.displayName),
      );
    _replaceReady(ready.copyWith(peers: peers));
  }

  void _expirePeer(VerticalSliceReady ready, String peerId) {
    final NearbyPeer? current = ready.peerById(peerId);
    if (current == null) {
      return;
    }
    if (current.trust == PeerTrust.authenticated) {
      _replacePeer(ready, current.copyWith(isAvailable: false));
    } else {
      _replaceReady(
        ready.copyWith(
          peers: ready.peers.where((NearbyPeer peer) => peer.id != peerId),
        ),
      );
    }
    if (_requestedPairPeerId == peerId) {
      _requestedPairPeerId = null;
      _activeAttemptId = null;
      _clearPairing('The selected nearby device is no longer available.');
    }
  }

  void _applyPairingAttempt(
    VerticalSliceReady ready,
    PairingGatewayAttempt attempt,
  ) {
    if (ready.peerById(attempt.peerId) == null ||
        (_requestedPairPeerId != null &&
            _requestedPairPeerId != attempt.peerId) ||
        (_activeAttemptId != null && _activeAttemptId != attempt.id)) {
      return;
    }

    switch (attempt.state) {
      case PairingGatewayState.starting:
        _clearedPairPeerId = null;
        _activeAttemptId = attempt.id;
        _requestedPairPeerId = attempt.peerId;
        _replaceReady(
          ready.copyWith(pairingMessage: 'Starting a secure pairing check...'),
        );
      case PairingGatewayState.awaitingConfirmation:
        if (attempt.sasWords.length != 5 ||
            attempt.sasWords.any((String word) => word.isEmpty)) {
          _markUnavailable('The native pairing response was invalid.');
          return;
        }
        _clearedPairPeerId = null;
        _activeAttemptId = attempt.id;
        _requestedPairPeerId = attempt.peerId;
        _replaceReady(
          ready.copyWith(
            pairing: PairingCeremony(
              attemptId: attempt.id,
              peerId: attempt.peerId,
              sasWords: attempt.sasWords,
            ),
            pairingMessage: null,
          ),
        );
      case PairingGatewayState.paired:
        final bool authenticated =
            ready.peerById(attempt.peerId)?.trust == PeerTrust.authenticated;
        _activeAttemptId = null;
        _clearedPairPeerId = null;
        _requestedPairPeerId = authenticated ? null : attempt.peerId;
        _replaceReady(
          ready.copyWith(
            pairing: null,
            pairingMessage: authenticated
                ? 'Pairing completed. The peer is authenticated.'
                : 'Pairing completed. Waiting for trust activation...',
          ),
        );
      case PairingGatewayState.closed:
        _activeAttemptId = null;
        _requestedPairPeerId = null;
        _clearedPairPeerId = null;
        _replaceReady(
          ready.copyWith(
            pairing: null,
            pairingMessage: _pairingFailureMessage(attempt.error),
          ),
        );
    }
  }

  void _applyPairingCleared(VerticalSliceReady ready, String attemptId) {
    if (_activeAttemptId != attemptId &&
        ready.pairing?.attemptId != attemptId) {
      return;
    }
    _clearedPairPeerId = ready.pairing?.peerId ?? _requestedPairPeerId;
    _clearPairing('Pairing ended before its final status was received.');
  }

  String _pairingFailureMessage(PairingGatewayError error) {
    return switch (error) {
      PairingGatewayError.rejected => 'Pairing was rejected.',
      PairingGatewayError.cancelled => 'Pairing was cancelled.',
      PairingGatewayError.timedOut => 'Pairing timed out.',
      PairingGatewayError.busy => 'Another pairing attempt is already active.',
      PairingGatewayError.unavailable =>
        'Secure pairing is not available on this device.',
      PairingGatewayError.failed ||
      PairingGatewayError.none =>
        'Pairing failed.',
    };
  }

  void _applyTrustUpdate(
    VerticalSliceReady ready,
    String peerId,
    bool isActive,
  ) {
    final NearbyPeer? current = ready.peerById(peerId);
    if (isActive) {
      final bool completesPairing = _requestedPairPeerId == peerId ||
          ready.pairing?.peerId == peerId ||
          _clearedPairPeerId == peerId;
      if (completesPairing) {
        _activeAttemptId = null;
        _requestedPairPeerId = null;
        _clearedPairPeerId = null;
      }
      final NearbyPeer replacement =
          current?.copyWith(trust: PeerTrust.authenticated) ??
              NearbyPeer(
                id: peerId,
                displayName: 'Authenticated peer',
                trust: PeerTrust.authenticated,
                isAvailable: false,
              );
      final Iterable<NearbyPeer> peers = current == null
          ? <NearbyPeer>[...ready.peers, replacement]
          : ready.peers.map(
              (NearbyPeer peer) =>
                  peer.id == replacement.id ? replacement : peer,
            );
      VerticalSliceReady next = ready.copyWith(peers: peers);
      if (completesPairing) {
        next = next.copyWith(
          pairing: null,
          pairingMessage: 'Pairing completed. The peer is authenticated.',
        );
      }
      _replaceReady(next);
      return;
    }

    if (current == null) {
      return;
    }
    if (current.isAvailable) {
      _replacePeer(ready, current.copyWith(trust: PeerTrust.untrusted));
    } else {
      _replaceReady(
        ready.copyWith(
          peers: ready.peers.where((NearbyPeer peer) => peer.id != peerId),
        ),
      );
    }
  }

  bool _isCurrentPairingAttempt(String attemptId) {
    return _activeAttemptId == attemptId ||
        _readyState?.pairing?.attemptId == attemptId;
  }

  void _replacePeer(VerticalSliceReady ready, NearbyPeer replacement) {
    _replaceReady(
      ready.copyWith(
        peers: ready.peers.map(
          (NearbyPeer peer) => peer.id == replacement.id ? replacement : peer,
        ),
      ),
    );
  }

  void _onTransferStateChanged() {
    if (_disposed || _state is VerticalSliceInitializing) {
      return;
    }
    switch (_transfers.state) {
      case TransferInitializing():
        return;
      case TransferUnavailable():
        _markUnavailable('The native transfer service is unavailable.');
      case final TransferReady transfers:
        final VerticalSliceReady? ready = _readyState;
        if (ready != null) {
          _replaceReady(ready.copyWith(transfers: transfers));
        }
    }
  }

  void _clearPairing(String message) {
    _activeAttemptId = null;
    _requestedPairPeerId = null;
    final VerticalSliceReady? ready = _readyState;
    if (ready != null) {
      _replaceReady(ready.copyWith(pairing: null, pairingMessage: message));
    }
  }

  void _setPairingMessage(String message) {
    final VerticalSliceReady? ready = _readyState;
    if (ready != null) {
      _replaceReady(ready.copyWith(pairingMessage: message));
    }
  }

  void _setNotice(String message) {
    final VerticalSliceReady? ready = _readyState;
    if (ready != null) {
      _replaceReady(ready.copyWith(notice: message));
    }
  }

  VerticalSliceReady? get _readyState {
    final VerticalSliceState current = _state;
    return current is VerticalSliceReady ? current : null;
  }

  void _replaceReady(VerticalSliceReady next) {
    _setState(next);
  }

  void _markUnavailable(String reason) {
    if (_disposed || _state is VerticalSliceUnavailable) {
      return;
    }
    _initializationEvents.clear();
    _setState(VerticalSliceUnavailable(reason));
  }

  void _setState(VerticalSliceState next) {
    if (_disposed) {
      return;
    }
    _state = next;
    notifyListeners();
  }

  @override
  void dispose() {
    if (_disposed) {
      return;
    }
    _disposed = true;
    _initializationEvents.clear();
    _transfers.removeListener(_onTransferStateChanged);
    unawaited(_eventSubscription?.cancel());
    _transfers.dispose();
    super.dispose();
  }
}
