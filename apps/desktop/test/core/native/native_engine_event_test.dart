import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:xnn_transfer/core/native/native_engine.dart';
import 'package:xnn_transfer/core/native/native_event_decoder.dart';

void main() {
  final String? libraryPath = Platform.environment['XNN_TRANSFER_LIBRARY_PATH'];
  final Object skipReason = libraryPath == null || libraryPath.isEmpty
      ? 'Requires a built native library via XNN_TRANSFER_LIBRARY_PATH'
      : false;

  test(
    'listener drains native-owned lifecycle events after wakeup returns',
    () async {
      final NativeEngine engine = NativeEngine.open();
      addTearDown(engine.dispose);

      final Future<List<NativeEngineEvent>> eventsFuture =
          engine.events.take(4).toList().timeout(const Duration(seconds: 5));

      engine.initialize();
      engine.start();
      engine.stop();

      final List<NativeEngineEvent> events = await eventsFuture;
      expect(
        events.map((NativeEngineEvent event) => event.sequence),
        <int>[1, 2, 3, 4],
      );
      expect(
        events.map((NativeEngineEvent event) => event.state),
        <NativeEngineState>[
          NativeEngineState.created,
          NativeEngineState.running,
          NativeEngineState.stopping,
          NativeEngineState.stopped,
        ],
      );
      expect(
        events.every((NativeEngineEvent event) => !event.eventsDroppedBefore),
        isTrue,
      );
    },
    skip: skipReason,
  );

  test(
    'dispose is an idempotent callback shutdown barrier',
    () async {
      final NativeEngine engine = NativeEngine.open();
      engine.initialize();
      engine.start();

      engine.dispose();
      engine.dispose();

      await expectLater(engine.events, emitsDone);
    },
    skip: skipReason,
  );

  test(
    'Dart exercises discovery commands and fixed snapshot layout',
    () {
      final NativeEngine engine = NativeEngine.open();
      addTearDown(engine.dispose);

      engine.initialize();
      engine.start();
      engine.startDiscovery(servicePort: 45879);

      final NativeDiscoverySnapshot snapshot = engine.discoverySnapshot();
      expect(snapshot.revision, greaterThan(0));
      expect(snapshot.peers, isEmpty);

      engine.stopDiscovery();
      engine.stop();
    },
    skip: skipReason,
  );

  test('discovery decoder copies bounded peer bytes', () {
    const int pointerSize = 8;
    final Uint8List payload = Uint8List(176);
    final ByteData fields = ByteData.sublistView(payload);
    fields
      ..setUint64(0, payload.length, Endian.host)
      ..setUint32(8, 1, Endian.host)
      ..setUint32(12, 1, Endian.host)
      ..setUint64(16, 9, Endian.host)
      ..setUint32(24, 0, Endian.host)
      ..setUint64(32, 144, Endian.host)
      ..setUint32(40, 1, Endian.host)
      ..setUint64(48, 42, Endian.host)
      ..setUint16(56, 45879, Endian.host)
      ..setUint8(58, 4)
      ..setUint8(59, 4)
      ..setUint32(60, 4, Endian.host);
    payload.setRange(64, 68, <int>[192, 0, 2, 7]);
    payload.setRange(80, 84, utf8.encode('Desk'));

    final NativeDiscoveryPeerEvent event =
        decodeNativeDiscoveryPeerEventPayload(
      sequence: 17,
      payloadVersion: 1,
      flags: 1,
      payload: payload,
      pointerSize: pointerSize,
    );

    fields.setUint32(60, 97, Endian.host);
    expect(
      () => decodeNativeDiscoveryPeerEventPayload(
        sequence: 18,
        payloadVersion: 1,
        flags: 0,
        payload: payload,
        pointerSize: pointerSize,
      ),
      throwsStateError,
    );
    payload.fillRange(0, payload.length, 0);

    expect(event.sequence, 17);
    expect(event.snapshotRevision, 9);
    expect(event.change, NativeDiscoveryPeerChange.appeared);
    expect(event.eventsDroppedBefore, isTrue);
    expect(event.peer.peerId, 42);
    expect(event.peer.servicePort, 45879);
    expect(event.peer.address, <int>[192, 0, 2, 7]);
    expect(event.peer.displayLabel, 'Desk');
  });
}
