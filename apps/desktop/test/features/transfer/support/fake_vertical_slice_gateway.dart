import 'dart:async';

import 'package:xnn_transfer/features/transfer/domain/transfer_gateway.dart';
import 'package:xnn_transfer/features/transfer/domain/transfer_models.dart';
import 'package:xnn_transfer/features/transfer/domain/vertical_slice_gateway.dart';
import 'package:xnn_transfer/features/transfer/domain/vertical_slice_models.dart';

final class FakeVerticalSliceGateway implements VerticalSliceGateway {
  final StreamController<TransferGatewayEvent> _transferEvents =
      StreamController<TransferGatewayEvent>.broadcast(sync: true);
  final StreamController<VerticalSliceGatewayEvent> _verticalSliceEvents =
      StreamController<VerticalSliceGatewayEvent>.broadcast(sync: true);

  final List<String> pairedPeers = <String>[];
  final List<String> confirmedAttempts = <String>[];
  final List<String> rejectedAttempts = <String>[];
  final List<String> selectedPeers = <String>[];
  final List<String> acceptedOffers = <String>[];
  final List<String> rejectedOffers = <String>[];
  final List<String> cancelledTransfers = <String>[];

  Completer<void>? initializationCompleter;
  Completer<void>? pairingCompleter;
  Completer<void>? rejectionCompleter;
  Completer<SendSelectionOutcome>? selectionCompleter;
  Object? initializationError;
  Object? commandError;
  TransferEntry? acceptedTransfer;
  SendSelectionOutcome selectionOutcome = SendSelectionOutcome.submitted;
  int initializeCalls = 0;
  bool disposed = false;

  @override
  Stream<TransferGatewayEvent> get events => _transferEvents.stream;

  @override
  Stream<VerticalSliceGatewayEvent> get verticalSliceEvents =>
      _verticalSliceEvents.stream;

  void emitTransfer(TransferGatewayEvent event) {
    _transferEvents.add(event);
  }

  void emitVerticalSlice(VerticalSliceGatewayEvent event) {
    _verticalSliceEvents.add(event);
  }

  void emitVerticalSliceError(Object error) {
    _verticalSliceEvents.addError(error);
  }

  @override
  Future<void> initialize() async {
    initializeCalls += 1;
    final Completer<void>? completer = initializationCompleter;
    if (completer != null) {
      await completer.future;
    }
    final Object? error = initializationError;
    if (error != null) {
      throw error;
    }
  }

  @override
  Future<void> startPairing(String peerId) async {
    pairedPeers.add(peerId);
    _throwCommandError();
    final Completer<void>? completer = pairingCompleter;
    if (completer != null) {
      await completer.future;
    }
  }

  @override
  Future<void> confirmPairing(String attemptId) async {
    confirmedAttempts.add(attemptId);
    _throwCommandError();
  }

  @override
  Future<void> rejectPairing(String attemptId) async {
    rejectedAttempts.add(attemptId);
    _throwCommandError();
    final Completer<void>? completer = rejectionCompleter;
    if (completer != null) {
      await completer.future;
    }
  }

  @override
  Future<SendSelectionOutcome> selectAndSendFile(String peerId) async {
    selectedPeers.add(peerId);
    _throwCommandError();
    final Completer<SendSelectionOutcome>? completer = selectionCompleter;
    return completer == null ? selectionOutcome : completer.future;
  }

  @override
  Future<TransferEntry> acceptOffer(String offerId) async {
    acceptedOffers.add(offerId);
    _throwCommandError();
    return acceptedTransfer ??
        (throw StateError('No accepted transfer was configured'));
  }

  @override
  Future<void> rejectOffer(String offerId) async {
    rejectedOffers.add(offerId);
    _throwCommandError();
  }

  @override
  Future<void> pauseTransfer(String transferId) async {
    _throwCommandError();
  }

  @override
  Future<void> resumeTransfer(String transferId) async {
    _throwCommandError();
  }

  @override
  Future<void> cancelTransfer(String transferId) async {
    cancelledTransfers.add(transferId);
    _throwCommandError();
  }

  void _throwCommandError() {
    final Object? error = commandError;
    commandError = null;
    if (error != null) {
      throw error;
    }
  }

  @override
  void dispose() {
    if (disposed) {
      return;
    }
    disposed = true;
    unawaited(_transferEvents.close());
    unawaited(_verticalSliceEvents.close());
  }
}
