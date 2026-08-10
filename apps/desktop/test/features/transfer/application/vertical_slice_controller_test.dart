import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:xnn_transfer/features/transfer/application/vertical_slice_controller.dart';
import 'package:xnn_transfer/features/transfer/application/vertical_slice_state.dart';
import 'package:xnn_transfer/features/transfer/domain/vertical_slice_gateway.dart';
import 'package:xnn_transfer/features/transfer/domain/vertical_slice_models.dart';

import '../support/fake_vertical_slice_gateway.dart';

const NearbyPeer _peer = NearbyPeer(
  id: 'peer-1',
  displayName: 'Nearby laptop',
  trust: PeerTrust.untrusted,
  isAvailable: true,
);

PairingGatewayAttempt _attempt({
  String id = 'attempt-1',
  PairingGatewayState state = PairingGatewayState.awaitingConfirmation,
  PairingGatewayError error = PairingGatewayError.none,
  List<String> words = const <String>[
    'abandon',
    'ability',
    'able',
    'about',
    'above',
  ],
}) {
  return PairingGatewayAttempt(
    id: id,
    peerId: _peer.id,
    state: state,
    sasWords: words,
    error: error,
  );
}

VerticalSliceReady _ready(VerticalSliceController controller) {
  return controller.state as VerticalSliceReady;
}

Future<VerticalSliceController> _initialized(
  FakeVerticalSliceGateway gateway,
) async {
  final VerticalSliceController controller = VerticalSliceController(gateway);
  addTearDown(controller.dispose);
  await controller.initialize();
  return controller;
}

