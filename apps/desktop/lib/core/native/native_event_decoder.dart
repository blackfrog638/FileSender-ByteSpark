import 'dart:convert';
import 'dart:typed_data';

const int nativeDiscoveryPeerEventPayloadVersion = 1;
const int nativeDiscoveryDisplayLabelMaxSize = 96;
const int nativeDiscoveryAddressMaxSize = 16;

enum NativeDiscoveryAddressFamily {
  ipv4,
  ipv6,
}

enum NativeDiscoveryPeerChange {
  appeared,
  updated,
  expired,
}

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

  final Uint8List address =
      Uint8List.fromList(addressBytes.sublist(0, addressSize));
  final String displayLabel = const Utf8Decoder(allowMalformed: false)
      .convert(displayLabelBytes.sublist(0, displayLabelSize));
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
