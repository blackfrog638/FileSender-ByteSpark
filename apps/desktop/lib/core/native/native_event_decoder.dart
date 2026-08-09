import 'dart:convert';
import 'dart:typed_data';

const int nativeDiscoveryPeerEventPayloadVersion = 1;
const int nativeDiscoveryDisplayLabelMaxSize = 96;
const int nativeDiscoveryAddressMaxSize = 16;

enum NativeDiscoveryAddressFamily { ipv4, ipv6 }

enum NativeDiscoveryPeerChange { appeared, updated, expired }

enum NativeDiscoveryExpiryReason {
  none,
  ttl,
  withdrawn,
  interfaceRemoved,
  wake,
  discoveryStopped,
}

final class NativeDiscoveryPeer {
  NativeDiscoveryPeer({
    required this.peerId,
    required this.servicePort,
    required this.addressFamily,
    required Uint8List address,
    required this.displayLabel,
  }) : address = Uint8List.fromList(address);

  final int peerId;
  final int servicePort;
  final NativeDiscoveryAddressFamily addressFamily;
  final Uint8List address;
  final String displayLabel;
}

final class NativeDiscoveryPeerEvent {
  const NativeDiscoveryPeerEvent({
    required this.sequence,
    required this.snapshotRevision,
    required this.change,
    required this.expiryReason,
    required this.peer,
    required this.eventsDroppedBefore,
  });

  final int sequence;
  final int snapshotRevision;
  final NativeDiscoveryPeerChange change;
  final NativeDiscoveryExpiryReason expiryReason;
  final NativeDiscoveryPeer peer;
  final bool eventsDroppedBefore;
}

final class NativeDiscoverySnapshot {
  NativeDiscoverySnapshot({
    required this.revision,
    required List<NativeDiscoveryPeer> peers,
  }) : peers = List<NativeDiscoveryPeer>.unmodifiable(peers);

  final int revision;
  final List<NativeDiscoveryPeer> peers;
}

NativeDiscoveryPeer decodeNativeDiscoveryPeerFields({
  required int structSize,
  required int abiVersion,
  required int reserved,
  required int peerId,
  required int servicePort,
  required int rawAddressFamily,
  required int addressSize,
  required int displayLabelSize,
  required Uint8List addressBytes,
  required Uint8List displayLabelBytes,
  required int expectedStructSize,
}) {
  if (structSize < expectedStructSize || abiVersion != 1 || reserved != 0) {
    throw StateError('Native discovery peer uses an incompatible struct');
  }
  if (peerId == 0 || servicePort == 0) {
    throw StateError('Native discovery peer has an invalid identity or port');
  }
  final NativeDiscoveryAddressFamily addressFamily;
  final int expectedAddressSize;
  switch (rawAddressFamily) {
    case 4:
      addressFamily = NativeDiscoveryAddressFamily.ipv4;
      expectedAddressSize = 4;
      break;
    case 6:
      addressFamily = NativeDiscoveryAddressFamily.ipv6;
      expectedAddressSize = 16;
      break;
    default:
      throw StateError('Native discovery address family is unsupported');
  }
  if (addressSize != expectedAddressSize ||
      addressSize > nativeDiscoveryAddressMaxSize ||
      addressBytes.length < addressSize) {
    throw StateError('Native discovery address size is invalid');
  }
  if (displayLabelSize > nativeDiscoveryDisplayLabelMaxSize ||
      displayLabelBytes.length < displayLabelSize) {
    throw StateError('Native discovery display label size is invalid');
  }

  final Uint8List address = Uint8List.fromList(
    addressBytes.sublist(0, addressSize),
  );
  final String displayLabel = const Utf8Decoder(
    allowMalformed: false,
  ).convert(displayLabelBytes.sublist(0, displayLabelSize));
  return NativeDiscoveryPeer(
    peerId: peerId,
    servicePort: servicePort,
    addressFamily: addressFamily,
    address: address,
    displayLabel: displayLabel,
  );
}

