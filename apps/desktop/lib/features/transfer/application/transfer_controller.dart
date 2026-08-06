import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:xnn_transfer/features/transfer/application/transfer_state.dart';
import 'package:xnn_transfer/features/transfer/domain/transfer_gateway.dart';
import 'package:xnn_transfer/features/transfer/domain/transfer_models.dart';

enum TransferCommandOutcome { applied, invalidState, gatewayError }

class TransferController extends ChangeNotifier {
  TransferController(this._gateway);

  final TransferGateway _gateway;
  final Set<String> _pendingOfferCommands = <String>{};
  final Set<String> _pendingTransferCommands = <String>{};

  TransferState _state = const TransferInitializing();
  StreamSubscription<TransferGatewayEvent>? _eventSubscription;
  bool _initializationStarted = false;
  bool _disposed = false;

  TransferState get state => _state;

  Future<void> initialize() async {
    if (_initializationStarted || _disposed) {
      return;
    }
    _initializationStarted = true;

    try {
      _eventSubscription = _gateway.events.listen(
        _onGatewayEvent,
        onError: (Object error, StackTrace stackTrace) {
          _markUnavailable(error.toString());
        },
      );
      await _gateway.initialize();
      if (_state is TransferInitializing) {
        _setState(TransferReady());
      }
    } on Object catch (error) {
      _markUnavailable(error.toString());
    }
  }

  Future<TransferCommandOutcome> acceptOffer(String offerId) async {
    final TransferReady? ready = _readyState;
    final IncomingTransferOffer? offer = ready?.offerById(offerId);
    if (ready == null || offer == null || !_pendingOfferCommands.add(offerId)) {
      return TransferCommandOutcome.invalidState;
    }

    try {
      final TransferEntry transfer = await _gateway.acceptOffer(offerId);
      final TransferReady? current = _readyState;
      if (current == null) {
        return TransferCommandOutcome.invalidState;
      }
      if (!_isAcceptedTransferValid(offer, transfer) ||
          current.transferById(transfer.id) != null) {
        _markUnavailable('Transfer gateway returned an invalid acceptance');
        return TransferCommandOutcome.gatewayError;
      }

      _setState(
        current.copyWith(
          incomingOffers: current.incomingOffers.where(
            (IncomingTransferOffer item) => item.id != offerId,
          ),
          transfers: <TransferEntry>[...current.transfers, transfer],
        ),
      );
      return TransferCommandOutcome.applied;
    } on Object {
      return TransferCommandOutcome.gatewayError;
    } finally {
      _pendingOfferCommands.remove(offerId);
    }
  }

  Future<TransferCommandOutcome> rejectOffer(String offerId) async {
    final TransferReady? ready = _readyState;
    if (ready?.offerById(offerId) == null ||
        !_pendingOfferCommands.add(offerId)) {
      return TransferCommandOutcome.invalidState;
    }

    try {
      await _gateway.rejectOffer(offerId);
      final TransferReady? current = _readyState;
      if (current == null) {
        return TransferCommandOutcome.invalidState;
      }

      _setState(
        current.copyWith(
          incomingOffers: current.incomingOffers.where(
            (IncomingTransferOffer offer) => offer.id != offerId,
          ),
        ),
      );
      return TransferCommandOutcome.applied;
    } on Object {
      return TransferCommandOutcome.gatewayError;
    } finally {
      _pendingOfferCommands.remove(offerId);
    }
  }

  Future<TransferCommandOutcome> pauseTransfer(String transferId) {
    return _changeTransferStatus(
      transferId: transferId,
      allowedStatus: TransferStatus.running,
      nextStatus: TransferStatus.paused,
      gatewayCommand: _gateway.pauseTransfer,
    );
  }

  Future<TransferCommandOutcome> resumeTransfer(String transferId) {
    return _changeTransferStatus(
      transferId: transferId,
      allowedStatus: TransferStatus.paused,
      nextStatus: TransferStatus.queued,
      gatewayCommand: _gateway.resumeTransfer,
    );
  }

  Future<TransferCommandOutcome> cancelTransfer(String transferId) async {
    final TransferReady? ready = _readyState;
    final TransferEntry? transfer = ready?.transferById(transferId);
    if (transfer == null ||
        !_canCancel(transfer.status) ||
        !_pendingTransferCommands.add(transferId)) {
      return TransferCommandOutcome.invalidState;
    }

    try {
      await _gateway.cancelTransfer(transferId);
      _replaceTransferIfCurrent(
        transferId,
        expectedStatus: transfer.status,
        nextStatus: TransferStatus.cancelled,
      );
      return TransferCommandOutcome.applied;
    } on Object {
      return TransferCommandOutcome.gatewayError;
    } finally {
      _pendingTransferCommands.remove(transferId);
    }
  }

