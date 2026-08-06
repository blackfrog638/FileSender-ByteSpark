import 'dart:async';
import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

const int _eventPayloadMaxSize = 256;
const int _eventTypeEngineStateChanged = 1;
const int _engineStatePayloadVersion = 1;
const int _eventFlagEventsDroppedBefore = 1;

final class _NativeEngineHandle extends Opaque {}

final class _NativeEngineConfig extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint32()
  external int reserved;
}

typedef _EventWakeupNative = Void Function(Pointer<Void>);

final class _NativeEventCallbackConfig extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint32()
  external int reserved;

  external Pointer<NativeFunction<_EventWakeupNative>> callback;

  external Pointer<Void> userData;
}

final class _NativeEvent extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint32()
  external int type;

  @Uint64()
  external int sequence;

  @Uint32()
  external int payloadVersion;

  @Uint32()
  external int payloadSize;

  @Uint32()
  external int flags;

  @Uint32()
  external int reserved;

  @Array(_eventPayloadMaxSize)
  external Array<Uint8> payload;
}

final class _NativeEngineStateEventPayload extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint32()
  external int state;

  @Uint32()
  external int reserved;
}

typedef _AbiVersionNative = Uint32 Function();
typedef _AbiVersionDart = int Function();
typedef _CreateNative = Int32 Function(
  Pointer<_NativeEngineConfig>,
  Pointer<Pointer<_NativeEngineHandle>>,
);
typedef _CreateDart = int Function(
  Pointer<_NativeEngineConfig>,
  Pointer<Pointer<_NativeEngineHandle>>,
);
typedef _DestroyNative = Void Function(Pointer<_NativeEngineHandle>);
typedef _DestroyDart = void Function(Pointer<_NativeEngineHandle>);
typedef _LifecycleNative = Int32 Function(Pointer<_NativeEngineHandle>);
typedef _LifecycleDart = int Function(Pointer<_NativeEngineHandle>);
typedef _SetEventCallbackNative = Int32 Function(
  Pointer<_NativeEngineHandle>,
  Pointer<_NativeEventCallbackConfig>,
);
typedef _SetEventCallbackDart = int Function(
  Pointer<_NativeEngineHandle>,
  Pointer<_NativeEventCallbackConfig>,
);
typedef _PollEventNative = Int32 Function(
  Pointer<_NativeEngineHandle>,
  Pointer<_NativeEvent>,
);
typedef _PollEventDart = int Function(
  Pointer<_NativeEngineHandle>,
  Pointer<_NativeEvent>,
);

enum NativeEngineState {
  created,
  running,
  stopped,
  stopping,
}

final class NativeEngineEvent {
  const NativeEngineEvent({
    required this.sequence,
    required this.state,
    required this.eventsDroppedBefore,
  });

  final int sequence;
  final NativeEngineState state;
  final bool eventsDroppedBefore;
}

class NativeEngine {
  NativeEngine._(DynamicLibrary library) {
    _abiVersion = library.lookupFunction<_AbiVersionNative, _AbiVersionDart>(
      'xnn_transfer_abi_version',
    );
    _create = library.lookupFunction<_CreateNative, _CreateDart>(
      'xnn_transfer_engine_create',
    );
    _destroy = library.lookupFunction<_DestroyNative, _DestroyDart>(
      'xnn_transfer_engine_destroy',
    );
    _start = library.lookupFunction<_LifecycleNative, _LifecycleDart>(
      'xnn_transfer_engine_start',
    );
    _stop = library.lookupFunction<_LifecycleNative, _LifecycleDart>(
      'xnn_transfer_engine_stop',
    );
    _setEventCallback =
        library.lookupFunction<_SetEventCallbackNative, _SetEventCallbackDart>(
      'xnn_transfer_engine_set_event_callback',
    );
    _pollEvent = library.lookupFunction<_PollEventNative, _PollEventDart>(
      'xnn_transfer_engine_poll_event',
    );
  }