NativeDiscoveryPeerEvent decodeNativeDiscoveryPeerEventPayload({
  required int sequence,
  required int payloadVersion,
  required int flags,
  required Uint8List payload,
  required int pointerSize,
}) {
  if (payloadVersion != nativeDiscoveryPeerEventPayloadVersion ||
      pointerSize != 8 ||
      payload.length < 176) {
    throw StateError('Native discovery event payload is incompatible');
  }

  final ByteData fields = ByteData.sublistView(payload);
  final int structSize = fields.getUint64(0, Endian.host);
  final int abiVersion = fields.getUint32(8, Endian.host);
  final int rawChange = fields.getUint32(12, Endian.host);
  final int snapshotRevision = fields.getUint64(16, Endian.host);
  final int rawExpiryReason = fields.getUint32(24, Endian.host);
  final int reserved = fields.getUint32(28, Endian.host);
  if (structSize < 176 ||
      structSize > payload.length ||
      abiVersion != 1 ||
      reserved != 0 ||
      snapshotRevision == 0) {
    throw StateError('Native discovery event header is invalid');
  }
  final int peerStructSize = fields.getUint64(32, Endian.host);
  if (peerStructSize < 144 || 32 + peerStructSize > payload.length) {
    throw StateError('Native discovery peer payload is truncated');
  }

  final NativeDiscoveryPeerChange change = switch (rawChange) {
    1 => NativeDiscoveryPeerChange.appeared,
    2 => NativeDiscoveryPeerChange.updated,
    3 => NativeDiscoveryPeerChange.expired,
    _ => throw StateError('Native discovery peer change is unsupported'),
  };
  final NativeDiscoveryExpiryReason expiryReason = switch (rawExpiryReason) {
    0 => NativeDiscoveryExpiryReason.none,
    1 => NativeDiscoveryExpiryReason.ttl,
    2 => NativeDiscoveryExpiryReason.withdrawn,
    3 => NativeDiscoveryExpiryReason.interfaceRemoved,
    4 => NativeDiscoveryExpiryReason.wake,
    5 => NativeDiscoveryExpiryReason.discoveryStopped,
    _ => throw StateError('Native discovery expiry reason is unsupported'),
  };
  if ((change == NativeDiscoveryPeerChange.expired) !=
      (expiryReason != NativeDiscoveryExpiryReason.none)) {
    throw StateError('Native discovery change and expiry reason disagree');
  }

  final NativeDiscoveryPeer peer = decodeNativeDiscoveryPeerFields(
    structSize: peerStructSize,
    abiVersion: fields.getUint32(40, Endian.host),
    reserved: fields.getUint32(44, Endian.host),
    peerId: fields.getUint64(48, Endian.host),
    servicePort: fields.getUint16(56, Endian.host),
    rawAddressFamily: fields.getUint8(58),
    addressSize: fields.getUint8(59),
    displayLabelSize: fields.getUint32(60, Endian.host),
    addressBytes: Uint8List.fromList(payload.sublist(64, 80)),
    displayLabelBytes: Uint8List.fromList(payload.sublist(80, 176)),
    expectedStructSize: 144,
  );
  return NativeDiscoveryPeerEvent(
    sequence: sequence,
    snapshotRevision: snapshotRevision,
    change: change,
    expiryReason: expiryReason,
    peer: peer,
    eventsDroppedBefore: flags & 1 != 0,
  );
}

const int nativePairingAttemptEventPayloadVersion = 1;
const int nativeTrustEventPayloadVersion = 1;
const int nativePairingAttemptIdSize = 16;
const int nativePairingSasWordCount = 5;

enum NativePairingAttemptState {
  starting,
  awaitingConfirmation,
  paired,
  closed,
}

enum NativePairingError {
  none,
  rejected,
  cancelled,
  timedOut,
  busy,
  unavailable,
  failed,
}

