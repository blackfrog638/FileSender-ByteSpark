import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

final class _NativeEngineHandle extends Opaque {}

final class _NativeEngineConfig extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

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

class NativeEngine {
  NativeEngine._(DynamicLibrary library)
      : _abiVersion =
            library.lookupFunction<_AbiVersionNative, _AbiVersionDart>(
          'xnn_transfer_abi_version',
        ),
        _create = library.lookupFunction<_CreateNative, _CreateDart>(
          'xnn_transfer_engine_create',
        ),
        _destroy = library.lookupFunction<_DestroyNative, _DestroyDart>(
          'xnn_transfer_engine_destroy',
        ),
        _start = library.lookupFunction<_LifecycleNative, _LifecycleDart>(
          'xnn_transfer_engine_start',
        ),
        _stop = library.lookupFunction<_LifecycleNative, _LifecycleDart>(
          'xnn_transfer_engine_stop',
        );

  static const int expectedAbiVersion = 1;
  static const int statusOk = 0;

  final _AbiVersionDart _abiVersion;
  final _CreateDart _create;
  final _DestroyDart _destroy;
  final _LifecycleDart _start;
  final _LifecycleDart _stop;

  Pointer<_NativeEngineHandle>? _handle;

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
    final Pointer<_NativeEngineHandle>? handle = _handle;
    if (handle == null) {
      return;
    }

    _stop(handle);
    _destroy(handle);
    _handle = null;
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
}