  static const int expectedAbiVersion = 1;
  static const int statusOk = 0;
  static const int statusEventQueueEmpty = 5;

  late final _AbiVersionDart _abiVersion;
  late final _CreateDart _create;
  late final _DestroyDart _destroy;
  late final _LifecycleDart _start;
  late final _LifecycleDart _stop;
  late final _SetEventCallbackDart _setEventCallback;
  late final _PollEventDart _pollEvent;

  final StreamController<NativeEngineEvent> _events =
      StreamController<NativeEngineEvent>.broadcast();

  Pointer<_NativeEngineHandle>? _handle;
  NativeCallable<_EventWakeupNative>? _eventWakeup;
  bool _disposed = false;

  Stream<NativeEngineEvent> get events => _events.stream;

  static NativeEngine open() {
    final String? configuredPath =
        Platform.environment['XNN_TRANSFER_LIBRARY_PATH'];
    if (configuredPath != null && configuredPath.isNotEmpty) {
      return NativeEngine._(DynamicLibrary.open(configuredPath));
    }

    final File executable = File(Platform.resolvedExecutable);
    final String libraryPath = switch (Platform.operatingSystem) {
      'macos' => '${executable.parent.parent.path}/Frameworks/'
          'libxnn_transfer_core.dylib',
      'windows' => '${executable.parent.path}\\xnn_transfer_core.dll',
      'linux' => '${executable.parent.path}/libxnn_transfer_core.so',
      _ => throw UnsupportedError(
          'Unsupported desktop platform: ${Platform.operatingSystem}',
        ),
    };
    return NativeEngine._(DynamicLibrary.open(libraryPath));
  }

  void initialize() {
    if (_disposed) {
      throw StateError('Native engine is disposed');
    }
    if (_handle != null) {
      return;
    }
    if (_abiVersion() != expectedAbiVersion) {
      throw StateError('The native library uses an incompatible ABI');
    }

    final Pointer<_NativeEngineConfig> config = calloc<_NativeEngineConfig>();
    final Pointer<Pointer<_NativeEngineHandle>> output =
        calloc<Pointer<_NativeEngineHandle>>();
    try {
      config.ref
        ..structSize = sizeOf<_NativeEngineConfig>()
        ..abiVersion = expectedAbiVersion
        ..reserved = 0;

      final int status = _create(config, output);
      if (status != statusOk || output.value == nullptr) {
        throw StateError('Native engine creation failed with status $status');
      }
      _handle = output.value;
      try {
        _registerEventWakeup();
      } catch (_) {
        _destroy(output.value);
        _handle = null;
        rethrow;
      }
    } finally {
      calloc.free(output);
      calloc.free(config);
    }
  }

  void start() {
    _requireSuccess(_start(_requireHandle()), 'start');
  }

  void stop() {
    final Pointer<_NativeEngineHandle>? handle = _handle;
    if (handle == null) {
      return;
    }
    _requireSuccess(_stop(handle), 'stop');
  }

  void dispose() {
    if (_disposed) {
      return;
    }
    _disposed = true;

    final Pointer<_NativeEngineHandle>? handle = _handle;
    if (handle != null) {
      // Clearing is the native in-flight callback barrier. It must complete
      // before NativeCallable.close makes the function pointer invalid.
      _setEventCallback(handle, nullptr);
      _stop(handle);
      _destroy(handle);
      _handle = null;
    }

    _eventWakeup?.close();
    _eventWakeup = null;
    unawaited(_events.close());
  }

  Pointer<_NativeEngineHandle> _requireHandle() {
    final Pointer<_NativeEngineHandle>? handle = _handle;
    if (handle == null) {
      throw StateError('Native engine is not initialized');
    }
    return handle;
  }

  void _requireSuccess(int status, String operation) {
    if (status != statusOk) {
      throw StateError('Native engine $operation failed with status $status');
    }
  }