enum NativeTrustState { active, revoked }

final class NativePairingAttempt {
  NativePairingAttempt({
    required this.state,
    required this.peerId,
    required this.deadlineMs,
    required List<int> attemptId,
    required List<int> sasWordIndices,
    required this.error,
  })  : attemptId = List<int>.unmodifiable(attemptId),
        sasWordIndices = List<int>.unmodifiable(sasWordIndices);

  final NativePairingAttemptState state;
  final int peerId;
  final int deadlineMs;
  final List<int> attemptId;
  final List<int> sasWordIndices;
  final NativePairingError error;
}

final class NativePairingAttemptEvent {
  const NativePairingAttemptEvent({
    required this.sequence,
    required this.attempt,
    required this.eventsDroppedBefore,
  });

  final int sequence;
  final NativePairingAttempt attempt;
  final bool eventsDroppedBefore;
}

final class NativePairingSnapshot {
  const NativePairingSnapshot({required this.revision, required this.attempt});

  final int revision;
  final NativePairingAttempt? attempt;
}

final class NativeTrustRecord {
  const NativeTrustRecord({
    required this.trustId,
    required this.peerId,
    required this.state,
  });

  final int trustId;
  final int peerId;
  final NativeTrustState state;
}

final class NativeTrustEvent {
  const NativeTrustEvent({
    required this.sequence,
    required this.record,
    required this.eventsDroppedBefore,
  });

  final int sequence;
  final NativeTrustRecord record;
  final bool eventsDroppedBefore;
}

final class NativeTrustSnapshot {
  NativeTrustSnapshot({
    required this.revision,
    required List<NativeTrustRecord> records,
  }) : records = List<NativeTrustRecord>.unmodifiable(records);

  final int revision;
  final List<NativeTrustRecord> records;
}

NativePairingAttempt decodeNativePairingAttemptFields({
  required int structSize,
  required int abiVersion,
  required int rawState,
  required int peerId,
  required int deadlineMs,
  required List<int> attemptId,
  required List<int> sasWordIndices,
  required int sasWordCount,
  required int rawError,
  required int reserved,
  required int expectedStructSize,
}) {
  if (structSize < expectedStructSize ||
      abiVersion != 1 ||
      reserved != 0 ||
      peerId < 0 ||
      attemptId.length != nativePairingAttemptIdSize ||
      attemptId.every((int value) => value == 0) ||
      sasWordIndices.length != nativePairingSasWordCount) {
    throw StateError('Native pairing attempt uses an incompatible struct');
  }

  final NativePairingAttemptState state = switch (rawState) {
    1 => NativePairingAttemptState.starting,
    2 => NativePairingAttemptState.awaitingConfirmation,
    3 => NativePairingAttemptState.paired,
    4 => NativePairingAttemptState.closed,
    _ => throw StateError('Native pairing attempt state is unsupported'),
  };
  final NativePairingError error = switch (rawError) {
    0 => NativePairingError.none,
    1 => NativePairingError.rejected,
    2 => NativePairingError.cancelled,
    3 => NativePairingError.timedOut,
    4 => NativePairingError.busy,
    5 => NativePairingError.unavailable,
    6 => NativePairingError.failed,
    _ => throw StateError('Native pairing error is unsupported'),
  };

  final bool awaiting = state == NativePairingAttemptState.awaitingConfirmation;
  if (awaiting) {
    if (deadlineMs <= 0 ||
        sasWordCount != nativePairingSasWordCount ||
        error != NativePairingError.none ||
        sasWordIndices.any((int value) => value < 0 || value >= 2048)) {
      throw StateError('Native pairing SAS payload is invalid');
    }
  } else if (deadlineMs != 0 ||
      sasWordCount != 0 ||
      sasWordIndices.any((int value) => value != 0) ||
      (state == NativePairingAttemptState.closed) ==
          (error == NativePairingError.none)) {
    throw StateError('Native pairing terminal payload is invalid');
  }

  return NativePairingAttempt(
    state: state,
    peerId: peerId,
    deadlineMs: deadlineMs,
    attemptId: attemptId,
    sasWordIndices: awaiting ? sasWordIndices : const <int>[],
    error: error,
  );
}

