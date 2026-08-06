import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:xnn_transfer/app/xnn_transfer_app.dart';
import 'package:xnn_transfer/features/transfer/domain/engine_gateway.dart';

class _FakeEngineGateway implements EngineGateway {
  bool initialized = false;
  bool running = false;

  @override
  void initialize() {
    initialized = true;
  }

  @override
  void start() {
    if (!initialized) {
      throw StateError('Engine was not initialized');
    }
    running = true;
  }

  @override
  void stop() {
    running = false;
  }

  @override
  void dispose() {
    running = false;
  }
}

void main() {
  testWidgets('shows the native lifecycle without claiming transfer support',
      (WidgetTester tester) async {
    final _FakeEngineGateway gateway = _FakeEngineGateway();

    await tester.pumpWidget(
      XnnTransferApp(gatewayFactory: () => gateway),
    );
    await tester.pump();

    expect(find.text('XnnTransfer'), findsOneWidget);
    expect(find.text('Native engine is ready.'), findsOneWidget);
    expect(
      find.text(
        'Peer discovery and file transfer are not implemented in this scaffold.',
      ),
      findsOneWidget,
    );

    await tester.tap(find.widgetWithText(FilledButton, 'Start native engine'));
    await tester.pump();

    expect(gateway.running, isTrue);
    expect(find.text('Native engine is running.'), findsOneWidget);
  });
}