  void _registerEventWakeup() {
    final Pointer<_NativeEngineHandle> handle = _requireHandle();
    final NativeCallable<_EventWakeupNative> wakeup =
        NativeCallable<_EventWakeupNative>.listener(_handleEventWakeup);
    final Pointer<_NativeEventCallbackConfig> config =
        calloc<_NativeEventCallbackConfig>();
    _eventWakeup = wakeup;
    try {
      config.ref
        ..structSize = sizeOf<_NativeEventCallbackConfig>()
        ..abiVersion = expectedAbiVersion
        ..reserved = 0
        ..callback = wakeup.nativeFunction
        ..userData = nullptr;
      final int status = _setEventCallback(handle, config);
      if (status != statusOk) {
        throw StateError(
          'Native event callback registration failed with status $status',
        );
      }
    } catch (_) {
      _eventWakeup = null;
      wakeup.close();
      rethrow;
    } finally {
      calloc.free(config);
    }
  }

  // NativeCallable.listener schedules this after the native wakeup has
  // returned. No event pointer crosses that asynchronous boundary.
  void _handleEventWakeup(Pointer<Void> _) {
    if (_disposed) {
      return;
    }
    final Pointer<_NativeEngineHandle>? handle = _handle;
    if (handle == null) {
      return;
    }

    final Pointer<_NativeEvent> event = calloc<_NativeEvent>();
    try {
      for (;;) {
        event.ref
          ..structSize = sizeOf<_NativeEvent>()
          ..abiVersion = expectedAbiVersion;
        final int status = _pollEvent(handle, event);
        if (status == statusEventQueueEmpty) {
          return;
        }
        if (status != statusOk) {
          _events.addError(
            StateError('Native event polling failed with status $status'),
          );
          return;
        }
        _events.add(_decodeEvent(event.ref));
      }
    } on Object catch (error, stackTrace) {
      _events.addError(error, stackTrace);
    } finally {
      calloc.free(event);
    }
  }

  NativeEngineEvent _decodeEvent(_NativeEvent event) {
    if (event.structSize < sizeOf<_NativeEvent>() ||
        event.abiVersion != expectedAbiVersion) {
      throw StateError('Native event uses an incompatible struct');
    }
    if (event.type != _eventTypeEngineStateChanged ||
        event.payloadVersion != _engineStatePayloadVersion) {
      throw StateError('Native event type or payload version is unsupported');
    }
    if (event.payloadSize != sizeOf<_NativeEngineStateEventPayload>() ||
        event.payloadSize > _eventPayloadMaxSize) {
      throw StateError('Native event payload size is invalid');
    }

    final Uint8List bytes = Uint8List(event.payloadSize);
    for (int index = 0; index < bytes.length; index += 1) {
      bytes[index] = event.payload[index];
    }
    final ByteData payload = ByteData.sublistView(bytes);
    final int sizeFieldWidth = sizeOf<UintPtr>();
    final int payloadStructSize = sizeFieldWidth == 8
        ? payload.getUint64(0, Endian.host)
        : payload.getUint32(0, Endian.host);
    final int payloadAbiVersion =
        payload.getUint32(sizeFieldWidth, Endian.host);
    final int rawState = payload.getUint32(sizeFieldWidth + 4, Endian.host);
    if (payloadStructSize != sizeOf<_NativeEngineStateEventPayload>() ||
        payloadAbiVersion != expectedAbiVersion) {
      throw StateError('Native engine state payload is invalid');
    }
    final NativeEngineState state = switch (rawState) {
      0 => NativeEngineState.created,
      1 => NativeEngineState.running,
      2 => NativeEngineState.stopped,
      3 => NativeEngineState.stopping,
      _ => throw StateError('Native engine state is unsupported'),
    };

    return NativeEngineEvent(
      sequence: event.sequence,
      state: state,
      eventsDroppedBefore: event.flags & _eventFlagEventsDroppedBefore != 0,
    );
  }
}
