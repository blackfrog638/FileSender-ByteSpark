import 'dart:async';

import 'package:flutter/material.dart';
import 'package:xnn_transfer/features/transfer/application/engine_lifecycle_controller.dart';
import 'package:xnn_transfer/features/transfer/application/vertical_slice_controller.dart';
import 'package:xnn_transfer/features/transfer/application/vertical_slice_state.dart';
import 'package:xnn_transfer/features/transfer/domain/engine_gateway.dart';
import 'package:xnn_transfer/features/transfer/domain/transfer_models.dart';
import 'package:xnn_transfer/features/transfer/domain/vertical_slice_gateway.dart';
import 'package:xnn_transfer/features/transfer/domain/vertical_slice_models.dart';

class TransferHomePage extends StatefulWidget {
  const TransferHomePage({required this.gatewayFactory, super.key});

  final VerticalSliceGatewayFactory gatewayFactory;

  @override
  State<TransferHomePage> createState() => _TransferHomePageState();
}

class _TransferHomePageState extends State<TransferHomePage> {
  late final VerticalSliceController _controller;

  @override
  void initState() {
    super.initState();
    _controller = VerticalSliceController(widget.gatewayFactory());
    WidgetsBinding.instance.addPostFrameCallback((Duration _) {
      if (mounted) {
        unawaited(_controller.initialize());
      }
    });
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('XnnTransfer')),
      body: SafeArea(
        child: AnimatedBuilder(
          animation: _controller,
          builder: (BuildContext context, Widget? child) {
            return switch (_controller.state) {
              VerticalSliceInitializing() => Center(
                  child: Semantics(
                    label: 'Starting secure peer services',
                    child: const CircularProgressIndicator(),
                  ),
                ),
              final VerticalSliceUnavailable state => _UnavailableView(state),
              final VerticalSliceReady state => _ReadyView(
                  state: state,
                  controller: _controller,
                ),
            };
          },
        ),
      ),
    );
  }
}

class _UnavailableView extends StatelessWidget {
  const _UnavailableView(this.state);

  final VerticalSliceUnavailable state;

  @override
  Widget build(BuildContext context) {
    return Center(
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 520),
        child: Padding(
          padding: const EdgeInsets.all(32),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              Icon(
                Icons.portable_wifi_off,
                size: 56,
                color: Theme.of(context).colorScheme.error,
              ),
              const SizedBox(height: 16),
              Text(
                'Secure transfer is unavailable',
                style: Theme.of(context).textTheme.headlineSmall,
                textAlign: TextAlign.center,
              ),
              const SizedBox(height: 8),
              Text(state.reason, textAlign: TextAlign.center),
            ],
          ),
        ),
      ),
    );
  }
}

class _ReadyView extends StatelessWidget {
  const _ReadyView({required this.state, required this.controller});

  final VerticalSliceReady state;
  final VerticalSliceController controller;

