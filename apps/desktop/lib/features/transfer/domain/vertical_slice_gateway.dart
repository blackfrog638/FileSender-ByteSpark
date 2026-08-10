import 'dart:async';

import 'package:xnn_transfer/features/transfer/domain/transfer_gateway.dart';
import 'package:xnn_transfer/features/transfer/domain/vertical_slice_models.dart';

sealed class VerticalSliceGatewayEvent {
  const VerticalSliceGatewayEvent();
}

final class NearbyPeerUpserted extends VerticalSliceGatewayEvent {
  const NearbyPeerUpserted(this.peer);

  final NearbyPeer peer;
}

final class NearbyPeerExpired extends VerticalSliceGatewayEvent {
  const NearbyPeerExpired(this.peerId);

  final String peerId;
}

final class PairingAttemptUpdated extends VerticalSliceGatewayEvent {
  const PairingAttemptUpdated(this.attempt);

  final PairingGatewayAttempt attempt;
}

final class PairingAttemptCleared extends VerticalSliceGatewayEvent {
  const PairingAttemptCleared(this.attemptId);

  final String attemptId;
}

final class PeerTrustUpdated extends VerticalSliceGatewayEvent {
  const PeerTrustUpdated({required this.peerId, required this.isActive});

  final String peerId;
  final bool isActive;
}

final class VerticalSliceGatewayNotice extends VerticalSliceGatewayEvent {
  const VerticalSliceGatewayNotice(this.message);

  final String message;
}

abstract interface class VerticalSliceGateway implements TransferGateway {
  Stream<VerticalSliceGatewayEvent> get verticalSliceEvents;

  Future<void> startPairing(String peerId);

  Future<void> confirmPairing(String attemptId);

  Future<void> rejectPairing(String attemptId);

  Future<SendSelectionOutcome> selectAndSendFile(String peerId);
}

typedef VerticalSliceGatewayFactory = VerticalSliceGateway Function();
