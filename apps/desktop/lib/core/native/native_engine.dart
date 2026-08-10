import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';
import 'package:xnn_transfer/core/native/native_event_decoder.dart';

const int _eventPayloadMaxSize = 256;
const int _eventTypeEngineStateChanged = 1;
const int _eventTypeDiscoveryPeerChanged = 2;
const int _eventTypePairingAttemptChanged = 3;
const int _eventTypeTrustChanged = 4;
const int _eventTypeTransferChanged = 5;
const int _engineStatePayloadVersion = 1;
const int _eventFlagEventsDroppedBefore = 1;
const int _discoveryDisplayLabelMaxSize = 96;
const int _discoveryAddressMaxSize = 16;
const int _discoverySnapshotPageCapacity = 8;
const int _discoveryMaxPeers = 256;
const int _pairingAttemptIdSize = 16;
const int _pairingSasWordCount = 5;
const int _trustSnapshotPageCapacity = 8;
const int _trustMaxRecords = 256;
const int _transferPathMaxSize = 1024;
const int _transferIdSize = 16;
const int _transferPeerLabelMaxSize = 96;
const int _transferSnapshotPageCapacity = 4;
const int _transferMaxRecords = 256;

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

final class _NativeDiscoveryConfig extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint16()
  external int servicePort;

  @Uint16()
  external int reserved;

  @Uint32()
  external int displayLabelSize;

  @Array(_discoveryDisplayLabelMaxSize)
  external Array<Uint8> displayLabel;
}

final class _NativeDiscoveryPeer extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint32()
  external int reserved;

  @Uint64()
  external int peerId;

  @Uint16()
  external int servicePort;

  @Uint8()
  external int addressFamily;

  @Uint8()
  external int addressSize;

  @Uint32()
  external int displayLabelSize;

  @Array(_discoveryAddressMaxSize)
  external Array<Uint8> address;

  @Array(_discoveryDisplayLabelMaxSize)
  external Array<Uint8> displayLabel;
}

final class _NativeDiscoverySnapshotPage extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint32()
  external int reserved;

  @Uint64()
  external int snapshotRevision;

  @Uint32()
  external int offset;

  @Uint32()
  external int count;

  @Uint32()
  external int totalCount;

  @Uint32()
  external int reserved2;

  @Array(_discoverySnapshotPageCapacity)
  external Array<_NativeDiscoveryPeer> peers;
}

final class _NativePairingWindowConfig extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint32()
  external int reserved;

  @Uint64()
  external int durationMs;
}

final class _NativePairingStartRequest extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint32()
  external int reserved;

  @Uint64()
  external int peerId;
}

final class _NativePairingAttemptRef extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint32()
  external int reserved;

  @Array(_pairingAttemptIdSize)
  external Array<Uint8> attemptId;
}

final class _NativeTrustRef extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint32()
  external int reserved;

  @Uint64()
  external int trustId;
}

final class _NativePairingAttemptEventPayload extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint32()
  external int state;

  @Uint64()
  external int peerId;

  @Uint64()
  external int deadlineMs;

  @Array(_pairingAttemptIdSize)
  external Array<Uint8> attemptId;

  @Array(_pairingSasWordCount)
  external Array<Uint16> sasWordIndices;

  @Uint16()
  external int sasWordCount;

  @Uint16()
  external int error;

  @Uint16()
  external int reserved;
}

final class _NativeTrustEventPayload extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint32()
  external int state;

  @Uint64()
  external int trustId;

  @Uint64()
  external int peerId;

  @Uint32()
  external int reserved;

  @Uint32()
  external int reserved2;
}

final class _NativePairingSnapshot extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint32()
  external int reserved;

  @Uint64()
  external int snapshotRevision;

  @Uint32()
  external int hasAttempt;

  @Uint32()
  external int reserved2;

  external _NativePairingAttemptEventPayload attempt;
}

final class _NativeTrustSnapshotPage extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint32()
  external int reserved;

  @Uint64()
  external int snapshotRevision;

  @Uint32()
  external int offset;

  @Uint32()
  external int count;

  @Uint32()
  external int totalCount;

  @Uint32()
  external int reserved2;

  @Array(_trustSnapshotPageCapacity)
  external Array<_NativeTrustEventPayload> records;
}

final class _NativeTransferSendRequest extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint32()
  external int reserved;

  @Uint64()
  external int trustId;

  @Uint32()
  external int pathSize;

  @Uint32()
  external int reserved2;

  @Array(_transferPathMaxSize)
  external Array<Uint8> path;
}

final class _NativeTransferRef extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint32()
  external int reserved;

  @Array(_transferIdSize)
  external Array<Uint8> transferId;
}

final class _NativeTransferEventPayload extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint32()
  external int change;

  @Uint64()
  external int snapshotRevision;

  @Uint32()
  external int direction;

  @Uint32()
  external int state;

  @Uint32()
  external int error;

  @Uint32()
  external int peerLabelSize;

  @Uint32()
  external int reserved;

  @Uint32()
  external int reserved2;

  @Uint64()
  external int totalBytes;

  @Uint64()
  external int transferredBytes;

  @Array(_transferIdSize)
  external Array<Uint8> transferId;

  @Array(_transferPeerLabelMaxSize)
  external Array<Uint8> peerLabel;
}