  Future<TransferCommandOutcome> _changeTransferStatus({
    required String transferId,
    required TransferStatus allowedStatus,
    required TransferStatus nextStatus,
    required Future<void> Function(String transferId) gatewayCommand,
  }) async {
    final TransferReady? ready = _readyState;
    final TransferEntry? transfer = ready?.transferById(transferId);
    if (transfer?.status != allowedStatus ||
        !_pendingTransferCommands.add(transferId)) {
      return TransferCommandOutcome.invalidState;
    }

    try {
      await gatewayCommand(transferId);
      _replaceTransferIfCurrent(
        transferId,
        expectedStatus: allowedStatus,
        nextStatus: nextStatus,
      );
      return TransferCommandOutcome.applied;
    } on Object {
      return TransferCommandOutcome.gatewayError;
    } finally {
      _pendingTransferCommands.remove(transferId);
    }
  }

  void _replaceTransferIfCurrent(
    String transferId, {
    required TransferStatus expectedStatus,
    required TransferStatus nextStatus,
  }) {
    final TransferReady? ready = _readyState;
    final TransferEntry? current = ready?.transferById(transferId);
    if (ready == null || current?.status != expectedStatus) {
      return;
    }

    _replaceTransfer(ready, current!.copyWith(status: nextStatus));
  }

  void _onGatewayEvent(TransferGatewayEvent event) {
    final TransferReady? ready = _readyState;
    switch (event) {
      case IncomingOfferReceived():
        if (ready == null || ready.offerById(event.offer.id) != null) {
          return;
        }
        _setState(
          ready.copyWith(
            incomingOffers: <IncomingTransferOffer>[
              ...ready.incomingOffers,
              event.offer,
            ],
          ),
        );
      case IncomingOfferWithdrawn():
        if (ready == null || ready.offerById(event.offerId) == null) {
          return;
        }
        _setState(
          ready.copyWith(
            incomingOffers: ready.incomingOffers.where(
              (IncomingTransferOffer offer) => offer.id != event.offerId,
            ),
          ),
        );
      case TransferUpdated():
        if (ready == null) {
          return;
        }
        final TransferEntry? current = ready.transferById(event.transfer.id);
        if (current == null || !_isValidUpdate(current, event.transfer)) {
          return;
        }
        _replaceTransfer(ready, event.transfer);
      case TransferGatewayUnavailable():
        _markUnavailable(event.reason);
    }
  }

  void _replaceTransfer(TransferReady ready, TransferEntry replacement) {
    _setState(
      ready.copyWith(
        transfers: ready.transfers.map(
          (TransferEntry transfer) =>
              transfer.id == replacement.id ? replacement : transfer,
        ),
      ),
    );
  }

  bool _isAcceptedTransferValid(
    IncomingTransferOffer offer,
    TransferEntry transfer,
  ) {
    return transfer.direction == TransferDirection.incoming &&
        transfer.peerName == offer.peerName &&
        transfer.fileCount == offer.fileCount &&
        transfer.totalBytes == offer.totalBytes &&
        transfer.transferredBytes == 0 &&
        transfer.status == TransferStatus.queued &&
        transfer.failure == null;
  }

  bool _isValidUpdate(TransferEntry current, TransferEntry next) {
    if (next.direction != current.direction ||
        next.peerName != current.peerName ||
        next.fileCount != current.fileCount ||
        next.totalBytes != current.totalBytes ||
        next.transferredBytes < current.transferredBytes ||
        next.transferredBytes > current.totalBytes ||
        (next.status == TransferStatus.completed &&
            next.transferredBytes != next.totalBytes) ||
        (next.status == TransferStatus.failed) != (next.failure != null)) {
      return false;
    }
    return _isLegalTransition(current.status, next.status);
  }

  bool _isLegalTransition(TransferStatus current, TransferStatus next) {
    if (current == next) {
      return true;
    }
    return switch (current) {
      TransferStatus.queued => next == TransferStatus.running ||
          next == TransferStatus.cancelled ||
          next == TransferStatus.failed,
      TransferStatus.running => next == TransferStatus.paused ||
          next == TransferStatus.cancelled ||
          next == TransferStatus.failed ||
          next == TransferStatus.completed,
      TransferStatus.paused => next == TransferStatus.queued ||
          next == TransferStatus.running ||
          next == TransferStatus.cancelled ||
          next == TransferStatus.failed,
      TransferStatus.cancelled ||
      TransferStatus.failed ||
      TransferStatus.completed =>
        false,
    };
  }

  bool _canCancel(TransferStatus status) {
    return status == TransferStatus.queued ||
        status == TransferStatus.running ||
        status == TransferStatus.paused;
  }

  TransferReady? get _readyState {
    final TransferState current = _state;
    return current is TransferReady ? current : null;
  }

  void _markUnavailable(String reason) {
    if (_disposed || _state is TransferUnavailable) {
      return;
    }
    _setState(TransferUnavailable(reason));
  }

  void _setState(TransferState next) {
    if (_disposed) {
      return;
    }
    _state = next;
    notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    unawaited(_eventSubscription?.cancel());
    try {
      _gateway.dispose();
    } finally {
      super.dispose();
    }
  }
}
