import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:xnn_transfer/app/native_vertical_slice_gateway.dart';
import 'package:xnn_transfer/core/native/native_engine.dart';
import 'package:xnn_transfer/core/native/native_event_decoder.dart';
import 'package:xnn_transfer/features/transfer/domain/vertical_slice_gateway.dart';

void main() {
  test('an empty recovery snapshot clears the published pairing attempt',
      () async {
    final _FakeNativeEngine engine = _FakeNativeEngine();
    final NativeVerticalSliceGateway gateway = NativeVerticalSliceGateway(
      engine: engine,
    );
    final List<VerticalSliceGatewayEvent> events =
        <VerticalSliceGatewayEvent>[];
    final StreamSubscription<VerticalSliceGatewayEvent> subscription =
        gateway.verticalSliceEvents.listen(events.add);
    addTearDown(() async {
      await subscription.cancel();
      gateway.dispose();
    });
    await gateway.initialize();

    engine.emitPairingSnapshot(
      NativePairingSnapshot(
        revision: 2,
        attempt: NativePairingAttempt(
          state: NativePairingAttemptState.awaitingConfirmation,
          peerId: 7,
          deadlineMs: 1000,
          attemptId: <int>[1, ...List<int>.filled(15, 0)],
          sasWordIndices: const <int>[0, 1, 2, 3, 4],
          error: NativePairingError.none,
        ),
      ),
    );
    engine.emitPairingSnapshot(
      const NativePairingSnapshot(revision: 3, attempt: null),
    );
    await Future<void>.delayed(Duration.zero);

    final PairingAttemptUpdated update =
        events.whereType<PairingAttemptUpdated>().single;
    final PairingAttemptCleared cleared =
        events.whereType<PairingAttemptCleared>().single;
    expect(cleared.attemptId, update.attempt.id);
  });
}

final class _FakeNativeEngine implements NativeEngine {
  final StreamController<NativeDiscoveryPeerEvent> _discoveryEvents =
      StreamController<NativeDiscoveryPeerEvent>.broadcast(sync: true);
  final StreamController<NativeDiscoverySnapshot> _discoverySnapshots =
      StreamController<NativeDiscoverySnapshot>.broadcast(sync: true);
  final StreamController<NativePairingAttemptEvent> _pairingEvents =
      StreamController<NativePairingAttemptEvent>.broadcast(sync: true);
  final StreamController<NativePairingSnapshot> _pairingSnapshots =
      StreamController<NativePairingSnapshot>.broadcast(sync: true);
  final StreamController<NativeTrustEvent> _trustEvents =
      StreamController<NativeTrustEvent>.broadcast(sync: true);
  final StreamController<NativeTrustSnapshot> _trustSnapshots =
      StreamController<NativeTrustSnapshot>.broadcast(sync: true);
  final StreamController<NativeTransferEvent> _transferEvents =
      StreamController<NativeTransferEvent>.broadcast(sync: true);
  final StreamController<NativeTransferSnapshot> _transferSnapshots =
      StreamController<NativeTransferSnapshot>.broadcast(sync: true);

  NativePairingSnapshot _pairingSnapshot =
      const NativePairingSnapshot(revision: 1, attempt: null);

  @override
  Stream<NativeDiscoveryPeerEvent> get discoveryEvents =>
      _discoveryEvents.stream;

  @override
  Stream<NativeDiscoverySnapshot> get discoverySnapshots =>
      _discoverySnapshots.stream;

  @override
  Stream<NativePairingAttemptEvent> get pairingEvents => _pairingEvents.stream;

  @override
  Stream<NativePairingSnapshot> get pairingSnapshots =>
      _pairingSnapshots.stream;

  @override
  Stream<NativeTrustEvent> get trustEvents => _trustEvents.stream;

  @override
  Stream<NativeTrustSnapshot> get trustSnapshots => _trustSnapshots.stream;

  @override
  Stream<NativeTransferEvent> get transferEvents => _transferEvents.stream;

  @override
  Stream<NativeTransferSnapshot> get transferSnapshots =>
      _transferSnapshots.stream;

  void emitPairingSnapshot(NativePairingSnapshot snapshot) {
    _pairingSnapshot = snapshot;
    _pairingSnapshots.add(snapshot);
  }

  @override
  void initialize() {}

  @override
  void start() {}

  @override
  NativeDiscoverySnapshot discoverySnapshot() {
    return NativeDiscoverySnapshot(
      revision: 1,
      peers: const <NativeDiscoveryPeer>[],
    );
  }

  @override
  NativePairingSnapshot pairingSnapshot() => _pairingSnapshot;

  @override
  NativeTrustSnapshot trustSnapshot() {
    return NativeTrustSnapshot(
      revision: 1,
      records: const <NativeTrustRecord>[],
    );
  }

  @override
  NativeTransferSnapshot transferSnapshot() {
    return NativeTransferSnapshot(
      revision: 1,
      records: const <NativeTransferRecord>[],
    );
  }

  @override
  void dispose() {}

  @override
  dynamic noSuchMethod(Invocation invocation) {
    return super.noSuchMethod(invocation);
  }
}
