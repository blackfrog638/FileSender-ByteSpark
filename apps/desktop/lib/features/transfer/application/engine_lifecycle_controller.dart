import 'package:flutter/foundation.dart';
import 'package:xnn_transfer/features/transfer/domain/engine_gateway.dart';

enum EnginePhase { initializing, ready, running, stopped, unavailable }

class EngineLifecycleController extends ChangeNotifier {
  EngineLifecycleController(this._gateway);

  final EngineGateway _gateway;

  EnginePhase _phase = EnginePhase.initializing;
  String? _error;

  EnginePhase get phase => _phase;
  String? get error => _error;

  void initialize() {
    try {
      _gateway.initialize();
      _transitionTo(EnginePhase.ready);
    } on Object catch (error) {
      _error = error.toString();
      _transitionTo(EnginePhase.unavailable);
    }
  }

  void start() {
    if (_phase != EnginePhase.ready) {
      return;
    }

    try {
      _gateway.start();
      _transitionTo(EnginePhase.running);
    } on Object catch (error) {
      _error = error.toString();
      _transitionTo(EnginePhase.unavailable);
    }
  }

  void stop() {
    if (_phase != EnginePhase.ready && _phase != EnginePhase.running) {
      return;
    }

    _gateway.stop();
    _transitionTo(EnginePhase.stopped);
  }

  void _transitionTo(EnginePhase next) {
    _phase = next;
    notifyListeners();
  }

  @override
  void dispose() {
    _gateway.dispose();
    super.dispose();
  }
}
