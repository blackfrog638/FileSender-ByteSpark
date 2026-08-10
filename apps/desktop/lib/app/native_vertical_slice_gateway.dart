import 'dart:async';
import 'dart:io';

import 'package:xnn_transfer/app/sas_word_list.dart';
import 'package:xnn_transfer/core/native/native_engine.dart';
import 'package:xnn_transfer/core/native/native_engine_gateway.dart';
import 'package:xnn_transfer/core/native/native_event_decoder.dart';
import 'package:xnn_transfer/features/transfer/domain/transfer_gateway.dart';
import 'package:xnn_transfer/features/transfer/domain/transfer_models.dart';
import 'package:xnn_transfer/features/transfer/domain/vertical_slice_gateway.dart';
import 'package:xnn_transfer/features/transfer/domain/vertical_slice_models.dart';

typedef FilePathSelector = Future<String?> Function();

final class NativeVerticalSliceGateway implements VerticalSliceGateway {
  NativeVerticalSliceGateway({
    NativeEngine? engine,
    FilePathSelector? filePathSelector,
    int? servicePort,
  })  : _engine = engine ?? NativeEngine.open(),
        _filePathSelector = filePathSelector ?? selectPlatformFile,
        _servicePort = servicePort {
    _transfers = NativeTransferGateway(
      engine: _engine,
      manageEngineLifecycle: false,
    );
  }

  final NativeEngine _engine;
  final FilePathSelector _filePathSelector;
  final int? _servicePort;
  late final NativeTransferGateway _transfers;
  final StreamController<VerticalSliceGatewayEvent> _verticalSliceEvents =
      StreamController<VerticalSliceGatewayEvent>.broadcast();
  final List<StreamSubscription<Object?>> _subscriptions =
      <StreamSubscription<Object?>>[];
  final Map<String, List<int>> _attemptIds = <String, List<int>>{};
  final Map<String, int> _activeTrustByPeer = <String, int>{};
  final Map<String, NearbyPeer> _peers = <String, NearbyPeer>{};

  bool _initialized = false;
  bool _discoveryStarted = false;
  bool _disposed = false;
  String? _publishedPairingAttemptId;

  @override
  Stream<TransferGatewayEvent> get events => _transfers.events;

  @override
  Stream<VerticalSliceGatewayEvent> get verticalSliceEvents =>
      _verticalSliceEvents.stream;

  @override
  Future<void> initialize() async {
    if (_disposed) {
      throw StateError('Native vertical slice gateway is disposed');
    }
    if (_initialized) {
      return;
    }

    _subscribeToNativeState();
    try {
      await _transfers.initialize();
      _reconcileDiscovery(_engine.discoverySnapshot());
      _reconcilePairing(_engine.pairingSnapshot());
      _reconcileTrust(_engine.trustSnapshot());
      final int? servicePort = _servicePort;
      if (servicePort == null) {
        _verticalSliceEvents.add(
          const VerticalSliceGatewayNotice(
            'LAN discovery is waiting for the native service endpoint.',
          ),
        );
      } else {
        _engine.startDiscovery(servicePort: servicePort);
        _discoveryStarted = true;
      }
      _initialized = true;
    } on Object {
      await _cancelSubscriptions();
      rethrow;
    }
  }

  void _subscribeToNativeState() {
    _subscriptions
      ..add(
        _engine.discoveryEvents.listen(
          _onDiscoveryEvent,
          onError: _onNativeStreamError,
        ),
      )
      ..add(
        _engine.discoverySnapshots.listen(
          _reconcileDiscovery,
          onError: _onNativeStreamError,
        ),
      )
      ..add(
        _engine.pairingEvents.listen(
          _onPairingEvent,
          onError: _onNativeStreamError,
        ),
      )
      ..add(
        _engine.pairingSnapshots.listen(
          _reconcilePairing,
          onError: _onNativeStreamError,
        ),
      )
      ..add(
        _engine.trustEvents.listen(
          _onTrustEvent,
          onError: _onNativeStreamError,
        ),
      )
      ..add(
        _engine.trustSnapshots.listen(
          _reconcileTrust,
          onError: _onNativeStreamError,
        ),
      );
  }

