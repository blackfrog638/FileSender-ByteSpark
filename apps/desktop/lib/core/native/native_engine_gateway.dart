import 'package:xnn_transfer/core/native/native_engine.dart';
import 'package:xnn_transfer/features/transfer/domain/engine_gateway.dart';

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
