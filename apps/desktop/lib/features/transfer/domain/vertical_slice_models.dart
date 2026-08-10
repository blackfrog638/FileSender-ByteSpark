enum PeerTrust { untrusted, authenticated }

final class NearbyPeer {
  const NearbyPeer({
    required this.id,
    required this.displayName,
    required this.trust,
    required this.isAvailable,
  });

  final String id;
  final String displayName;
  final PeerTrust trust;
  final bool isAvailable;

  NearbyPeer copyWith({
    String? displayName,
    PeerTrust? trust,
    bool? isAvailable,
  }) {
    return NearbyPeer(
      id: id,
      displayName: displayName ?? this.displayName,
      trust: trust ?? this.trust,
      isAvailable: isAvailable ?? this.isAvailable,
    );
  }
}

enum PairingGatewayState { starting, awaitingConfirmation, paired, closed }

enum PairingGatewayError {
  none,
  rejected,
  cancelled,
  timedOut,
  busy,
  unavailable,
  failed,
}

final class PairingGatewayAttempt {
  PairingGatewayAttempt({
    required this.id,
    required this.peerId,
    required this.state,
    required Iterable<String> sasWords,
    required this.error,
  }) : sasWords = List<String>.unmodifiable(sasWords);

  final String id;
  final String peerId;
  final PairingGatewayState state;
  final List<String> sasWords;
  final PairingGatewayError error;
}

final class PairingCeremony {
  PairingCeremony({
    required this.attemptId,
    required this.peerId,
    required Iterable<String> sasWords,
  }) : sasWords = List<String>.unmodifiable(sasWords);

  final String attemptId;
  final String peerId;
  final List<String> sasWords;
}

enum SendSelectionOutcome { submitted, cancelled }
