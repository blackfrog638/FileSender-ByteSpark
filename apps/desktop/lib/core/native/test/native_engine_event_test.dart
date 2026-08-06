// ignore_for_file: depend_on_referenced_packages

import 'package:flutter_test/flutter_test.dart';
import 'package:xnn_transfer/core/native/native_engine.dart';

void main() {
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
  );

  test('dispose is an idempotent callback shutdown barrier', () async {
    final NativeEngine engine = NativeEngine.open();
    engine.initialize();
    engine.start();

    engine.dispose();
    engine.dispose();

    await expectLater(engine.events, emitsDone);
  });
}