final class _NativeTransferSnapshotPage extends Struct {
  @UintPtr()
  external int structSize;

  @Uint32()
  external int abiVersion;

  @Uint32()
  external int reserved;

  @Uint64()
  external int snapshotRevision;

  @Uint32()
  external int offset;

  @Uint32()
  external int count;

  @Uint32()
  external int totalCount;

  @Uint32()
  external int reserved2;

  @Array(_transferSnapshotPageCapacity)
  external Array<_NativeTransferEventPayload> records;
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
    Pointer<_NativeEngineHandle>, Pointer<_NativeEvent>);
typedef _PollEventDart = int Function(
    Pointer<_NativeEngineHandle>, Pointer<_NativeEvent>);
typedef _DiscoveryStartNative = Int32 Function(
  Pointer<_NativeEngineHandle>,
  Pointer<_NativeDiscoveryConfig>,
);
typedef _DiscoveryStartDart = int Function(
    Pointer<_NativeEngineHandle>, Pointer<_NativeDiscoveryConfig>);
typedef _DiscoverySnapshotNative = Int32 Function(
  Pointer<_NativeEngineHandle>,
  Uint64,
  Uint32,
  Pointer<_NativeDiscoverySnapshotPage>,
);
typedef _DiscoverySnapshotDart = int Function(
  Pointer<_NativeEngineHandle>,
  int,
  int,
  Pointer<_NativeDiscoverySnapshotPage>,
);
typedef _PairingOpenWindowNative = Int32 Function(
  Pointer<_NativeEngineHandle>,
  Pointer<_NativePairingWindowConfig>,
);
typedef _PairingOpenWindowDart = int Function(
  Pointer<_NativeEngineHandle>,
  Pointer<_NativePairingWindowConfig>,
);
typedef _PairingStartNative = Int32 Function(
  Pointer<_NativeEngineHandle>,
  Pointer<_NativePairingStartRequest>,
);
typedef _PairingStartDart = int Function(
  Pointer<_NativeEngineHandle>,
  Pointer<_NativePairingStartRequest>,
);
typedef _PairingAttemptCommandNative = Int32 Function(
  Pointer<_NativeEngineHandle>,
  Pointer<_NativePairingAttemptRef>,
);
typedef _PairingAttemptCommandDart = int Function(
  Pointer<_NativeEngineHandle>,
  Pointer<_NativePairingAttemptRef>,
);
typedef _PairingRevokeNative = Int32 Function(
    Pointer<_NativeEngineHandle>, Pointer<_NativeTrustRef>);
typedef _PairingRevokeDart = int Function(
    Pointer<_NativeEngineHandle>, Pointer<_NativeTrustRef>);
typedef _PairingSnapshotNative = Int32 Function(
  Pointer<_NativeEngineHandle>,
  Pointer<_NativePairingSnapshot>,
);
typedef _PairingSnapshotDart = int Function(
    Pointer<_NativeEngineHandle>, Pointer<_NativePairingSnapshot>);
typedef _TrustSnapshotNative = Int32 Function(
  Pointer<_NativeEngineHandle>,
  Uint64,
  Uint32,
  Pointer<_NativeTrustSnapshotPage>,
);
typedef _TrustSnapshotDart = int Function(
  Pointer<_NativeEngineHandle>,
  int,
  int,
  Pointer<_NativeTrustSnapshotPage>,
);
typedef _TransferSendNative = Int32 Function(
  Pointer<_NativeEngineHandle>,
  Pointer<_NativeTransferSendRequest>,
  Pointer<_NativeTransferRef>,
);
typedef _TransferSendDart = int Function(
  Pointer<_NativeEngineHandle>,
  Pointer<_NativeTransferSendRequest>,
  Pointer<_NativeTransferRef>,
);
typedef _TransferCommandNative = Int32 Function(
  Pointer<_NativeEngineHandle>,
  Pointer<_NativeTransferRef>,
);
typedef _TransferCommandDart = int Function(
  Pointer<_NativeEngineHandle>,
  Pointer<_NativeTransferRef>,
);
typedef _TransferSnapshotNative = Int32 Function(
  Pointer<_NativeEngineHandle>,
  Uint64,
  Uint32,
  Pointer<_NativeTransferSnapshotPage>,
);
typedef _TransferSnapshotDart = int Function(
  Pointer<_NativeEngineHandle>,
  int,
  int,
  Pointer<_NativeTransferSnapshotPage>,
);

enum NativeEngineState { created, running, stopped, stopping }

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

final class NativeEngineOperationException implements Exception {
  const NativeEngineOperationException({
    required this.operation,
    required this.status,
  });

  final String operation;
  final int status;

  String get code => switch (status) {
        1 => 'invalid_argument',
        2 => 'incompatible_abi',
        3 => 'invalid_state',
        4 => 'internal_error',
        5 => 'event_queue_empty',
        6 => 'stale_snapshot',
        7 => 'unavailable',
        8 => 'stale_handle',
        _ => 'unknown',
      };

