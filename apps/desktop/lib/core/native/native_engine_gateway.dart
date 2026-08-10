import 'dart:async';

import 'package:xnn_transfer/core/native/native_engine.dart';
import 'package:xnn_transfer/core/native/native_event_decoder.dart';
import 'package:xnn_transfer/features/transfer/domain/engine_gateway.dart';
import 'package:xnn_transfer/features/transfer/domain/transfer_gateway.dart';
import 'package:xnn_transfer/features/transfer/domain/transfer_models.dart';

class NativeEngineGateway implements EngineGateway {
  NativeEngine? _engine;

  @override
  void initialize() {
    final NativeEngine engine = _engine ??= NativeEngine.open();
    engine.initialize();
  }

  @override
  void start() {
    _requireEngine().start();
  }

  @override
  void stop() {
    _engine?.stop();
  }

  @override
  void dispose() {
    _engine?.dispose();
    _engine = null;
  }

  NativeEngine _requireEngine() {
    final NativeEngine? engine = _engine;
    if (engine == null) {
      throw StateError('Native engine gateway is not initialized');
    }
    return engine;
  }
}

final class NativeTransferGatewayException implements Exception {
  const NativeTransferGatewayException(this.code, this.message);

  final String code;
  final String message;

  @override
  String toString() => 'NativeTransferGatewayException($code, $message)';
}

class NativeTransferGateway implements TransferGateway {
  NativeTransferGateway({
    NativeEngine? engine,
    bool manageEngineLifecycle = true,
  })  : _engine = engine ?? NativeEngine.open(),
        _manageEngineLifecycle = manageEngineLifecycle;

  final NativeEngine _engine;
  final bool _manageEngineLifecycle;
  final StreamController<TransferGatewayEvent> _events =
      StreamController<TransferGatewayEvent>.broadcast();
  final Map<String, NativeTransferRecord> _records =
      <String, NativeTransferRecord>{};
  final List<StreamSubscription<Object?>> _subscriptions =
      <StreamSubscription<Object?>>[];

  int _revision = 0;
  bool _initialized = false;
  bool _disposed = false;

  @override
  Stream<TransferGatewayEvent> get events => _events.stream;

  @override
  Future<void> initialize() async {
    if (_disposed) {
      throw const NativeTransferGatewayException(
        'invalid_state',
        'Transfer gateway is disposed',
      );
    }
    if (_initialized) {
      return;
    }

    try {
      _engine.initialize();
      _subscriptions
        ..add(
          _engine.transferEvents.listen(
            _onTransferEvent,
            onError: _onNativeStreamError,
          ),
        )
        ..add(
          _engine.transferSnapshots.listen(
            _reconcileSnapshot,
            onError: _onNativeStreamError,
          ),
        );
      _engine.start();
      _reconcileSnapshot(_engine.transferSnapshot());
      _initialized = true;
    } on Object catch (error) {
      await _cancelSubscriptions();
      throw _mapCommandError(error, 'initialize');
    }
  }

  @override
  Future<TransferEntry> acceptOffer(String offerId) async {
    final NativeTransferRecord record = _requireRecord(
      offerId,
      expectedState: NativeTransferState.offered,
    );
    try {
      _engine.acceptTransfer(record.transferId);
      return _toTransferEntry(
        _copyRecordWithState(record, NativeTransferState.queued),
      );
    } on Object catch (error) {
      throw _mapCommandError(error, 'accept');
    }
  }

  @override
  Future<void> rejectOffer(String offerId) async {
    final NativeTransferRecord record = _requireRecord(
      offerId,
      expectedState: NativeTransferState.offered,
    );
    try {
      _engine.rejectTransfer(record.transferId);
    } on Object catch (error) {
      throw _mapCommandError(error, 'reject');
    }
  }

  @override
  Future<void> pauseTransfer(String transferId) {
    return Future<void>.error(
      const NativeTransferGatewayException(
        'unsupported_operation',
        'Native transfers cannot be paused',
      ),
    );
  }

