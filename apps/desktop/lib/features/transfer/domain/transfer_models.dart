enum TransferDirection { incoming, outgoing }

enum TransferStatus { queued, running, paused, cancelled, failed, completed }

final class IncomingTransferOffer {
  const IncomingTransferOffer({
    required this.id,
    required this.peerName,
    required this.fileCount,
    required this.totalBytes,
  });

  final String id;
  final String peerName;
  final int fileCount;
  final int totalBytes;
}

final class TransferFailure {
  const TransferFailure({required this.code, required this.message});

  final String code;
  final String message;
}

final class TransferEntry {
  const TransferEntry({
    required this.id,
    required this.direction,
    required this.peerName,
    required this.fileCount,
    required this.totalBytes,
    required this.transferredBytes,
    required this.status,
    this.failure,
  });

  final String id;
  final TransferDirection direction;
  final String peerName;
  final int fileCount;
  final int totalBytes;
  final int transferredBytes;
  final TransferStatus status;
  final TransferFailure? failure;

  TransferEntry copyWith({
    int? transferredBytes,
    TransferStatus? status,
    TransferFailure? failure,
  }) {
    return TransferEntry(
      id: id,
      direction: direction,
      peerName: peerName,
      fileCount: fileCount,
      totalBytes: totalBytes,
      transferredBytes: transferredBytes ?? this.transferredBytes,
      status: status ?? this.status,
      failure: failure,
    );
  }
}
