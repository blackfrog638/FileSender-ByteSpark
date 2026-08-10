import 'package:xnn_transfer/features/transfer/application/transfer_state.dart';
import 'package:xnn_transfer/features/transfer/domain/vertical_slice_models.dart';

const Object _unchanged = Object();

sealed class VerticalSliceState {
  const VerticalSliceState();
}

final class VerticalSliceInitializing extends VerticalSliceState {
  const VerticalSliceInitializing();
}

final class VerticalSliceUnavailable extends VerticalSliceState {
  const VerticalSliceUnavailable(this.reason);

  final String reason;
}

final class VerticalSliceReady extends VerticalSliceState {
  VerticalSliceReady({
    Iterable<NearbyPeer> peers = const <NearbyPeer>[],
    this.pairing,
    this.pairingMessage,
    this.notice,
    TransferReady? transfers,
  })  : peers = List<NearbyPeer>.unmodifiable(peers),
        transfers = transfers ?? TransferReady();

  final List<NearbyPeer> peers;
  final PairingCeremony? pairing;
  final String? pairingMessage;
  final String? notice;
  final TransferReady transfers;

  NearbyPeer? peerById(String peerId) {
    for (final NearbyPeer peer in peers) {
      if (peer.id == peerId) {
        return peer;
      }
    }
    return null;
  }

  VerticalSliceReady copyWith({
    Iterable<NearbyPeer>? peers,
    Object? pairing = _unchanged,
    Object? pairingMessage = _unchanged,
    Object? notice = _unchanged,
    TransferReady? transfers,
  }) {
    return VerticalSliceReady(
      peers: peers ?? this.peers,
      pairing: identical(pairing, _unchanged)
          ? this.pairing
          : pairing as PairingCeremony?,
      pairingMessage: identical(pairingMessage, _unchanged)
          ? this.pairingMessage
          : pairingMessage as String?,
      notice: identical(notice, _unchanged) ? this.notice : notice as String?,
      transfers: transfers ?? this.transfers,
    );
  }
}