  void _onDiscoveryEvent(NativeDiscoveryPeerEvent event) {
    if (_disposed) {
      return;
    }
    final String peerId = _peerId(event.peer.peerId);
    if (event.change == NativeDiscoveryPeerChange.expired) {
      _peers.remove(peerId);
      _verticalSliceEvents.add(NearbyPeerExpired(peerId));
      return;
    }
    _publishPeer(event.peer);
  }

  void _reconcileDiscovery(NativeDiscoverySnapshot snapshot) {
    if (_disposed) {
      return;
    }
    final Set<String> nextIds = snapshot.peers
        .map((NativeDiscoveryPeer peer) => _peerId(peer.peerId))
        .toSet();
    for (final String currentId in _peers.keys.toList(growable: false)) {
      if (!nextIds.contains(currentId)) {
        _peers.remove(currentId);
        _verticalSliceEvents.add(NearbyPeerExpired(currentId));
      }
    }
    for (final NativeDiscoveryPeer peer in snapshot.peers) {
      _publishPeer(peer);
    }
  }

  void _publishPeer(NativeDiscoveryPeer peer) {
    final String peerId = _peerId(peer.peerId);
    final NearbyPeer mapped = NearbyPeer(
      id: peerId,
      displayName:
          peer.displayLabel.isEmpty ? 'Nearby device' : peer.displayLabel,
      trust: _activeTrustByPeer.containsKey(peerId)
          ? PeerTrust.authenticated
          : PeerTrust.untrusted,
      isAvailable: true,
    );
    _peers[peerId] = mapped;
    _verticalSliceEvents.add(NearbyPeerUpserted(mapped));
  }

  void _onPairingEvent(NativePairingAttemptEvent event) {
    if (_disposed) {
      return;
    }
    _publishPairingAttempt(event.attempt);
  }

  void _reconcilePairing(NativePairingSnapshot snapshot) {
    if (_disposed) {
      return;
    }
    final NativePairingAttempt? attempt = snapshot.attempt;
    if (attempt == null) {
      _clearPublishedPairingAttempt();
      return;
    }
    _publishPairingAttempt(attempt);
  }

  void _publishPairingAttempt(NativePairingAttempt attempt) {
    final String attemptId = _opaqueId(attempt.attemptId);
    final String? publishedAttemptId = _publishedPairingAttemptId;
    if (publishedAttemptId != null && publishedAttemptId != attemptId) {
      _clearPublishedPairingAttempt();
    }
    final bool terminal = attempt.state == NativePairingAttemptState.paired ||
        attempt.state == NativePairingAttemptState.closed;
    if (!terminal) {
      _publishedPairingAttemptId = attemptId;
    }
    _attemptIds[attemptId] = attempt.attemptId;
    final PairingGatewayAttempt mapped = PairingGatewayAttempt(
      id: attemptId,
      peerId: _peerId(attempt.peerId),
      state: switch (attempt.state) {
        NativePairingAttemptState.starting => PairingGatewayState.starting,
        NativePairingAttemptState.awaitingConfirmation =>
          PairingGatewayState.awaitingConfirmation,
        NativePairingAttemptState.paired => PairingGatewayState.paired,
        NativePairingAttemptState.closed => PairingGatewayState.closed,
      },
      sasWords: attempt.sasWordIndices.map(sasWordForIndex),
      error: switch (attempt.error) {
        NativePairingError.none => PairingGatewayError.none,
        NativePairingError.rejected => PairingGatewayError.rejected,
        NativePairingError.cancelled => PairingGatewayError.cancelled,
        NativePairingError.timedOut => PairingGatewayError.timedOut,
        NativePairingError.busy => PairingGatewayError.busy,
        NativePairingError.unavailable => PairingGatewayError.unavailable,
        NativePairingError.failed => PairingGatewayError.failed,
      },
    );
    _verticalSliceEvents.add(PairingAttemptUpdated(mapped));
    if (terminal) {
      _attemptIds.remove(attemptId);
      if (_publishedPairingAttemptId == attemptId) {
        _publishedPairingAttemptId = null;
      }
    }
  }

