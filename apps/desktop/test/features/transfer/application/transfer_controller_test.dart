import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:xnn_transfer/features/transfer/application/transfer_controller.dart';
import 'package:xnn_transfer/features/transfer/application/transfer_state.dart';
import 'package:xnn_transfer/features/transfer/domain/transfer_gateway.dart';
import 'package:xnn_transfer/features/transfer/domain/transfer_models.dart';

const IncomingTransferOffer _offer = IncomingTransferOffer(
  id: 'offer-1',
  peerName: 'Nearby laptop',
  fileCount: 2,
  totalBytes: 100,
);

const IncomingTransferOffer _secondOffer = IncomingTransferOffer(
  id: 'offer-2',
  peerName: 'Desktop',
  fileCount: 1,
  totalBytes: 50,
);

TransferEntry _transfer({
  String id = 'transfer-1',
  int transferredBytes = 0,
  TransferStatus status = TransferStatus.queued,
  TransferFailure? failure,
}) {
  return TransferEntry(
    id: id,
    direction: TransferDirection.incoming,
    peerName: _offer.peerName,
    fileCount: _offer.fileCount,
    totalBytes: _offer.totalBytes,
    transferredBytes: transferredBytes,
    status: status,
    failure: failure,
  );
}

TransferReady _readyState(TransferController controller) {
  return controller.state as TransferReady;
}

Future<TransferController> _initializedController(
  _FakeTransferGateway gateway,
) async {
  final TransferController controller = TransferController(gateway);
  addTearDown(controller.dispose);
  await controller.initialize();
  return controller;
}

Future<TransferController> _controllerWithQueuedTransfer(
  _FakeTransferGateway gateway,
) async {
  final TransferController controller = await _initializedController(gateway);
  gateway.emit(const IncomingOfferReceived(_offer));
  gateway.acceptedTransfer = _transfer();

  final TransferCommandOutcome outcome =
      await controller.acceptOffer(_offer.id);
  expect(outcome, TransferCommandOutcome.applied);
  return controller;
}

