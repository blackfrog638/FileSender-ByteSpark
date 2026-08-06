import 'package:xnn_transfer/features/transfer/domain/transfer_models.dart';

sealed class TransferState {
  const TransferState();
}

final class TransferInitializing extends TransferState {
  const TransferInitializing();
}

final class TransferUnavailable extends TransferState {
  const TransferUnavailable(this.reason);

  final String reason;
}

final class TransferReady extends TransferState {
  TransferReady({
    Iterable<IncomingTransferOffer> incomingOffers =
        const <IncomingTransferOffer>[],
    Iterable<TransferEntry> transfers = const <TransferEntry>[],
  })  : incomingOffers = List<IncomingTransferOffer>.unmodifiable(
          incomingOffers,
        ),
        transfers = List<TransferEntry>.unmodifiable(transfers);

  final List<IncomingTransferOffer> incomingOffers;
  final List<TransferEntry> transfers;

  IncomingTransferOffer? offerById(String offerId) {
    for (final IncomingTransferOffer offer in incomingOffers) {
      if (offer.id == offerId) {
        return offer;
      }
    }
    return null;
  }

  TransferEntry? transferById(String transferId) {
    for (final TransferEntry transfer in transfers) {
      if (transfer.id == transferId) {
        return transfer;
      }
    }
    return null;
  }

  TransferReady copyWith({
    Iterable<IncomingTransferOffer>? incomingOffers,
    Iterable<TransferEntry>? transfers,
  }) {
    return TransferReady(
      incomingOffers: incomingOffers ?? this.incomingOffers,
      transfers: transfers ?? this.transfers,
    );
  }
}