NativePairingAttemptEvent decodeNativePairingAttemptEventPayload({
  required int sequence,
  required int payloadVersion,
  required int flags,
  required Uint8List payload,
  required int pointerSize,
}) {
  if (payloadVersion != nativePairingAttemptEventPayloadVersion ||
      pointerSize != 8 ||
      payload.length < 64) {
    throw StateError('Native pairing event payload is incompatible');
  }

  final ByteData fields = ByteData.sublistView(payload);
  final int structSize = fields.getUint64(0, Endian.host);
  if (structSize < 64 || structSize > payload.length) {
    throw StateError('Native pairing event payload is truncated');
  }
  final List<int> words = <int>[
    for (int index = 0; index < nativePairingSasWordCount; index += 1)
      fields.getUint16(48 + index * 2, Endian.host),
  ];
  return NativePairingAttemptEvent(
    sequence: sequence,
    attempt: decodeNativePairingAttemptFields(
      structSize: structSize,
      abiVersion: fields.getUint32(8, Endian.host),
      rawState: fields.getUint32(12, Endian.host),
      peerId: fields.getUint64(16, Endian.host),
      deadlineMs: fields.getUint64(24, Endian.host),
      attemptId: List<int>.from(payload.sublist(32, 48)),
      sasWordIndices: words,
      sasWordCount: fields.getUint16(58, Endian.host),
      rawError: fields.getUint16(60, Endian.host),
      reserved: fields.getUint16(62, Endian.host),
      expectedStructSize: 64,
    ),
    eventsDroppedBefore: flags & 1 != 0,
  );
}

NativeTrustRecord decodeNativeTrustFields({
  required int structSize,
  required int abiVersion,
  required int rawState,
  required int trustId,
  required int peerId,
  required int reserved,
  required int reserved2,
  required int expectedStructSize,
}) {
  if (structSize < expectedStructSize ||
      abiVersion != 1 ||
      trustId <= 0 ||
      peerId < 0 ||
      reserved != 0 ||
      reserved2 != 0) {
    throw StateError('Native trust record uses an incompatible struct');
  }
  final NativeTrustState state = switch (rawState) {
    1 => NativeTrustState.active,
    2 => NativeTrustState.revoked,
    _ => throw StateError('Native trust state is unsupported'),
  };
  return NativeTrustRecord(trustId: trustId, peerId: peerId, state: state);
}

NativeTrustEvent decodeNativeTrustEventPayload({
  required int sequence,
  required int payloadVersion,
  required int flags,
  required Uint8List payload,
  required int pointerSize,
}) {
  if (payloadVersion != nativeTrustEventPayloadVersion ||
      pointerSize != 8 ||
      payload.length < 40) {
    throw StateError('Native trust event payload is incompatible');
  }

  final ByteData fields = ByteData.sublistView(payload);
  final int structSize = fields.getUint64(0, Endian.host);
  if (structSize < 40 || structSize > payload.length) {
    throw StateError('Native trust event payload is truncated');
  }
  return NativeTrustEvent(
    sequence: sequence,
    record: decodeNativeTrustFields(
      structSize: structSize,
      abiVersion: fields.getUint32(8, Endian.host),
      rawState: fields.getUint32(12, Endian.host),
      trustId: fields.getUint64(16, Endian.host),
      peerId: fields.getUint64(24, Endian.host),
      reserved: fields.getUint32(32, Endian.host),
      reserved2: fields.getUint32(36, Endian.host),
      expectedStructSize: 40,
    ),
    eventsDroppedBefore: flags & 1 != 0,
  );
}