  @override
  Widget build(BuildContext context) {
    return FocusTraversalGroup(
      child: SingleChildScrollView(
        padding: const EdgeInsets.all(24),
        child: Center(
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 920),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: <Widget>[
                Text(
                  'Nearby transfer',
                  style: Theme.of(context).textTheme.headlineMedium,
                ),
                const SizedBox(height: 4),
                const Text(
                  'Discovery is untrusted until both devices confirm the same '
                  'security words.',
                ),
                if (state.notice case final String notice) ...<Widget>[
                  const SizedBox(height: 16),
                  _StatusBanner(icon: Icons.info_outline, message: notice),
                ],
                if (state.pairingMessage case final String message) ...<Widget>[
                  const SizedBox(height: 16),
                  _StatusBanner(icon: Icons.security, message: message),
                ],
                if (state.pairing
                    case final PairingCeremony pairing) ...<Widget>[
                  const SizedBox(height: 16),
                  _PairingCard(
                    pairing: pairing,
                    peerName: state.peerById(pairing.peerId)?.displayName ??
                        'Nearby device',
                    onConfirm: controller.confirmPairing,
                    onReject: controller.rejectPairing,
                  ),
                ],
                const SizedBox(height: 28),
                _SectionHeading(title: 'Devices', count: state.peers.length),
                const SizedBox(height: 8),
                if (state.peers.isEmpty)
                  const _EmptyCard(
                    icon: Icons.radar,
                    message: 'No nearby devices are visible.',
                  )
                else
                  for (final NearbyPeer peer in state.peers) ...<Widget>[
                    _PeerCard(
                      peer: peer,
                      onPair: () => controller.pairPeer(peer.id),
                      onSend: () => controller.selectAndSendFile(peer.id),
                    ),
                    const SizedBox(height: 8),
                  ],
                const SizedBox(height: 20),
                _SectionHeading(
                  title: 'Incoming',
                  count: state.transfers.incomingOffers.length,
                ),
                const SizedBox(height: 8),
                if (state.transfers.incomingOffers.isEmpty)
                  const _EmptyCard(
                    icon: Icons.move_to_inbox_outlined,
                    message: 'No incoming file offers.',
                  )
                else
                  for (final IncomingTransferOffer offer
                      in state.transfers.incomingOffers) ...<Widget>[
                    _IncomingOfferCard(
                      offer: offer,
                      onAccept: () => controller.acceptOffer(offer.id),
                      onReject: () => controller.rejectOffer(offer.id),
                    ),
                    const SizedBox(height: 8),
                  ],
                const SizedBox(height: 20),
                _SectionHeading(
                  title: 'Transfers',
                  count: state.transfers.transfers.length,
                ),
                const SizedBox(height: 8),
                if (state.transfers.transfers.isEmpty)
                  const _EmptyCard(
                    icon: Icons.swap_vert,
                    message: 'No file transfers yet.',
                  )
                else
                  for (final TransferEntry transfer
                      in state.transfers.transfers) ...<Widget>[
                    _TransferCard(
                      transfer: transfer,
                      onCancel: () => controller.cancelTransfer(transfer.id),
                    ),
                    const SizedBox(height: 8),
                  ],
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _SectionHeading extends StatelessWidget {
  const _SectionHeading({required this.title, required this.count});

  final String title;
  final int count;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: <Widget>[
        Expanded(
          child: Text(title, style: Theme.of(context).textTheme.titleLarge),
        ),
        Text('$count'),
      ],
    );
  }
}

class _StatusBanner extends StatelessWidget {
  const _StatusBanner({required this.icon, required this.message});

  final IconData icon;
  final String message;

  @override
  Widget build(BuildContext context) {
    return Semantics(
      liveRegion: true,
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: Theme.of(context).colorScheme.surfaceContainerHighest,
          borderRadius: BorderRadius.circular(12),
        ),
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Row(
            children: <Widget>[
              Icon(icon),
              const SizedBox(width: 12),
              Expanded(child: Text(message)),
            ],
          ),
        ),
      ),
    );
  }
}

class _EmptyCard extends StatelessWidget {
  const _EmptyCard({required this.icon, required this.message});

  final IconData icon;
  final String message;

  @override
  Widget build(BuildContext context) {
    return Card.outlined(
      child: Padding(
        padding: const EdgeInsets.all(20),
        child: Row(
          children: <Widget>[
            Icon(icon),
            const SizedBox(width: 12),
            Expanded(child: Text(message)),
          ],
        ),
      ),
    );
  }
}

class _PeerCard extends StatelessWidget {
  const _PeerCard({
    required this.peer,
    required this.onPair,
    required this.onSend,
  });

  final NearbyPeer peer;
  final Future<VerticalSliceCommandOutcome> Function() onPair;
  final Future<VerticalSliceCommandOutcome> Function() onSend;

  @override
  Widget build(BuildContext context) {
    final bool authenticated = peer.trust == PeerTrust.authenticated;
    final String status = authenticated
        ? peer.isAvailable
            ? 'Authenticated peer'
            : 'Authenticated peer, offline'
        : 'Untrusted discovery';
    return Semantics(
      container: true,
      label: '${peer.displayName}, $status',
      child: Card.outlined(
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: Row(
            children: <Widget>[
              Icon(authenticated ? Icons.verified_user : Icons.devices),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: <Widget>[
                    Text(
                      peer.displayName,
                      style: Theme.of(context).textTheme.titleMedium,
                    ),
                    const SizedBox(height: 2),
                    Text(status),
                  ],
                ),
              ),
              const SizedBox(width: 12),
              if (!authenticated)
                FilledButton.tonal(
                  key: ValueKey<String>('pair-${peer.id}'),
                  onPressed:
                      peer.isAvailable ? () => unawaited(onPair()) : null,
                  child: const Text('Pair'),
                )
              else
                FilledButton(
                  key: ValueKey<String>('send-${peer.id}'),
                  onPressed:
                      peer.isAvailable ? () => unawaited(onSend()) : null,
                  child: const Text('Send one file'),
                ),
            ],
          ),
        ),
      ),
    );
  }
}

class _PairingCard extends StatefulWidget {
  const _PairingCard({
    required this.pairing,
    required this.peerName,
    required this.onConfirm,
    required this.onReject,
  });