void main() {
  group('availability', () {
    test('moves from initializing to ready once', () async {
      final _FakeTransferGateway gateway = _FakeTransferGateway();
      final TransferController controller = TransferController(gateway);
      addTearDown(controller.dispose);

      expect(controller.state, isA<TransferInitializing>());

      await controller.initialize();
      await controller.initialize();

      expect(controller.state, isA<TransferReady>());
      expect(gateway.initializeCalls, 1);
    });

    test('reports initialization failure as unavailable', () async {
      final _FakeTransferGateway gateway = _FakeTransferGateway()
        ..initializeError = StateError('native adapter unavailable');
      final TransferController controller = TransferController(gateway);
      addTearDown(controller.dispose);

      await controller.initialize();

      final TransferUnavailable state = controller.state as TransferUnavailable;
      expect(state.reason, contains('native adapter unavailable'));
    });

    test('reports a gateway unavailable event', () async {
      final _FakeTransferGateway gateway = _FakeTransferGateway();
      final TransferController controller =
          await _initializedController(gateway);

      gateway.emit(const TransferGatewayUnavailable('adapter disconnected'));

      final TransferUnavailable state = controller.state as TransferUnavailable;
      expect(state.reason, 'adapter disconnected');
    });

    test('replays synchronous initialization events in order', () async {
      final _FakeTransferGateway gateway = _FakeTransferGateway()
        ..initializationEvents.addAll(<TransferGatewayEvent>[
          const IncomingOfferReceived(_offer),
          IncomingOfferWithdrawn(_offer.id),
          const IncomingOfferReceived(_secondOffer),
        ]);
      final TransferController controller = TransferController(gateway);
      addTearDown(controller.dispose);

      await controller.initialize();

      final TransferReady state = _readyState(controller);
      expect(
        state.incomingOffers.map((IncomingTransferOffer offer) => offer.id),
        <String>[_secondOffer.id],
      );
    });

    test('replays events emitted while asynchronous initialization is pending',
        () async {
      final Completer<void> initialization = Completer<void>();
      final _FakeTransferGateway gateway = _FakeTransferGateway()
        ..initializationCompleter = initialization;
      final TransferController controller = TransferController(gateway);
      addTearDown(controller.dispose);

      final Future<void> initializeFuture = controller.initialize();
      gateway.emit(const IncomingOfferReceived(_offer));
      expect(controller.state, isA<TransferInitializing>());

      initialization.complete();
      await initializeFuture;

      expect(_readyState(controller).incomingOffers, <IncomingTransferOffer>[
        _offer,
      ]);
    });

    test('fails closed on unavailable while initialization is pending',
        () async {
      final Completer<void> initialization = Completer<void>();
      final _FakeTransferGateway gateway = _FakeTransferGateway()
        ..initializationCompleter = initialization;
      final TransferController controller = TransferController(gateway);
      addTearDown(controller.dispose);

      final Future<void> initializeFuture = controller.initialize();
      gateway.emit(const IncomingOfferReceived(_offer));
      gateway.emit(
        const TransferGatewayUnavailable('adapter stopped during startup'),
      );
      gateway.emit(const IncomingOfferReceived(_secondOffer));
      initialization.complete();
      await initializeFuture;

      final TransferUnavailable state = controller.state as TransferUnavailable;
      expect(state.reason, 'adapter stopped during startup');
    });

    test('does not replay buffered events after dispose', () async {
      final Completer<void> initialization = Completer<void>();
      final _FakeTransferGateway gateway = _FakeTransferGateway()
        ..initializationCompleter = initialization;
      final TransferController controller = TransferController(gateway);
      int notifications = 0;
      controller.addListener(() {
        notifications += 1;
      });

      final Future<void> initializeFuture = controller.initialize();
      gateway.emit(const IncomingOfferReceived(_offer));
      controller.dispose();
      initialization.complete();
      await initializeFuture;

      expect(controller.state, isA<TransferInitializing>());
      expect(notifications, 0);
      expect(gateway.disposed, isTrue);
    });
  });

  group('immutable ready state', () {
    test('defensively copies and exposes unmodifiable collections', () {
      final List<IncomingTransferOffer> offers = <IncomingTransferOffer>[
        _offer,
      ];
      final List<TransferEntry> transfers = <TransferEntry>[_transfer()];
      final TransferReady state = TransferReady(
        incomingOffers: offers,
        transfers: transfers,
      );

      offers.clear();
      transfers.clear();

      expect(state.incomingOffers, hasLength(1));
      expect(state.transfers, hasLength(1));
      expect(
        () => state.incomingOffers.add(_offer),
        throwsUnsupportedError,
      );
      expect(
        () => state.transfers.add(_transfer(id: 'another-transfer')),
        throwsUnsupportedError,
      );
    });
  });

  group('incoming offers', () {
    test('accepts an offer and creates a queued transfer', () async {
      final _FakeTransferGateway gateway = _FakeTransferGateway();
      final TransferController controller =
          await _initializedController(gateway);
      gateway.emit(const IncomingOfferReceived(_offer));
      gateway.acceptedTransfer = _transfer();

      final TransferCommandOutcome outcome =
          await controller.acceptOffer(_offer.id);

      final TransferReady state = _readyState(controller);
      expect(outcome, TransferCommandOutcome.applied);
      expect(gateway.acceptedOffers, <String>[_offer.id]);
      expect(state.incomingOffers, isEmpty);
      expect(state.transfers.single.status, TransferStatus.queued);
    });

    test('rejects an offer without creating a transfer', () async {
      final _FakeTransferGateway gateway = _FakeTransferGateway();
      final TransferController controller =
          await _initializedController(gateway);
      gateway.emit(const IncomingOfferReceived(_offer));

      final TransferCommandOutcome outcome =
          await controller.rejectOffer(_offer.id);

      final TransferReady state = _readyState(controller);
      expect(outcome, TransferCommandOutcome.applied);
      expect(gateway.rejectedOffers, <String>[_offer.id]);
      expect(state.incomingOffers, isEmpty);
      expect(state.transfers, isEmpty);
    });

    test('removes an offer withdrawn by the gateway', () async {
      final _FakeTransferGateway gateway = _FakeTransferGateway();
      final TransferController controller =
          await _initializedController(gateway);
      gateway.emit(const IncomingOfferReceived(_offer));

      gateway.emit(IncomingOfferWithdrawn(_offer.id));

      expect(_readyState(controller).incomingOffers, isEmpty);
    });

    test('rejects commands for unknown offers without calling gateway',
        () async {
      final _FakeTransferGateway gateway = _FakeTransferGateway();
      final TransferController controller =
          await _initializedController(gateway);

      expect(
        await controller.acceptOffer('missing'),
        TransferCommandOutcome.invalidState,
      );
      expect(
        await controller.rejectOffer('missing'),
        TransferCommandOutcome.invalidState,
      );
      expect(gateway.acceptedOffers, isEmpty);
      expect(gateway.rejectedOffers, isEmpty);
    });

    test('fails closed when acceptance violates the gateway contract',
        () async {
      final _FakeTransferGateway gateway = _FakeTransferGateway();
      final TransferController controller =
          await _initializedController(gateway);
      gateway.emit(const IncomingOfferReceived(_offer));
      gateway.acceptedTransfer = _transfer(status: TransferStatus.running);

      final TransferCommandOutcome outcome =
          await controller.acceptOffer(_offer.id);

      expect(outcome, TransferCommandOutcome.gatewayError);
      expect(controller.state, isA<TransferUnavailable>());
    });

    test('tracks an accepted offer withdrawn while command is pending',
        () async {
      final Completer<TransferEntry> acceptance = Completer<TransferEntry>();
      final _FakeTransferGateway gateway = _FakeTransferGateway()
        ..acceptCompleter = acceptance;
      final TransferController controller =
          await _initializedController(gateway);
      gateway.emit(const IncomingOfferReceived(_offer));

      final Future<TransferCommandOutcome> acceptFuture =
          controller.acceptOffer(_offer.id);
      gateway.emit(IncomingOfferWithdrawn(_offer.id));

      expect(_readyState(controller).incomingOffers, <IncomingTransferOffer>[
        _offer,
      ]);
      acceptance.complete(_transfer());
      expect(await acceptFuture, TransferCommandOutcome.applied);

      final TransferReady state = _readyState(controller);
      expect(state.incomingOffers, isEmpty);
      expect(state.transfers.single.id, 'transfer-1');
    });

    test('removes a rejected offer withdrawn while command is pending',
        () async {
      final Completer<void> rejection = Completer<void>();
      final _FakeTransferGateway gateway = _FakeTransferGateway()
        ..rejectCompleter = rejection;
      final TransferController controller =
          await _initializedController(gateway);
      gateway.emit(const IncomingOfferReceived(_offer));

      final Future<TransferCommandOutcome> rejectFuture =
          controller.rejectOffer(_offer.id);
      gateway.emit(IncomingOfferWithdrawn(_offer.id));

      expect(_readyState(controller).incomingOffers, <IncomingTransferOffer>[
        _offer,
      ]);
      rejection.complete();
      expect(await rejectFuture, TransferCommandOutcome.applied);
      expect(_readyState(controller).incomingOffers, isEmpty);
    });

    test('applies deferred withdrawal after a command failure', () async {
      final Completer<void> rejection = Completer<void>();
      final _FakeTransferGateway gateway = _FakeTransferGateway()
        ..rejectCompleter = rejection;
      final TransferController controller =
          await _initializedController(gateway);
      gateway.emit(const IncomingOfferReceived(_offer));

      final Future<TransferCommandOutcome> rejectFuture =
          controller.rejectOffer(_offer.id);
      gateway.emit(IncomingOfferWithdrawn(_offer.id));
      rejection.completeError(StateError('offer already withdrawn'));

      expect(await rejectFuture, TransferCommandOutcome.gatewayError);
      expect(_readyState(controller).incomingOffers, isEmpty);
    });
  });

  group('transfer lifecycle', () {
    test('supports queued, running, paused, resumed, and cancelled', () async {
      final _FakeTransferGateway gateway = _FakeTransferGateway();
      final TransferController controller =
          await _controllerWithQueuedTransfer(gateway);

      expect(
        _readyState(controller).transfers.single.status,
        TransferStatus.queued,
      );

      gateway.emit(
        TransferUpdated(
          _transfer(
            transferredBytes: 20,
            status: TransferStatus.running,
          ),
        ),
      );
      expect(
        _readyState(controller).transfers.single.status,
        TransferStatus.running,
      );

      expect(
        await controller.pauseTransfer('transfer-1'),
        TransferCommandOutcome.applied,
      );
      expect(
        _readyState(controller).transfers.single.status,
        TransferStatus.paused,
      );

      expect(
        await controller.resumeTransfer('transfer-1'),
        TransferCommandOutcome.applied,
      );
      expect(
        _readyState(controller).transfers.single.status,
        TransferStatus.queued,
      );

      gateway.emit(
        TransferUpdated(
          _transfer(
            transferredBytes: 20,
            status: TransferStatus.running,
          ),
        ),
      );
      expect(
        await controller.cancelTransfer('transfer-1'),
        TransferCommandOutcome.applied,
      );
      expect(
        _readyState(controller).transfers.single.status,
        TransferStatus.cancelled,
      );
      expect(gateway.pausedTransfers, <String>['transfer-1']);
      expect(gateway.resumedTransfers, <String>['transfer-1']);
      expect(gateway.cancelledTransfers, <String>['transfer-1']);
    });

    test('accepts a failed update with a structured failure', () async {
      final _FakeTransferGateway gateway = _FakeTransferGateway();
      final TransferController controller =
          await _controllerWithQueuedTransfer(gateway);
      gateway.emit(
        TransferUpdated(
          _transfer(
            transferredBytes: 30,
            status: TransferStatus.running,
          ),
        ),
      );

      gateway.emit(
        TransferUpdated(
          _transfer(
            transferredBytes: 30,
            status: TransferStatus.failed,
            failure: const TransferFailure(
              code: 'transport_closed',
              message: 'Transfer transport closed',
            ),
          ),
        ),
      );

      final TransferEntry transfer = _readyState(controller).transfers.single;
      expect(transfer.status, TransferStatus.failed);
      expect(transfer.failure?.code, 'transport_closed');
    });

    test('accepts completion only after a running transfer reaches total bytes',
        () async {
      final _FakeTransferGateway gateway = _FakeTransferGateway();
      final TransferController controller =
          await _controllerWithQueuedTransfer(gateway);
      gateway.emit(
        TransferUpdated(
          _transfer(
            transferredBytes: 70,
            status: TransferStatus.running,
          ),
        ),
      );

      gateway.emit(
        TransferUpdated(
          _transfer(
            transferredBytes: 100,
            status: TransferStatus.completed,
          ),
        ),
      );

      final TransferEntry transfer = _readyState(controller).transfers.single;
      expect(transfer.status, TransferStatus.completed);
      expect(transfer.transferredBytes, transfer.totalBytes);
    });
  });

  group('illegal transitions', () {
    test('rejects commands that do not match the current status', () async {
      final _FakeTransferGateway gateway = _FakeTransferGateway();
      final TransferController controller =
          await _controllerWithQueuedTransfer(gateway);

      expect(
        await controller.pauseTransfer('transfer-1'),
        TransferCommandOutcome.invalidState,
      );
      expect(
        await controller.resumeTransfer('transfer-1'),
        TransferCommandOutcome.invalidState,
      );
      expect(gateway.pausedTransfers, isEmpty);
      expect(gateway.resumedTransfers, isEmpty);
    });

    test('ignores illegal gateway status and progress updates', () async {
      final _FakeTransferGateway gateway = _FakeTransferGateway();
      final TransferController controller =
          await _controllerWithQueuedTransfer(gateway);

      gateway.emit(
        TransferUpdated(
          _transfer(
            transferredBytes: 100,
            status: TransferStatus.completed,
          ),
        ),
      );
      expect(
        _readyState(controller).transfers.single.status,
        TransferStatus.queued,
      );

      gateway.emit(
        TransferUpdated(
          _transfer(
            transferredBytes: 50,
            status: TransferStatus.running,
          ),
        ),
      );
      gateway.emit(
        TransferUpdated(
          _transfer(
            transferredBytes: 40,
            status: TransferStatus.running,
          ),
        ),
      );

      final TransferEntry transfer = _readyState(controller).transfers.single;
      expect(transfer.status, TransferStatus.running);
      expect(transfer.transferredBytes, 50);
    });

    test('does not leave a terminal state or call a terminal command',
        () async {
      final _FakeTransferGateway gateway = _FakeTransferGateway();
      final TransferController controller =
          await _controllerWithQueuedTransfer(gateway);
      gateway.emit(
        TransferUpdated(
          _transfer(
            transferredBytes: 50,
            status: TransferStatus.running,
          ),
        ),
      );
      gateway.emit(
        TransferUpdated(
          _transfer(
            transferredBytes: 100,
            status: TransferStatus.completed,
          ),
        ),
      );

      gateway.emit(
        TransferUpdated(
          _transfer(
            transferredBytes: 100,
            status: TransferStatus.running,
          ),
        ),
      );
      expect(
        await controller.cancelTransfer('transfer-1'),
        TransferCommandOutcome.invalidState,
      );

      expect(
        _readyState(controller).transfers.single.status,
        TransferStatus.completed,
      );
      expect(gateway.cancelledTransfers, isEmpty);
    });

    test('keeps state when a gateway command fails', () async {
      final _FakeTransferGateway gateway = _FakeTransferGateway();
      final TransferController controller =
          await _controllerWithQueuedTransfer(gateway);
      gateway.emit(
        TransferUpdated(
          _transfer(
            transferredBytes: 20,
            status: TransferStatus.running,
          ),
        ),
      );
      gateway.commandError = StateError('pause unavailable');

      expect(
        await controller.pauseTransfer('transfer-1'),
        TransferCommandOutcome.gatewayError,
      );
      expect(
        _readyState(controller).transfers.single.status,
        TransferStatus.running,
      );
    });
  });
}