  void _clearPublishedPairingAttempt() {
    final String? attemptId = _publishedPairingAttemptId;
    if (attemptId == null) {
      return;
    }
    _publishedPairingAttemptId = null;
    _attemptIds.remove(attemptId);
    _verticalSliceEvents.add(PairingAttemptCleared(attemptId));
  }

  void _onTrustEvent(NativeTrustEvent event) {
    if (_disposed) {
      return;
    }
    _publishTrust(event.record);
  }

  void _reconcileTrust(NativeTrustSnapshot snapshot) {
    if (_disposed) {
      return;
    }
    final Map<String, int> next = <String, int>{};
    for (final NativeTrustRecord record in snapshot.records) {
      if (record.state == NativeTrustState.active) {
        next[_peerId(record.peerId)] = record.trustId;
      }
    }
    for (final String peerId in _activeTrustByPeer.keys) {
      if (!next.containsKey(peerId)) {
        _verticalSliceEvents.add(
          PeerTrustUpdated(peerId: peerId, isActive: false),
        );
      }
    }
    _activeTrustByPeer
      ..clear()
      ..addAll(next);
    for (final String peerId in next.keys) {
      _verticalSliceEvents.add(
        PeerTrustUpdated(peerId: peerId, isActive: true),
      );
    }
  }

  void _publishTrust(NativeTrustRecord record) {
    final String peerId = _peerId(record.peerId);
    final bool isActive = record.state == NativeTrustState.active;
    if (isActive) {
      _activeTrustByPeer[peerId] = record.trustId;
    } else {
      _activeTrustByPeer.remove(peerId);
    }
    _verticalSliceEvents.add(
      PeerTrustUpdated(peerId: peerId, isActive: isActive),
    );
  }

  @override
  Future<void> startPairing(String peerId) async {
    _requireInitialized();
    _engine.startPairing(_decodePeerId(peerId));
  }

  @override
  Future<void> confirmPairing(String attemptId) async {
    _requireInitialized();
    _engine.confirmPairing(_requireAttemptId(attemptId));
  }

  @override
  Future<void> rejectPairing(String attemptId) async {
    _requireInitialized();
    _engine.rejectPairing(_requireAttemptId(attemptId));
  }

  @override
  Future<SendSelectionOutcome> selectAndSendFile(String peerId) async {
    _requireInitialized();
    final int? trustId = _activeTrustByPeer[peerId];
    if (trustId == null) {
      throw StateError('The peer is not authenticated');
    }
    final String? path = await _filePathSelector();
    if (path == null) {
      return SendSelectionOutcome.cancelled;
    }
    _engine.sendFile(trustId: trustId, path: path);
    return SendSelectionOutcome.submitted;
  }

  @override
  Future<TransferEntry> acceptOffer(String offerId) {
    return _transfers.acceptOffer(offerId);
  }

  @override
  Future<void> rejectOffer(String offerId) {
    return _transfers.rejectOffer(offerId);
  }

  @override
  Future<void> pauseTransfer(String transferId) {
    return _transfers.pauseTransfer(transferId);
  }

  @override
  Future<void> resumeTransfer(String transferId) {
    return _transfers.resumeTransfer(transferId);
  }

  @override
  Future<void> cancelTransfer(String transferId) {
    return _transfers.cancelTransfer(transferId);
  }

  List<int> _requireAttemptId(String attemptId) {
    final List<int>? bytes = _attemptIds[attemptId];
    if (bytes == null) {
      throw StateError('The pairing attempt is stale');
    }
    return bytes;
  }

