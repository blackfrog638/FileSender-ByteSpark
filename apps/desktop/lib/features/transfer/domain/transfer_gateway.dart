import 'dart:async';

import 'package:xnn_transfer/features/transfer/domain/transfer_models.dart';

sealed class TransferGatewayEvent {
  const TransferGatewayEvent();
}

final class IncomingOfferReceived extends TransferGatewayEvent {
  const IncomingOfferReceived(this.offer);

  final IncomingTransferOffer offer;
}

final class IncomingOfferWithdrawn extends TransferGatewayEvent {
  const IncomingOfferWithdrawn(this.offerId);

  final String offerId;
}

final class TransferUpdated extends TransferGatewayEvent {
  const TransferUpdated(this.transfer);

  final TransferEntry transfer;
}

final class TransferGatewayUnavailable extends TransferGatewayEvent {
  const TransferGatewayUnavailable(this.reason);

  final String reason;
}

abstract interface class TransferGateway {
  Stream<TransferGatewayEvent> get events;

  Future<void> initialize();

  Future<TransferEntry> acceptOffer(String offerId);

  Future<void> rejectOffer(String offerId);

  Future<void> pauseTransfer(String transferId);

  Future<void> resumeTransfer(String transferId);

  Future<void> cancelTransfer(String transferId);

  void dispose();
}