  final PairingCeremony pairing;
  final String peerName;
  final Future<VerticalSliceCommandOutcome> Function() onConfirm;
  final Future<VerticalSliceCommandOutcome> Function() onReject;

  @override
  State<_PairingCard> createState() => _PairingCardState();
}

class _PairingCardState extends State<_PairingCard> {
  bool _decisionPending = false;

  @override
  void didUpdateWidget(_PairingCard oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.pairing.attemptId != widget.pairing.attemptId) {
      _decisionPending = false;
    }
  }

  Future<void> _decide(
    Future<VerticalSliceCommandOutcome> Function() command,
  ) async {
    if (_decisionPending) {
      return;
    }
    final String attemptId = widget.pairing.attemptId;
    setState(() {
      _decisionPending = true;
    });
    try {
      await command();
    } finally {
      if (mounted && widget.pairing.attemptId == attemptId) {
        setState(() {
          _decisionPending = false;
        });
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    final String words = widget.pairing.sasWords.join(' ');
    return Semantics(
      container: true,
      label: 'Pairing security words for ${widget.peerName}: $words',
      child: Card(
        color: Theme.of(context).colorScheme.primaryContainer,
        child: Padding(
          padding: const EdgeInsets.all(20),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: <Widget>[
              Text(
                'Compare security words',
                style: Theme.of(context).textTheme.titleLarge,
              ),
              const SizedBox(height: 4),
              Text(
                'Confirm only if ${widget.peerName} shows these exact words.',
              ),
              const SizedBox(height: 16),
              Wrap(
                spacing: 8,
                runSpacing: 8,
                children: <Widget>[
                  for (final String word in widget.pairing.sasWords)
                    Chip(label: Text(word)),
                ],
              ),
              if (_decisionPending) ...<Widget>[
                const SizedBox(height: 12),
                Semantics(
                  liveRegion: true,
                  child: const Text('Applying pairing decision...'),
                ),
              ],
              const SizedBox(height: 16),
              Wrap(
                alignment: WrapAlignment.end,
                spacing: 8,
                runSpacing: 8,
                children: <Widget>[
                  OutlinedButton(
                    key: const ValueKey<String>('reject-pairing'),
                    onPressed: _decisionPending
                        ? null
                        : () => unawaited(_decide(widget.onReject)),
                    child: const Text('Reject'),
                  ),
                  FilledButton(
                    key: const ValueKey<String>('confirm-pairing'),
                    onPressed: _decisionPending
                        ? null
                        : () => unawaited(_decide(widget.onConfirm)),
                    child: const Text('Codes match'),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _IncomingOfferCard extends StatelessWidget {
  const _IncomingOfferCard({
    required this.offer,
    required this.onAccept,
    required this.onReject,
  });

  final IncomingTransferOffer offer;
  final Future<Object?> Function() onAccept;
  final Future<Object?> Function() onReject;

  @override
  Widget build(BuildContext context) {
    return Semantics(
      container: true,
      label:
          'Incoming file offer from ${offer.peerName}, ${_formatBytes(offer.totalBytes)}',
      child: Card.outlined(
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: Row(
            children: <Widget>[
              const Icon(Icons.description_outlined),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: <Widget>[
                    Text(
                      'Incoming file offer',
                      style: Theme.of(context).textTheme.titleMedium,
                    ),
                    Text(
                      '${offer.peerName} • ${_formatBytes(offer.totalBytes)}',
                    ),
                  ],
                ),
              ),
              const SizedBox(width: 12),
              OutlinedButton(
                key: ValueKey<String>('reject-offer-${offer.id}'),
                onPressed: () => unawaited(onReject()),
                child: const Text('Reject'),
              ),
              const SizedBox(width: 8),
              FilledButton(
                key: ValueKey<String>('accept-offer-${offer.id}'),
                onPressed: () => unawaited(onAccept()),
                child: const Text('Accept'),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _TransferCard extends StatelessWidget {
  const _TransferCard({required this.transfer, required this.onCancel});

  final TransferEntry transfer;
  final Future<Object?> Function() onCancel;

  @override
  Widget build(BuildContext context) {
    final double? progress = transfer.totalBytes == 0
        ? transfer.status == TransferStatus.completed
            ? 1
            : null
        : transfer.transferredBytes / transfer.totalBytes;
    final bool canCancel = transfer.status == TransferStatus.queued ||
        transfer.status == TransferStatus.running ||
        transfer.status == TransferStatus.paused;
    final String status = _statusLabel(transfer);
    return Semantics(
      container: true,
      label:
          '$status transfer with ${transfer.peerName}, ${_formatBytes(transfer.transferredBytes)} of ${_formatBytes(transfer.totalBytes)}',
      child: Card.outlined(
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: <Widget>[
              Row(
                children: <Widget>[
                  Icon(
                    transfer.direction == TransferDirection.outgoing
                        ? Icons.upload_file
                        : Icons.download,
                  ),
                  const SizedBox(width: 12),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: <Widget>[
                        Text(
                          transfer.peerName,
                          style: Theme.of(context).textTheme.titleMedium,
                        ),
                        Text(
                          '$status • '
                          '${_formatBytes(transfer.transferredBytes)} / '
                          '${_formatBytes(transfer.totalBytes)}',
                        ),
                      ],
                    ),
                  ),
                  if (canCancel)
                    TextButton(
                      key: ValueKey<String>('cancel-${transfer.id}'),
                      onPressed: () => unawaited(onCancel()),
                      child: const Text('Cancel'),
                    ),
                ],
              ),
              const SizedBox(height: 12),
              LinearProgressIndicator(
                key: ValueKey<String>('progress-${transfer.id}'),
                value: progress,
                semanticsLabel: '$status transfer progress',
                semanticsValue: progress == null
                    ? null
                    : '${(progress * 100).round()} percent',
              ),
              if (transfer.failure
                  case final TransferFailure failure) ...<Widget>[
                const SizedBox(height: 8),
                Text(
                  failure.message,
                  style: TextStyle(color: Theme.of(context).colorScheme.error),
                ),
              ],
            ],
          ),
        ),
      ),
    );
  }
}

String _statusLabel(TransferEntry transfer) {
  return switch (transfer.status) {
    TransferStatus.queued => 'Queued',
    TransferStatus.running => transfer.direction == TransferDirection.incoming
        ? 'Receiving'
        : 'Sending',
    TransferStatus.paused => 'Paused',
    TransferStatus.cancelled => 'Cancelled',
    TransferStatus.failed => 'Failed',
    TransferStatus.completed => 'Completed',
  };
}

String _formatBytes(int bytes) {
  if (bytes < 1024) {
    return '$bytes B';
  }
  final double kibibytes = bytes / 1024;
  if (kibibytes < 1024) {
    return '${kibibytes.toStringAsFixed(kibibytes < 10 ? 1 : 0)} KiB';
  }
  final double mebibytes = kibibytes / 1024;
  return '${mebibytes.toStringAsFixed(mebibytes < 10 ? 1 : 0)} MiB';
}

class EngineLifecycleHarnessPage extends StatefulWidget {
  const EngineLifecycleHarnessPage({required this.gatewayFactory, super.key});

  final EngineGatewayFactory gatewayFactory;

  @override
  State<EngineLifecycleHarnessPage> createState() =>
      _EngineLifecycleHarnessPageState();
}

class _EngineLifecycleHarnessPageState
    extends State<EngineLifecycleHarnessPage> {
  late final EngineLifecycleController _controller;

  @override
  void initState() {
    super.initState();
    _controller = EngineLifecycleController(widget.gatewayFactory());
    WidgetsBinding.instance.addPostFrameCallback((Duration _) {
      if (mounted) {
        _controller.initialize();
      }
    });
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('XnnTransfer')),
      body: Center(
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 560),
          child: Padding(
            padding: const EdgeInsets.all(32),
            child: AnimatedBuilder(
              animation: _controller,
              builder: (BuildContext context, Widget? child) {
                return Column(
                  mainAxisAlignment: MainAxisAlignment.center,
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: <Widget>[
                    Text(
                      _phaseLabel(_controller.phase),
                      textAlign: TextAlign.center,
                    ),
                    if (_controller.error case final String error) ...<Widget>[
                      const SizedBox(height: 12),
                      Text(error, textAlign: TextAlign.center),
                    ],
                    const SizedBox(height: 24),
                    FilledButton(
                      onPressed: _controller.phase == EnginePhase.ready
                          ? _controller.start
                          : null,
                      child: const Text('Start native engine'),
                    ),
                    const SizedBox(height: 12),
                    const Text(
                      'Peer discovery and file transfer are not implemented '
                      'in this scaffold.',
                      textAlign: TextAlign.center,
                    ),
                  ],
                );
              },
            ),
          ),
        ),
      ),
    );
  }

  String _phaseLabel(EnginePhase phase) {
    return switch (phase) {
      EnginePhase.initializing => 'Checking native engine...',
      EnginePhase.ready => 'Native engine is ready.',
      EnginePhase.running => 'Native engine is running.',
      EnginePhase.stopped => 'Native engine is stopped.',
      EnginePhase.unavailable => 'Native engine is unavailable.',
    };
  }
}
