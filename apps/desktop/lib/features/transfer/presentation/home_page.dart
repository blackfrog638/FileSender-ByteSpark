import 'package:flutter/material.dart';
import 'package:xnn_transfer/features/transfer/application/transfer_controller.dart';
import 'package:xnn_transfer/features/transfer/domain/engine_gateway.dart';

class TransferHomePage extends StatefulWidget {
  const TransferHomePage({
    required this.gatewayFactory,
    super.key,
  });

  final EngineGatewayFactory gatewayFactory;

  @override
  State<TransferHomePage> createState() => _TransferHomePageState();
}

class _TransferHomePageState extends State<TransferHomePage> {
  late final TransferController _controller;

  @override
  void initState() {
    super.initState();
    _controller = TransferController(widget.gatewayFactory());
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
                    Icon(
                      Icons.devices,
                      size: 64,
                      color: Theme.of(context).colorScheme.primary,
                    ),
                    const SizedBox(height: 24),
                    Text(
                      'Desktop engineering harness',
                      textAlign: TextAlign.center,
                      style: Theme.of(context).textTheme.headlineSmall,
                    ),
                    const SizedBox(height: 12),
                    Text(
                      _phaseLabel(_controller.phase),
                      textAlign: TextAlign.center,
                    ),
                    if (_controller.error case final String error) ...<Widget>[
                      const SizedBox(height: 12),
                      Text(
                        error,
                        textAlign: TextAlign.center,
                        style: TextStyle(
                          color: Theme.of(context).colorScheme.error,
                        ),
                      ),
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