  @override
  String toString() =>
      'NativeEngineOperationException($operation, $code, status=$status)';
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
    if (!library.providesSymbol('xnn_transfer_engine_set_event_callback') ||
        !library.providesSymbol('xnn_transfer_engine_poll_event') ||
        !library.providesSymbol('xnn_transfer_discovery_start') ||
        !library.providesSymbol('xnn_transfer_discovery_stop') ||
        !library.providesSymbol('xnn_transfer_discovery_get_snapshot') ||
        !library.providesSymbol('xnn_transfer_pairing_open_window') ||
        !library.providesSymbol('xnn_transfer_pairing_close_window') ||
        !library.providesSymbol('xnn_transfer_pairing_start') ||
        !library.providesSymbol('xnn_transfer_pairing_confirm') ||
        !library.providesSymbol('xnn_transfer_pairing_reject') ||
        !library.providesSymbol('xnn_transfer_pairing_revoke') ||
        !library.providesSymbol('xnn_transfer_pairing_get_snapshot') ||
        !library.providesSymbol('xnn_transfer_trust_get_snapshot') ||
        !library.providesSymbol('xnn_transfer_transfer_send') ||
        !library.providesSymbol('xnn_transfer_transfer_accept') ||
        !library.providesSymbol('xnn_transfer_transfer_reject') ||
        !library.providesSymbol('xnn_transfer_transfer_cancel') ||
        !library.providesSymbol('xnn_transfer_transfer_get_snapshot')) {
      throw StateError(
        'The native library does not provide the operation event ABI',
      );
    }
    _setEventCallback =
        library.lookupFunction<_SetEventCallbackNative, _SetEventCallbackDart>(
      'xnn_transfer_engine_set_event_callback',
    );
    _pollEvent = library.lookupFunction<_PollEventNative, _PollEventDart>(
      'xnn_transfer_engine_poll_event',
    );
    _discoveryStart =
        library.lookupFunction<_DiscoveryStartNative, _DiscoveryStartDart>(
      'xnn_transfer_discovery_start',
    );
    _discoveryStop = library.lookupFunction<_LifecycleNative, _LifecycleDart>(
      'xnn_transfer_discovery_stop',
    );
    _discoverySnapshot = library
        .lookupFunction<_DiscoverySnapshotNative, _DiscoverySnapshotDart>(
      'xnn_transfer_discovery_get_snapshot',
    );
    _pairingOpenWindow = library
        .lookupFunction<_PairingOpenWindowNative, _PairingOpenWindowDart>(
      'xnn_transfer_pairing_open_window',
    );
    _pairingCloseWindow =
        library.lookupFunction<_LifecycleNative, _LifecycleDart>(
      'xnn_transfer_pairing_close_window',
    );
    _pairingStart =
        library.lookupFunction<_PairingStartNative, _PairingStartDart>(
      'xnn_transfer_pairing_start',
    );
    _pairingConfirm = library.lookupFunction<_PairingAttemptCommandNative,
        _PairingAttemptCommandDart>('xnn_transfer_pairing_confirm');
    _pairingReject = library.lookupFunction<_PairingAttemptCommandNative,
        _PairingAttemptCommandDart>('xnn_transfer_pairing_reject');
    _pairingRevoke =
        library.lookupFunction<_PairingRevokeNative, _PairingRevokeDart>(
      'xnn_transfer_pairing_revoke',
    );
    _pairingSnapshot =
        library.lookupFunction<_PairingSnapshotNative, _PairingSnapshotDart>(
      'xnn_transfer_pairing_get_snapshot',
    );
    _trustSnapshot =
        library.lookupFunction<_TrustSnapshotNative, _TrustSnapshotDart>(
      'xnn_transfer_trust_get_snapshot',
    );
    _transferSend =
        library.lookupFunction<_TransferSendNative, _TransferSendDart>(
      'xnn_transfer_transfer_send',
    );
    _transferAccept =
        library.lookupFunction<_TransferCommandNative, _TransferCommandDart>(
      'xnn_transfer_transfer_accept',
    );
    _transferReject =
        library.lookupFunction<_TransferCommandNative, _TransferCommandDart>(
      'xnn_transfer_transfer_reject',
    );
    _transferCancel =
        library.lookupFunction<_TransferCommandNative, _TransferCommandDart>(
      'xnn_transfer_transfer_cancel',
    );
    _transferSnapshot =
        library.lookupFunction<_TransferSnapshotNative, _TransferSnapshotDart>(
      'xnn_transfer_transfer_get_snapshot',
    );
  }

  static const int expectedAbiVersion = 1;
  static const int statusOk = 0;
  static const int statusEventQueueEmpty = 5;
  static const int statusStaleSnapshot = 6;

  late final _AbiVersionDart _abiVersion;
  late final _CreateDart _create;
  late final _DestroyDart _destroy;
  late final _LifecycleDart _start;
  late final _LifecycleDart _stop;
  late final _SetEventCallbackDart _setEventCallback;
  late final _PollEventDart _pollEvent;
  late final _DiscoveryStartDart _discoveryStart;
  late final _LifecycleDart _discoveryStop;
  late final _DiscoverySnapshotDart _discoverySnapshot;
  late final _PairingOpenWindowDart _pairingOpenWindow;
  late final _LifecycleDart _pairingCloseWindow;
  late final _PairingStartDart _pairingStart;
  late final _PairingAttemptCommandDart _pairingConfirm;
  late final _PairingAttemptCommandDart _pairingReject;
  late final _PairingRevokeDart _pairingRevoke;
  late final _PairingSnapshotDart _pairingSnapshot;
  late final _TrustSnapshotDart _trustSnapshot;
  late final _TransferSendDart _transferSend;
  late final _TransferCommandDart _transferAccept;
  late final _TransferCommandDart _transferReject;
  late final _TransferCommandDart _transferCancel;
  late final _TransferSnapshotDart _transferSnapshot;

  final StreamController<NativeEngineEvent> _events =
      StreamController<NativeEngineEvent>.broadcast();
  final StreamController<NativeDiscoveryPeerEvent> _discoveryEvents =
      StreamController<NativeDiscoveryPeerEvent>.broadcast();
  final StreamController<NativeDiscoverySnapshot> _discoverySnapshots =
      StreamController<NativeDiscoverySnapshot>.broadcast();
  final StreamController<NativePairingAttemptEvent> _pairingEvents =
      StreamController<NativePairingAttemptEvent>.broadcast();
  final StreamController<NativeTrustEvent> _trustEvents =
      StreamController<NativeTrustEvent>.broadcast();
  final StreamController<NativePairingSnapshot> _pairingSnapshots =
      StreamController<NativePairingSnapshot>.broadcast();
  final StreamController<NativeTrustSnapshot> _trustSnapshots =
      StreamController<NativeTrustSnapshot>.broadcast();
  final StreamController<NativeTransferEvent> _transferEvents =
      StreamController<NativeTransferEvent>.broadcast();
  final StreamController<NativeTransferSnapshot> _transferSnapshots =
      StreamController<NativeTransferSnapshot>.broadcast();

  Pointer<_NativeEngineHandle>? _handle;
  NativeCallable<_EventWakeupNative>? _eventWakeup;
  bool _disposed = false;

  Stream<NativeEngineEvent> get events => _events.stream;
  Stream<NativeDiscoveryPeerEvent> get discoveryEvents =>
      _discoveryEvents.stream;
  Stream<NativeDiscoverySnapshot> get discoverySnapshots =>
      _discoverySnapshots.stream;
  Stream<NativePairingAttemptEvent> get pairingEvents => _pairingEvents.stream;
  Stream<NativeTrustEvent> get trustEvents => _trustEvents.stream;
  Stream<NativePairingSnapshot> get pairingSnapshots =>
      _pairingSnapshots.stream;
  Stream<NativeTrustSnapshot> get trustSnapshots => _trustSnapshots.stream;
  Stream<NativeTransferEvent> get transferEvents => _transferEvents.stream;
  Stream<NativeTransferSnapshot> get transferSnapshots =>
      _transferSnapshots.stream;

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

  void startDiscovery({required int servicePort, String displayLabel = ''}) {
    final List<int> label = utf8.encode(displayLabel);
    if (servicePort <= 0 ||
        servicePort > 0xffff ||
        label.length > _discoveryDisplayLabelMaxSize) {
      throw ArgumentError('Discovery configuration is out of range');
    }

    final Pointer<_NativeDiscoveryConfig> config =
        calloc<_NativeDiscoveryConfig>();
    try {
      config.ref
        ..structSize = sizeOf<_NativeDiscoveryConfig>()
        ..abiVersion = expectedAbiVersion
        ..servicePort = servicePort
        ..reserved = 0
        ..displayLabelSize = label.length;
      for (int index = 0; index < label.length; index += 1) {
        config.ref.displayLabel[index] = label[index];
      }
      _requireSuccess(
        _discoveryStart(_requireHandle(), config),
        'discovery start',
      );
    } finally {
      calloc.free(config);
    }
  }

  void stopDiscovery() {
    final Pointer<_NativeEngineHandle>? handle = _handle;
    if (handle == null) {
      return;
    }
    _requireSuccess(_discoveryStop(handle), 'discovery stop');
  }

  NativeDiscoverySnapshot discoverySnapshot() {
    return _readDiscoverySnapshot();
  }

  void openPairingWindow(Duration duration) {
    final int durationMs = duration.inMilliseconds;
    if (durationMs <= 0 || durationMs > 120000) {
      throw ArgumentError('Pairing window duration is out of range');
    }

    final Pointer<_NativePairingWindowConfig> config =
        calloc<_NativePairingWindowConfig>();
    try {
      config.ref
        ..structSize = sizeOf<_NativePairingWindowConfig>()
        ..abiVersion = expectedAbiVersion
        ..reserved = 0
        ..durationMs = durationMs;
      _requireSuccess(
        _pairingOpenWindow(_requireHandle(), config),
        'pairing window open',
      );
    } finally {
      calloc.free(config);
    }
  }

  void closePairingWindow() {
    _requireSuccess(
      _pairingCloseWindow(_requireHandle()),
      'pairing window close',
    );
  }

  void startPairing(int peerId) {
    if (peerId <= 0) {
      throw ArgumentError('Pairing requires a native peer observation');
    }
    final Pointer<_NativePairingStartRequest> request =
        calloc<_NativePairingStartRequest>();
    try {
      request.ref
        ..structSize = sizeOf<_NativePairingStartRequest>()
        ..abiVersion = expectedAbiVersion
        ..reserved = 0
        ..peerId = peerId;
      _requireSuccess(
        _pairingStart(_requireHandle(), request),
        'pairing start',
      );
    } finally {
      calloc.free(request);
    }
  }

  void confirmPairing(List<int> attemptId) {
    _decidePairing(attemptId, _pairingConfirm, 'pairing confirmation');
  }

  void rejectPairing(List<int> attemptId) {
    _decidePairing(attemptId, _pairingReject, 'pairing rejection');
  }

  void revokeTrust(int trustId) {
    if (trustId <= 0) {
      throw ArgumentError('Trust ID must be native-issued and nonzero');
    }
    final Pointer<_NativeTrustRef> trust = calloc<_NativeTrustRef>();
    try {
      trust.ref
        ..structSize = sizeOf<_NativeTrustRef>()
        ..abiVersion = expectedAbiVersion
        ..reserved = 0
        ..trustId = trustId;
      _requireSuccess(
        _pairingRevoke(_requireHandle(), trust),
        'pairing revocation',
      );
    } finally {
      calloc.free(trust);
    }
  }

  NativePairingSnapshot pairingSnapshot() {
    return _readPairingSnapshot();
  }

  NativeTrustSnapshot trustSnapshot() {
    return _readTrustSnapshot();
  }

  List<int> sendFile({required int trustId, required String path}) {
    final List<int> pathBytes = utf8.encode(path);
    if (trustId <= 0 ||
        pathBytes.isEmpty ||
        pathBytes.length > _transferPathMaxSize ||
        pathBytes.contains(0)) {
      throw ArgumentError('Transfer send request is invalid');
    }

    final Pointer<_NativeTransferSendRequest> request =
        calloc<_NativeTransferSendRequest>();
    final Pointer<_NativeTransferRef> transfer = calloc<_NativeTransferRef>();
    try {
      request.ref
        ..structSize = sizeOf<_NativeTransferSendRequest>()
        ..abiVersion = expectedAbiVersion
        ..reserved = 0
        ..trustId = trustId
        ..pathSize = pathBytes.length
        ..reserved2 = 0;
      for (int index = 0; index < pathBytes.length; index += 1) {
        request.ref.path[index] = pathBytes[index];
      }
      transfer.ref
        ..structSize = sizeOf<_NativeTransferRef>()
        ..abiVersion = expectedAbiVersion
        ..reserved = 0;
      _requireTransferSuccess(
        _transferSend(_requireHandle(), request, transfer),
        'transfer send',
      );
      return List<int>.unmodifiable(<int>[
        for (int index = 0; index < _transferIdSize; index += 1)
          transfer.ref.transferId[index],
      ]);
    } finally {
      calloc.free(transfer);
      calloc.free(request);
    }
  }

  void acceptTransfer(List<int> transferId) {
    _runTransferCommand(
      transferId,
      _transferAccept,
      'transfer acceptance',
    );
  }

  void rejectTransfer(List<int> transferId) {
    _runTransferCommand(transferId, _transferReject, 'transfer rejection');
  }

  void cancelTransfer(List<int> transferId) {
    _runTransferCommand(transferId, _transferCancel, 'transfer cancellation');
  }

  NativeTransferSnapshot transferSnapshot() {
    return _readTransferSnapshot();
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
    unawaited(_discoveryEvents.close());
    unawaited(_discoverySnapshots.close());
    unawaited(_pairingEvents.close());
    unawaited(_trustEvents.close());
    unawaited(_pairingSnapshots.close());
    unawaited(_trustSnapshots.close());
    unawaited(_transferEvents.close());
    unawaited(_transferSnapshots.close());
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

  void _requireTransferSuccess(int status, String operation) {
    if (status != statusOk) {
      throw NativeEngineOperationException(
        operation: operation,
        status: status,
      );
    }
  }

  void _decidePairing(
    List<int> attemptId,
    _PairingAttemptCommandDart command,
    String operation,
  ) {
    if (attemptId.length != _pairingAttemptIdSize ||
        attemptId.every((int value) => value == 0) ||
        attemptId.any((int value) => value < 0 || value > 0xff)) {
      throw ArgumentError('Pairing attempt ID is invalid');
    }
    final Pointer<_NativePairingAttemptRef> attempt =
        calloc<_NativePairingAttemptRef>();
    try {
      attempt.ref
        ..structSize = sizeOf<_NativePairingAttemptRef>()
        ..abiVersion = expectedAbiVersion
        ..reserved = 0;
      for (int index = 0; index < attemptId.length; index += 1) {
        attempt.ref.attemptId[index] = attemptId[index];
      }
      _requireSuccess(command(_requireHandle(), attempt), operation);
    } finally {
      calloc.free(attempt);
    }
  }

  void _runTransferCommand(
    List<int> transferId,
    _TransferCommandDart command,
    String operation,
  ) {
    if (transferId.length != _transferIdSize ||
        transferId.every((int value) => value == 0) ||
        transferId.any((int value) => value < 0 || value > 0xff)) {
      throw ArgumentError('Transfer ID is invalid');
    }
    final Pointer<_NativeTransferRef> transfer = calloc<_NativeTransferRef>();
    try {
      transfer.ref
        ..structSize = sizeOf<_NativeTransferRef>()
        ..abiVersion = expectedAbiVersion
        ..reserved = 0;
      for (int index = 0; index < transferId.length; index += 1) {
        transfer.ref.transferId[index] = transferId[index];
      }
      _requireTransferSuccess(command(_requireHandle(), transfer), operation);
    } finally {
      calloc.free(transfer);
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
          throw StateError('Native event polling failed with status $status');
        }
        if (event.ref.structSize < sizeOf<_NativeEvent>() ||
            event.ref.abiVersion != expectedAbiVersion) {
          throw StateError('Native event uses an incompatible struct');
        }
        final bool eventsDroppedBefore =
            event.ref.flags & _eventFlagEventsDroppedBefore != 0;
        switch (event.ref.type) {
          case _eventTypeEngineStateChanged:
            _events.add(_decodeEvent(event.ref));
            break;
          case _eventTypeDiscoveryPeerChanged:
            final NativeDiscoveryPeerEvent discoveryEvent =
                decodeNativeDiscoveryPeerEventPayload(
              sequence: event.ref.sequence,
              payloadVersion: event.ref.payloadVersion,
              flags: event.ref.flags,
              payload: _copyEventPayload(event.ref),
              pointerSize: sizeOf<UintPtr>(),
            );
            _discoveryEvents.add(discoveryEvent);
            break;
          case _eventTypePairingAttemptChanged:
            _pairingEvents.add(
              decodeNativePairingAttemptEventPayload(
                sequence: event.ref.sequence,
                payloadVersion: event.ref.payloadVersion,
                flags: event.ref.flags,
                payload: _copyEventPayload(event.ref),
                pointerSize: sizeOf<UintPtr>(),
              ),
            );
            break;
          case _eventTypeTrustChanged:
            _trustEvents.add(
              decodeNativeTrustEventPayload(
                sequence: event.ref.sequence,
                payloadVersion: event.ref.payloadVersion,
                flags: event.ref.flags,
                payload: _copyEventPayload(event.ref),
                pointerSize: sizeOf<UintPtr>(),
              ),
            );
            break;
          case _eventTypeTransferChanged:
            _transferEvents.add(
              decodeNativeTransferEventPayload(
                sequence: event.ref.sequence,
                payloadVersion: event.ref.payloadVersion,
                flags: event.ref.flags,
                payload: _copyEventPayload(event.ref),
                pointerSize: sizeOf<UintPtr>(),
              ),
            );
            break;
          default:
            throw StateError('Native event type is unsupported');
        }
        if (eventsDroppedBefore) {
          _publishRecoverySnapshots();
        }
      }
    } on Object catch (error, stackTrace) {
      _events.addError(error, stackTrace);
      _discoveryEvents.addError(error, stackTrace);
      _discoverySnapshots.addError(error, stackTrace);
      _pairingEvents.addError(error, stackTrace);
      _trustEvents.addError(error, stackTrace);
      _pairingSnapshots.addError(error, stackTrace);
      _trustSnapshots.addError(error, stackTrace);
      _transferEvents.addError(error, stackTrace);
      _transferSnapshots.addError(error, stackTrace);
    } finally {
      calloc.free(event);
    }
  }

  Uint8List _copyEventPayload(_NativeEvent event) {
    if (event.payloadSize > _eventPayloadMaxSize) {
      throw StateError('Native event payload size is invalid');
    }
    final Uint8List payload = Uint8List(event.payloadSize);
    for (int index = 0; index < payload.length; index += 1) {
      payload[index] = event.payload[index];
    }
    return payload;
  }

  void _publishRecoverySnapshots() {
    _discoverySnapshots.add(_readDiscoverySnapshot());
    _pairingSnapshots.add(_readPairingSnapshot());
    _trustSnapshots.add(_readTrustSnapshot());
    _transferSnapshots.add(_readTransferSnapshot());
  }

  NativeDiscoverySnapshot _readDiscoverySnapshot() {
    final Pointer<_NativeEngineHandle> handle = _requireHandle();
    final Pointer<_NativeDiscoverySnapshotPage> page =
        calloc<_NativeDiscoverySnapshotPage>();
    try {
      for (int attempt = 0; attempt < 3; attempt += 1) {
        final List<NativeDiscoveryPeer> peers = <NativeDiscoveryPeer>[];
        int revision = 0;
        int offset = 0;
        for (;;) {
          page.ref
            ..structSize = sizeOf<_NativeDiscoverySnapshotPage>()
            ..abiVersion = expectedAbiVersion;
          final int status = _discoverySnapshot(handle, revision, offset, page);
          if (status == statusStaleSnapshot) {
            break;
          }
          _requireSuccess(status, 'discovery snapshot');
          if (page.ref.structSize < sizeOf<_NativeDiscoverySnapshotPage>() ||
              page.ref.abiVersion != expectedAbiVersion ||
              page.ref.reserved != 0 ||
              page.ref.reserved2 != 0 ||
              page.ref.snapshotRevision == 0 ||
              page.ref.offset != offset ||
              page.ref.count > _discoverySnapshotPageCapacity ||
              page.ref.totalCount > _discoveryMaxPeers ||
              offset + page.ref.count > page.ref.totalCount) {
            throw StateError('Native discovery snapshot page is invalid');
          }
          if (revision == 0) {
            revision = page.ref.snapshotRevision;
          } else if (revision != page.ref.snapshotRevision) {
            throw StateError('Native discovery snapshot revision changed');
          }
          for (int index = 0; index < page.ref.count; index += 1) {
            peers.add(_decodeSnapshotPeer(page.ref.peers[index]));
          }
          offset += page.ref.count;
          if (offset == page.ref.totalCount) {
            return NativeDiscoverySnapshot(revision: revision, peers: peers);
          }
          if (page.ref.count == 0) {
            throw StateError('Native discovery snapshot made no progress');
          }
        }
      }
      throw StateError('Native discovery snapshot remained stale');
    } finally {
      calloc.free(page);
    }
  }

  NativeDiscoveryPeer _decodeSnapshotPeer(_NativeDiscoveryPeer peer) {
    final Uint8List address = Uint8List(_discoveryAddressMaxSize);
    final Uint8List displayLabel = Uint8List(_discoveryDisplayLabelMaxSize);
    for (int index = 0; index < address.length; index += 1) {
      address[index] = peer.address[index];
    }
    for (int index = 0; index < displayLabel.length; index += 1) {
      displayLabel[index] = peer.displayLabel[index];
    }
    return decodeNativeDiscoveryPeerFields(
      structSize: peer.structSize,
      abiVersion: peer.abiVersion,
      reserved: peer.reserved,
      peerId: peer.peerId,
      servicePort: peer.servicePort,
      rawAddressFamily: peer.addressFamily,
      addressSize: peer.addressSize,
      displayLabelSize: peer.displayLabelSize,
      addressBytes: address,
      displayLabelBytes: displayLabel,
      expectedStructSize: sizeOf<_NativeDiscoveryPeer>(),
    );
  }

  NativePairingSnapshot _readPairingSnapshot() {
    final Pointer<_NativePairingSnapshot> snapshot =
        calloc<_NativePairingSnapshot>();
    try {
      snapshot.ref
        ..structSize = sizeOf<_NativePairingSnapshot>()
        ..abiVersion = expectedAbiVersion;
      _requireSuccess(
        _pairingSnapshot(_requireHandle(), snapshot),
        'pairing snapshot',
      );
      if (snapshot.ref.structSize < sizeOf<_NativePairingSnapshot>() ||
          snapshot.ref.abiVersion != expectedAbiVersion ||
          snapshot.ref.reserved != 0 ||
          snapshot.ref.reserved2 != 0 ||
          snapshot.ref.snapshotRevision == 0 ||
          snapshot.ref.hasAttempt > 1) {
        throw StateError('Native pairing snapshot is invalid');
      }
      return NativePairingSnapshot(
        revision: snapshot.ref.snapshotRevision,
        attempt: snapshot.ref.hasAttempt == 0
            ? null
            : _decodePairingAttempt(snapshot.ref.attempt),
      );
    } finally {
      calloc.free(snapshot);
    }
  }

  NativeTrustSnapshot _readTrustSnapshot() {
    final Pointer<_NativeTrustSnapshotPage> page =
        calloc<_NativeTrustSnapshotPage>();
    try {
      for (int attempt = 0; attempt < 3; attempt += 1) {
        final List<NativeTrustRecord> records = <NativeTrustRecord>[];
        int revision = 0;
        int offset = 0;
        for (;;) {
          page.ref
            ..structSize = sizeOf<_NativeTrustSnapshotPage>()
            ..abiVersion = expectedAbiVersion;
          final int status = _trustSnapshot(
            _requireHandle(),
            revision,
            offset,
            page,
          );
          if (status == statusStaleSnapshot) {
            break;
          }
          _requireSuccess(status, 'trust snapshot');
          if (page.ref.structSize < sizeOf<_NativeTrustSnapshotPage>() ||
              page.ref.abiVersion != expectedAbiVersion ||
              page.ref.reserved != 0 ||
              page.ref.reserved2 != 0 ||
              page.ref.snapshotRevision == 0 ||
              page.ref.offset != offset ||
              page.ref.count > _trustSnapshotPageCapacity ||
              page.ref.totalCount > _trustMaxRecords ||
              offset + page.ref.count > page.ref.totalCount) {
            throw StateError('Native trust snapshot page is invalid');
          }
          if (revision == 0) {
            revision = page.ref.snapshotRevision;
          } else if (revision != page.ref.snapshotRevision) {
            throw StateError('Native trust snapshot revision changed');
          }
          for (int index = 0; index < page.ref.count; index += 1) {
            records.add(_decodeTrustRecord(page.ref.records[index]));
          }
          offset += page.ref.count;
          if (offset == page.ref.totalCount) {
            return NativeTrustSnapshot(revision: revision, records: records);
          }
          if (page.ref.count == 0) {
            throw StateError('Native trust snapshot made no progress');
          }
        }
      }
      throw StateError('Native trust snapshot remained stale');
    } finally {
      calloc.free(page);
    }
  }

  NativeTransferSnapshot _readTransferSnapshot() {
    final Pointer<_NativeTransferSnapshotPage> page =
        calloc<_NativeTransferSnapshotPage>();
    try {
      for (int attempt = 0; attempt < 3; attempt += 1) {
        final List<NativeTransferRecord> records = <NativeTransferRecord>[];
        int revision = 0;
        int offset = 0;
        for (;;) {
          page.ref
            ..structSize = sizeOf<_NativeTransferSnapshotPage>()
            ..abiVersion = expectedAbiVersion;
          final int status = _transferSnapshot(
            _requireHandle(),
            revision,
            offset,
            page,
          );
          if (status == statusStaleSnapshot) {
            break;
          }
          _requireTransferSuccess(status, 'transfer snapshot');
          if (page.ref.structSize < sizeOf<_NativeTransferSnapshotPage>() ||
              page.ref.abiVersion != expectedAbiVersion ||
              page.ref.reserved != 0 ||
              page.ref.reserved2 != 0 ||
              page.ref.snapshotRevision == 0 ||
              page.ref.offset != offset ||
              page.ref.count > _transferSnapshotPageCapacity ||
              page.ref.totalCount > _transferMaxRecords ||
              offset + page.ref.count > page.ref.totalCount) {
            throw StateError('Native transfer snapshot page is invalid');
          }
          if (revision == 0) {
            revision = page.ref.snapshotRevision;
          } else if (revision != page.ref.snapshotRevision) {
            throw StateError('Native transfer snapshot revision changed');
          }
          for (int index = 0; index < page.ref.count; index += 1) {
            final NativeTransferRecord record =
                _decodeTransferRecord(page.ref.records[index]);
            if (record.change != NativeTransferChange.upserted) {
              throw StateError('Native transfer snapshot contains a removal');
            }
            records.add(record);
          }
          offset += page.ref.count;
          if (offset == page.ref.totalCount) {
            return NativeTransferSnapshot(
              revision: revision,
              records: records,
            );
          }
          if (page.ref.count == 0) {
            throw StateError('Native transfer snapshot made no progress');
          }
        }
      }
      throw StateError('Native transfer snapshot remained stale');
    } finally {
      calloc.free(page);
    }
  }

  NativePairingAttempt _decodePairingAttempt(
    _NativePairingAttemptEventPayload attempt,
  ) {
    return decodeNativePairingAttemptFields(
      structSize: attempt.structSize,
      abiVersion: attempt.abiVersion,
      rawState: attempt.state,
      peerId: attempt.peerId,
      deadlineMs: attempt.deadlineMs,
      attemptId: <int>[
        for (int index = 0; index < _pairingAttemptIdSize; index += 1)
          attempt.attemptId[index],
      ],
      sasWordIndices: <int>[
        for (int index = 0; index < _pairingSasWordCount; index += 1)
          attempt.sasWordIndices[index],
      ],
      sasWordCount: attempt.sasWordCount,
      rawError: attempt.error,
      reserved: attempt.reserved,
      expectedStructSize: sizeOf<_NativePairingAttemptEventPayload>(),
    );
  }

  NativeTrustRecord _decodeTrustRecord(_NativeTrustEventPayload record) {
    return decodeNativeTrustFields(
      structSize: record.structSize,
      abiVersion: record.abiVersion,
      rawState: record.state,
      trustId: record.trustId,
      peerId: record.peerId,
      reserved: record.reserved,
      reserved2: record.reserved2,
      expectedStructSize: sizeOf<_NativeTrustEventPayload>(),
    );
  }

  NativeTransferRecord _decodeTransferRecord(
    _NativeTransferEventPayload record,
  ) {
    final Uint8List peerLabel = Uint8List(_transferPeerLabelMaxSize);
    for (int index = 0; index < peerLabel.length; index += 1) {
      peerLabel[index] = record.peerLabel[index];
    }
    return decodeNativeTransferFields(
      structSize: record.structSize,
      abiVersion: record.abiVersion,
      rawChange: record.change,
      revision: record.snapshotRevision,
      rawDirection: record.direction,
      rawState: record.state,
      rawError: record.error,
      peerLabelSize: record.peerLabelSize,
      reserved: record.reserved,
      reserved2: record.reserved2,
      totalBytes: record.totalBytes,
      transferredBytes: record.transferredBytes,
      transferId: <int>[
        for (int index = 0; index < _transferIdSize; index += 1)
          record.transferId[index],
      ],
      peerLabelBytes: peerLabel,
      expectedStructSize: sizeOf<_NativeTransferEventPayload>(),
    );
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
    final int payloadAbiVersion = payload.getUint32(
      sizeFieldWidth,
      Endian.host,
    );
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