final class _FakeTransferGateway implements TransferGateway {
  final StreamController<TransferGatewayEvent> _events =
      StreamController<TransferGatewayEvent>.broadcast(sync: true);

  final List<String> acceptedOffers = <String>[];
  final List<String> rejectedOffers = <String>[];
  final List<String> pausedTransfers = <String>[];
  final List<String> resumedTransfers = <String>[];
  final List<String> cancelledTransfers = <String>[];

  Object? initializeError;
  Object? commandError;
  TransferEntry? acceptedTransfer;
  Completer<void>? initializationCompleter;
  Completer<TransferEntry>? acceptCompleter;
  Completer<void>? rejectCompleter;
  final List<TransferGatewayEvent> initializationEvents =
      <TransferGatewayEvent>[];
  int initializeCalls = 0;
  bool disposed = false;

  @override
  Stream<TransferGatewayEvent> get events => _events.stream;

  void emit(TransferGatewayEvent event) {
    _events.add(event);
  }

  @override
  Future<void> initialize() async {
    initializeCalls += 1;
    for (final TransferGatewayEvent event in initializationEvents) {
      emit(event);
    }
    final Completer<void>? completer = initializationCompleter;
    if (completer != null) {
      await completer.future;
    }
    final Object? error = initializeError;
    if (error != null) {
      throw error;
    }
  }

  @override
  Future<TransferEntry> acceptOffer(String offerId) async {
    acceptedOffers.add(offerId);
    _throwCommandError();
    final Completer<TransferEntry>? completer = acceptCompleter;
    if (completer != null) {
      return completer.future;
    }
    return acceptedTransfer ??
        (throw StateError('No accepted transfer configured'));
  }

  @override
  Future<void> rejectOffer(String offerId) async {
    rejectedOffers.add(offerId);
    _throwCommandError();
    final Completer<void>? completer = rejectCompleter;
    if (completer != null) {
      await completer.future;
    }
  }

  @override
  Future<void> pauseTransfer(String transferId) async {
    pausedTransfers.add(transferId);
    _throwCommandError();
  }

  @override
  Future<void> resumeTransfer(String transferId) async {
    resumedTransfers.add(transferId);
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
    unawaited(_events.close());
  }
}