  void _requireInitialized() {
    if (!_initialized || _disposed) {
      throw StateError('Native vertical slice gateway is not initialized');
    }
  }

  void _onNativeStreamError(Object error, StackTrace stackTrace) {
    if (!_disposed) {
      _verticalSliceEvents.add(
        const VerticalSliceGatewayNotice(
          'Native peer updates are temporarily unavailable.',
        ),
      );
    }
  }

  Future<void> _cancelSubscriptions() async {
    final List<StreamSubscription<Object?>> subscriptions =
        List<StreamSubscription<Object?>>.of(_subscriptions);
    _subscriptions.clear();
    await Future.wait<void>(
      subscriptions.map(
        (StreamSubscription<Object?> subscription) => subscription.cancel(),
      ),
    );
  }

  @override
  void dispose() {
    if (_disposed) {
      return;
    }
    _disposed = true;
    if (_discoveryStarted) {
      try {
        _engine.stopDiscovery();
      } on Object {
        // Engine disposal is the final shutdown barrier.
      }
    }
    unawaited(_cancelSubscriptions());
    _transfers.dispose();
    _engine.dispose();
    unawaited(_verticalSliceEvents.close());
  }

  static String _peerId(int peerId) => peerId.toString();

  static int _decodePeerId(String peerId) {
    final int? decoded = int.tryParse(peerId);
    if (decoded == null || decoded <= 0) {
      throw StateError('The peer observation is stale');
    }
    return decoded;
  }

  static String _opaqueId(List<int> bytes) {
    return bytes
        .map((int value) => value.toRadixString(16).padLeft(2, '0'))
        .join();
  }
}

Future<String?> selectPlatformFile() async {
  return switch (Platform.operatingSystem) {
    'macos' => _selectMacOsFile(),
    'windows' => _selectWindowsFile(),
    'linux' => _selectLinuxFile(),
    _ => throw UnsupportedError('File selection is unavailable'),
  };
}

Future<String?> _selectMacOsFile() async {
  const String script = '''
try
  return POSIX path of (choose file with prompt "Choose one file to send")
on error number -128
  return ""
end try
''';
  final ProcessResult result = await Process.run('/usr/bin/osascript', <String>[
    '-e',
    script,
  ]);
  if (result.exitCode != 0) {
    throw StateError('The system file selector failed');
  }
  return _selectedPath(result.stdout.toString());
}

Future<String?> _selectWindowsFile() async {
  const String script = r'''
Add-Type -AssemblyName System.Windows.Forms
$dialog = New-Object System.Windows.Forms.OpenFileDialog
$dialog.Multiselect = $false
$dialog.CheckFileExists = $true
if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
  [Console]::Write($dialog.FileName)
}
''';
  final ProcessResult result = await Process.run('powershell.exe', <String>[
    '-NoProfile',
    '-STA',
    '-Command',
    script,
  ]);
  if (result.exitCode != 0) {
    throw StateError('The system file selector failed');
  }
  return _selectedPath(result.stdout.toString());
}

Future<String?> _selectLinuxFile() async {
  final List<(String, List<String>)> selectors = <(String, List<String>)>[
    ('zenity', <String>['--file-selection', '--title=Choose one file to send']),
    ('kdialog', <String>['--getopenfilename', '.', 'All files (*)']),
  ];
  for (final (String executable, List<String> arguments) in selectors) {
    try {
      final ProcessResult result = await Process.run(executable, arguments);
      if (result.exitCode == 0) {
        return _selectedPath(result.stdout.toString());
      }
      if (result.exitCode == 1) {
        return null;
      }
    } on ProcessException {
      continue;
    }
  }
  throw StateError('No supported system file selector is available');
}

String? _selectedPath(String output) {
  String value = output;
  if (value.endsWith('\r\n')) {
    value = value.substring(0, value.length - 2);
  } else if (value.endsWith('\n')) {
    value = value.substring(0, value.length - 1);
  }
  return value.isEmpty ? null : value;
}