  @override
  Future<void> resumeTransfer(String transferId) {
    return Future<void>.error(
      const NativeTransferGatewayException(
        'unsupported_operation',
        'Native transfers cannot be resumed',
      ),
    );
  }

  @override
  Future<void> cancelTransfer(String transferId) async {
    final NativeTransferRecord record = _requireRecord(transferId);
    if (_isTerminal(record.state) ||
        record.state == NativeTransferState.offered) {
      throw const NativeTransferGatewayException(
        'stale_handle',
        'Transfer is not active',
      );
    }
    try {
      _engine.cancelTransfer(record.transferId);
    } on Object catch (error) {
      throw _mapCommandError(error, 'cancel');
    }
  }

  void _onTransferEvent(NativeTransferEvent event) {
    if (_disposed) {
      return;
    }
    if (event.eventsDroppedBefore) {
      return;
    }
    if (event.record.revision <= _revision) {
      return;
    }
    if (_revision != 0 && event.record.revision != _revision + 1) {
      try {
        _reconcileSnapshot(_engine.transferSnapshot());
      } on Object {
        _markUnavailable('transfer_snapshot_failed');
      }
      return;
    }

    _revision = event.record.revision;
    _applyRecord(event.record);
  }

  void _applyRecord(NativeTransferRecord record) {
    final String id = _encodeTransferId(record.transferId);
    final NativeTransferRecord? previous = _records[id];
    if (record.change == NativeTransferChange.removed) {
      _records.remove(id);
      if (previous?.state == NativeTransferState.offered) {
        _events.add(IncomingOfferWithdrawn(id));
      }
      return;
    }

    _records[id] = record;
    if (record.state == NativeTransferState.offered) {
      if (previous?.state != NativeTransferState.offered) {
        _events.add(IncomingOfferReceived(_toIncomingOffer(record)));
      }
      return;
    }
    if (previous?.state == NativeTransferState.offered) {
      _events.add(IncomingOfferWithdrawn(id));
    }
    _events.add(TransferUpdated(_toTransferEntry(record)));
  }

  void _reconcileSnapshot(NativeTransferSnapshot snapshot) {
    if (_disposed || snapshot.revision < _revision) {
      return;
    }
    final Map<String, NativeTransferRecord> next =
        <String, NativeTransferRecord>{
      for (final NativeTransferRecord record in snapshot.records)
        _encodeTransferId(record.transferId): record,
    };

    for (final MapEntry<String, NativeTransferRecord> previous
        in _records.entries) {
      if (!next.containsKey(previous.key) &&
          previous.value.state == NativeTransferState.offered) {
        _events.add(IncomingOfferWithdrawn(previous.key));
      }
    }
    _records
      ..clear()
      ..addAll(next);
    _revision = snapshot.revision;
    for (final NativeTransferRecord record in snapshot.records) {
      if (record.state == NativeTransferState.offered) {
        _events.add(IncomingOfferReceived(_toIncomingOffer(record)));
      } else {
        _events.add(TransferUpdated(_toTransferEntry(record)));
      }
    }
  }

  IncomingTransferOffer _toIncomingOffer(NativeTransferRecord record) {
    return IncomingTransferOffer(
      id: _encodeTransferId(record.transferId),
      peerName: _peerName(record),
      fileCount: 1,
      totalBytes: record.totalBytes,
    );
  }