void main() {
  test('buffers peer events until transfer initialization completes', () async {
    final Completer<void> initialization = Completer<void>();
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway()
      ..initializationCompleter = initialization;
    final VerticalSliceController controller = VerticalSliceController(gateway);
    addTearDown(controller.dispose);

    final Future<void> initializeFuture = controller.initialize();
    gateway.emitVerticalSlice(const NearbyPeerUpserted(_peer));
    expect(controller.state, isA<VerticalSliceInitializing>());

    initialization.complete();
    await initializeFuture;

    expect(_ready(controller).peers, hasLength(1));
    expect(_ready(controller).peers.single.displayName, _peer.displayName);
  });

  test('keeps an initialization-time peer stream failure unavailable',
      () async {
    final Completer<void> initialization = Completer<void>();
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway()
      ..initializationCompleter = initialization;
    final VerticalSliceController controller = VerticalSliceController(gateway);
    addTearDown(controller.dispose);

    final Future<void> initializeFuture = controller.initialize();
    gateway.emitVerticalSliceError(StateError('peer stream stopped'));
    initialization.complete();
    await initializeFuture;

    final VerticalSliceUnavailable state =
        controller.state as VerticalSliceUnavailable;
    expect(state.reason, 'The native peer service stopped unexpectedly.');
  });

  test('removes expired untrusted peers but keeps authenticated peers offline',
      () async {
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway();
    final VerticalSliceController controller = await _initialized(gateway);
    gateway.emitVerticalSlice(const NearbyPeerUpserted(_peer));

    gateway.emitVerticalSlice(const NearbyPeerExpired('peer-1'));
    expect(_ready(controller).peers, isEmpty);

    gateway.emitVerticalSlice(const NearbyPeerUpserted(_peer));
    gateway.emitVerticalSlice(
      const PeerTrustUpdated(peerId: 'peer-1', isActive: true),
    );
    gateway.emitVerticalSlice(const NearbyPeerExpired('peer-1'));

    final NearbyPeer retained = _ready(controller).peers.single;
    expect(retained.trust, PeerTrust.authenticated);
    expect(retained.isAvailable, isFalse);
  });

  test('requires explicit SAS confirmation before showing authenticated trust',
      () async {
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway();
    final VerticalSliceController controller = await _initialized(gateway);
    gateway.emitVerticalSlice(const NearbyPeerUpserted(_peer));

    expect(
      await controller.pairPeer(_peer.id),
      VerticalSliceCommandOutcome.applied,
    );
    gateway.emitVerticalSlice(
      PairingAttemptUpdated(
        _attempt(state: PairingGatewayState.starting, words: const <String>[]),
      ),
    );
    gateway.emitVerticalSlice(PairingAttemptUpdated(_attempt()));

    expect(_ready(controller).pairing?.sasWords, hasLength(5));
    expect(_ready(controller).peers.single.trust, PeerTrust.untrusted);

    expect(
      await controller.confirmPairing(),
      VerticalSliceCommandOutcome.applied,
    );
    expect(gateway.confirmedAttempts, <String>['attempt-1']);

    gateway.emitVerticalSlice(
      PairingAttemptUpdated(
        _attempt(state: PairingGatewayState.paired, words: const <String>[]),
      ),
    );
    expect(
      _ready(controller).pairingMessage,
      'Pairing completed. Waiting for trust activation...',
    );
    expect(_ready(controller).peers.single.trust, PeerTrust.untrusted);
    expect(
      await controller.pairPeer(_peer.id),
      VerticalSliceCommandOutcome.invalidState,
    );

    gateway.emitVerticalSlice(
      const PeerTrustUpdated(peerId: 'peer-1', isActive: true),
    );

    expect(_ready(controller).pairing, isNull);
    expect(_ready(controller).peers.single.trust, PeerTrust.authenticated);
    expect(
      _ready(controller).pairingMessage,
      'Pairing completed. The peer is authenticated.',
    );
  });

  test('reports rejected and failed pairing without exposing gateway details',
      () async {
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway();
    final VerticalSliceController controller = await _initialized(gateway);
    gateway.emitVerticalSlice(const NearbyPeerUpserted(_peer));

    await controller.pairPeer(_peer.id);
    gateway.emitVerticalSlice(PairingAttemptUpdated(_attempt()));
    gateway.emitVerticalSlice(
      PairingAttemptUpdated(
        _attempt(
          state: PairingGatewayState.closed,
          error: PairingGatewayError.rejected,
          words: const <String>[],
        ),
      ),
    );
    expect(_ready(controller).pairingMessage, 'Pairing was rejected.');

    await controller.pairPeer(_peer.id);
    gateway.emitVerticalSlice(PairingAttemptUpdated(_attempt(id: 'attempt-2')));
    gateway.emitVerticalSlice(
      PairingAttemptUpdated(
        _attempt(
          id: 'attempt-2',
          state: PairingGatewayState.closed,
          error: PairingGatewayError.failed,
          words: const <String>[],
        ),
      ),
    );
    expect(_ready(controller).pairingMessage, 'Pairing failed.');
  });

  test('ignores an event for a replaced pairing attempt', () async {
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway();
    final VerticalSliceController controller = await _initialized(gateway);
    gateway.emitVerticalSlice(const NearbyPeerUpserted(_peer));
    await controller.pairPeer(_peer.id);
    gateway.emitVerticalSlice(PairingAttemptUpdated(_attempt()));

    gateway.emitVerticalSlice(
      PairingAttemptUpdated(
        _attempt(
          id: 'stale-attempt',
          state: PairingGatewayState.closed,
          error: PairingGatewayError.failed,
          words: const <String>[],
        ),
      ),
    );

    expect(_ready(controller).pairing?.attemptId, 'attempt-1');
  });

  test('clears an attempt missing from an authoritative recovery snapshot',
      () async {
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway();
    final VerticalSliceController controller = await _initialized(gateway);
    gateway.emitVerticalSlice(const NearbyPeerUpserted(_peer));
    await controller.pairPeer(_peer.id);
    gateway.emitVerticalSlice(PairingAttemptUpdated(_attempt()));

    gateway.emitVerticalSlice(const PairingAttemptCleared('attempt-1'));

    expect(_ready(controller).pairing, isNull);
    expect(
      _ready(controller).pairingMessage,
      'Pairing ended before its final status was received.',
    );
    expect(
      await controller.pairPeer(_peer.id),
      VerticalSliceCommandOutcome.applied,
    );
  });

  test('active trust recovery replaces an empty pairing snapshot warning',
      () async {
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway();
    final VerticalSliceController controller = await _initialized(gateway);
    gateway.emitVerticalSlice(const NearbyPeerUpserted(_peer));
    await controller.pairPeer(_peer.id);
    gateway.emitVerticalSlice(PairingAttemptUpdated(_attempt()));
    gateway.emitVerticalSlice(const PairingAttemptCleared('attempt-1'));

    gateway.emitVerticalSlice(
      const PeerTrustUpdated(peerId: 'peer-1', isActive: true),
    );

    expect(_ready(controller).peers.single.trust, PeerTrust.authenticated);
    expect(
      _ready(controller).pairingMessage,
      'Pairing completed. The peer is authenticated.',
    );
  });

  test('an old reject completion does not clear a newer pairing attempt',
      () async {
    final Completer<void> rejection = Completer<void>();
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway()
      ..rejectionCompleter = rejection;
    final VerticalSliceController controller = await _initialized(gateway);
    gateway.emitVerticalSlice(const NearbyPeerUpserted(_peer));
    await controller.pairPeer(_peer.id);
    gateway.emitVerticalSlice(PairingAttemptUpdated(_attempt()));

    final Future<VerticalSliceCommandOutcome> rejectFuture =
        controller.rejectPairing();
    gateway.emitVerticalSlice(
      PairingAttemptUpdated(
        _attempt(
          state: PairingGatewayState.closed,
          error: PairingGatewayError.rejected,
          words: const <String>[],
        ),
      ),
    );
    await controller.pairPeer(_peer.id);
    gateway.emitVerticalSlice(PairingAttemptUpdated(_attempt(id: 'attempt-2')));

    rejection.complete();
    expect(await rejectFuture, VerticalSliceCommandOutcome.applied);
    expect(_ready(controller).pairing?.attemptId, 'attempt-2');
    expect(_ready(controller).pairingMessage, isNull);
  });

  test('does not start another pairing before rejection becomes terminal',
      () async {
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway();
    final VerticalSliceController controller = await _initialized(gateway);
    gateway.emitVerticalSlice(const NearbyPeerUpserted(_peer));
    await controller.pairPeer(_peer.id);
    gateway.emitVerticalSlice(PairingAttemptUpdated(_attempt()));

    expect(
      await controller.rejectPairing(),
      VerticalSliceCommandOutcome.applied,
    );
    expect(
      await controller.pairPeer(_peer.id),
      VerticalSliceCommandOutcome.invalidState,
    );

    gateway.emitVerticalSlice(
      PairingAttemptUpdated(
        _attempt(
          state: PairingGatewayState.closed,
          error: PairingGatewayError.rejected,
          words: const <String>[],
        ),
      ),
    );
    expect(
      await controller.pairPeer(_peer.id),
      VerticalSliceCommandOutcome.applied,
    );
  });

  test('deduplicates pairing and file selection commands', () async {
    final Completer<void> pairing = Completer<void>();
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway()
      ..pairingCompleter = pairing;
    final VerticalSliceController controller = await _initialized(gateway);
    gateway.emitVerticalSlice(const NearbyPeerUpserted(_peer));

    final Future<VerticalSliceCommandOutcome> firstPair =
        controller.pairPeer(_peer.id);
    expect(
      await controller.pairPeer(_peer.id),
      VerticalSliceCommandOutcome.invalidState,
    );
    pairing.complete();
    expect(await firstPair, VerticalSliceCommandOutcome.applied);
    expect(gateway.pairedPeers, <String>[_peer.id]);

    gateway.emitVerticalSlice(
      const PeerTrustUpdated(peerId: 'peer-1', isActive: true),
    );
    final Completer<SendSelectionOutcome> selection =
        Completer<SendSelectionOutcome>();
    gateway.selectionCompleter = selection;
    final Future<VerticalSliceCommandOutcome> firstSend =
        controller.selectAndSendFile(_peer.id);
    expect(
      await controller.selectAndSendFile(_peer.id),
      VerticalSliceCommandOutcome.invalidState,
    );
    selection.complete(SendSelectionOutcome.submitted);
    expect(await firstSend, VerticalSliceCommandOutcome.applied);
    expect(gateway.selectedPeers, <String>[_peer.id]);
  });

  test('ignores buffered events after disposal', () async {
    final Completer<void> initialization = Completer<void>();
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway()
      ..initializationCompleter = initialization;
    final VerticalSliceController controller = VerticalSliceController(gateway);
    int notifications = 0;
    controller.addListener(() {
      notifications += 1;
    });

    final Future<void> initializeFuture = controller.initialize();
    gateway.emitVerticalSlice(const NearbyPeerUpserted(_peer));
    controller.dispose();
    initialization.complete();
    await initializeFuture;

    expect(notifications, 0);
    expect(gateway.disposed, isTrue);
  });
}