  TransferEntry _toTransferEntry(NativeTransferRecord record) {
    final TransferStatus status = switch (record.state) {
      NativeTransferState.offered ||
      NativeTransferState.queued =>
        TransferStatus.queued,
      NativeTransferState.running ||
      NativeTransferState.cancelling =>
        TransferStatus.running,
      NativeTransferState.completed => TransferStatus.completed,
      NativeTransferState.cancelled => TransferStatus.cancelled,
      NativeTransferState.rejected ||
      NativeTransferState.failed =>
        TransferStatus.failed,
    };
    return TransferEntry(
      id: _encodeTransferId(record.transferId),
      direction: record.direction == NativeTransferDirection.incoming
          ? TransferDirection.incoming
          : TransferDirection.outgoing,
      peerName: _peerName(record),
      fileCount: 1,
      totalBytes: record.totalBytes,
      transferredBytes: record.transferredBytes,
      status: status,
      failure: status == TransferStatus.failed
          ? TransferFailure(
              code: _errorCode(record.error),
              message: 'Native transfer failed',
            )
          : null,
    );
  }

  NativeTransferRecord _requireRecord(
    String id, {
    NativeTransferState? expectedState,
  }) {
    final NativeTransferRecord? record = _records[id];
    if (record == null ||
        (expectedState != null && record.state != expectedState)) {
      throw const NativeTransferGatewayException(
        'stale_handle',
        'Transfer ID is stale or unknown',
      );
    }
    return record;
  }

  NativeTransferRecord _copyRecordWithState(
    NativeTransferRecord record,
    NativeTransferState state,
  ) {
    return NativeTransferRecord(
      change: record.change,
      revision: record.revision,
      direction: record.direction,
      state: state,
      error: NativeTransferError.none,
      totalBytes: record.totalBytes,
      transferredBytes: record.transferredBytes,
      transferId: record.transferId,
      peerLabel: record.peerLabel,
    );
  }

  String _peerName(NativeTransferRecord record) {
    return record.peerLabel.isEmpty ? 'Trusted peer' : record.peerLabel;
  }

  String _encodeTransferId(List<int> transferId) {
    return transferId
        .map((int value) => value.toRadixString(16).padLeft(2, '0'))
        .join();
  }

  String _errorCode(NativeTransferError error) => switch (error) {
        NativeTransferError.none => 'none',
        NativeTransferError.rejected => 'rejected',
        NativeTransferError.cancelled => 'cancelled',
        NativeTransferError.timedOut => 'timed_out',
        NativeTransferError.busy => 'busy',
        NativeTransferError.noSpace => 'no_space',
        NativeTransferError.policyRejected => 'policy_rejected',
        NativeTransferError.ioFailure => 'io_failure',
        NativeTransferError.integrityFailed => 'integrity_failed',
        NativeTransferError.unavailable => 'unavailable',
        NativeTransferError.failed => 'failed',
      };

  bool _isTerminal(NativeTransferState state) {
    return state == NativeTransferState.completed ||
        state == NativeTransferState.cancelled ||
        state == NativeTransferState.rejected ||
        state == NativeTransferState.failed;
  }

  NativeTransferGatewayException _mapCommandError(
    Object error,
    String operation,
  ) {
    if (error is NativeTransferGatewayException) {
      return error;
    }
    if (error is NativeEngineOperationException) {
      return NativeTransferGatewayException(
        error.code,
        'Native transfer $operation failed',
      );
    }
    return NativeTransferGatewayException(
      'native_error',
      'Native transfer $operation failed',
    );
  }

  void _onNativeStreamError(Object error, StackTrace stackTrace) {
    _markUnavailable('native_transfer_event_error');
  }

  void _markUnavailable(String reason) {
    if (!_disposed) {
      _events.add(TransferGatewayUnavailable(reason));
    }
  }

  Future<void> _cancelSubscriptions() async {
    final List<StreamSubscription<Object?>> subscriptions =
        List<StreamSubscription<Object?>>.of(_subscriptions);
    _subscriptions.clear();
    await Future.wait<void>(
      subscriptions.map(
        (StreamSubscription<Object?> subscription) => subscription.cancel(),
      ),
    );
  }

  @override
  void dispose() {
    if (_disposed) {
      return;
    }
    _disposed = true;
    unawaited(_cancelSubscriptions());
    if (_manageEngineLifecycle) {
      _engine.dispose();
    }
    unawaited(_events.close());
  }
}
