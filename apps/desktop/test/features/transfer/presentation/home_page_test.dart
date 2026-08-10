import 'dart:async';
import 'dart:ui' show SemanticsFlag;

import 'package:flutter/material.dart';
import 'package:flutter/semantics.dart' show SemanticsNode;
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:xnn_transfer/app/xnn_transfer_app.dart';
import 'package:xnn_transfer/features/transfer/domain/transfer_gateway.dart';
import 'package:xnn_transfer/features/transfer/domain/transfer_models.dart';
import 'package:xnn_transfer/features/transfer/domain/vertical_slice_gateway.dart';
import 'package:xnn_transfer/features/transfer/domain/vertical_slice_models.dart';

import '../support/fake_vertical_slice_gateway.dart';

const NearbyPeer _peer = NearbyPeer(
  id: 'peer-1',
  displayName: 'Nearby laptop',
  trust: PeerTrust.untrusted,
  isAvailable: true,
);

const IncomingTransferOffer _offer = IncomingTransferOffer(
  id: 'offer-1',
  peerName: 'Authenticated desktop',
  fileCount: 1,
  totalBytes: 100,
);

PairingGatewayAttempt _pairing({
  String id = 'attempt-1',
  PairingGatewayState state = PairingGatewayState.awaitingConfirmation,
  PairingGatewayError error = PairingGatewayError.none,
}) {
  return PairingGatewayAttempt(
    id: id,
    peerId: _peer.id,
    state: state,
    sasWords: state == PairingGatewayState.awaitingConfirmation
        ? const <String>['abandon', 'ability', 'able', 'about', 'above']
        : const <String>[],
    error: error,
  );
}

TransferEntry _transfer({
  required String id,
  TransferDirection direction = TransferDirection.outgoing,
  int transferredBytes = 0,
  TransferStatus status = TransferStatus.queued,
}) {
  return TransferEntry(
    id: id,
    direction: direction,
    peerName: _peer.displayName,
    fileCount: 1,
    totalBytes: 100,
    transferredBytes: transferredBytes,
    status: status,
  );
}

Future<void> _pumpApp(
  WidgetTester tester,
  FakeVerticalSliceGateway gateway,
) async {
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = const Size(1200, 1400);
  addTearDown(tester.view.resetDevicePixelRatio);
  addTearDown(tester.view.resetPhysicalSize);
  await tester.pumpWidget(
    XnnTransferApp(verticalSliceGatewayFactory: () => gateway),
  );
  await tester.pump();
}

void main() {
  testWidgets('removes an expired untrusted peer', (WidgetTester tester) async {
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway();
    await _pumpApp(tester, gateway);

    gateway.emitVerticalSlice(const NearbyPeerUpserted(_peer));
    await tester.pump();
    expect(find.text(_peer.displayName), findsOneWidget);
    expect(find.text('Untrusted discovery'), findsOneWidget);

    gateway.emitVerticalSlice(const NearbyPeerExpired('peer-1'));
    await tester.pump();
    expect(find.text(_peer.displayName), findsNothing);
    expect(find.text('No nearby devices are visible.'), findsOneWidget);
  });

  testWidgets('supports keyboard pairing and explicit SAS confirmation',
      (WidgetTester tester) async {
    final SemanticsHandle semantics = tester.ensureSemantics();
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway();
    await _pumpApp(tester, gateway);
    gateway.emitVerticalSlice(const NearbyPeerUpserted(_peer));
    await tester.pump();

    await tester.sendKeyEvent(LogicalKeyboardKey.tab);
    await tester.pump();
    expect(FocusManager.instance.primaryFocus, isNotNull);
    await tester.sendKeyEvent(LogicalKeyboardKey.enter);
    await tester.pump();
    expect(gateway.pairedPeers, <String>[_peer.id]);

    gateway.emitVerticalSlice(PairingAttemptUpdated(_pairing()));
    await tester.pump();
    expect(find.text('Compare security words'), findsOneWidget);
    expect(find.text('abandon'), findsOneWidget);
    expect(find.text('above'), findsOneWidget);

    final SemanticsNode confirmSemantics = tester.getSemantics(
      find.byKey(const ValueKey<String>('confirm-pairing')),
    );
    expect(confirmSemantics.hasFlag(SemanticsFlag.isButton), isTrue);
    await tester.tap(
      find.byKey(const ValueKey<String>('confirm-pairing')),
    );
    await tester.pump();
    expect(gateway.confirmedAttempts, <String>['attempt-1']);

    gateway.emitVerticalSlice(
      PairingAttemptUpdated(
        _pairing(state: PairingGatewayState.paired),
      ),
    );
    await tester.pump();
    expect(
      find.text('Pairing completed. Waiting for trust activation...'),
      findsOneWidget,
    );
    expect(find.text('Untrusted discovery'), findsOneWidget);

    gateway.emitVerticalSlice(
      const PeerTrustUpdated(peerId: 'peer-1', isActive: true),
    );
    await tester.pump();

    expect(find.text('Authenticated peer'), findsOneWidget);
    expect(find.text('Send one file'), findsOneWidget);
    semantics.dispose();
  });

  testWidgets('disables pairing decisions while one is being applied',
      (WidgetTester tester) async {
    final Completer<void> rejection = Completer<void>();
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway()
      ..rejectionCompleter = rejection;
    await _pumpApp(tester, gateway);
    gateway.emitVerticalSlice(const NearbyPeerUpserted(_peer));
    await tester.pump();
    await tester.tap(find.byKey(const ValueKey<String>('pair-peer-1')));
    gateway.emitVerticalSlice(PairingAttemptUpdated(_pairing()));
    await tester.pump();

    await tester.tap(
      find.byKey(const ValueKey<String>('reject-pairing')),
    );
    await tester.pump();

    final OutlinedButton reject = tester.widget<OutlinedButton>(
      find.byKey(const ValueKey<String>('reject-pairing')),
    );
    final FilledButton confirm = tester.widget<FilledButton>(
      find.byKey(const ValueKey<String>('confirm-pairing')),
    );
    expect(reject.onPressed, isNull);
    expect(confirm.onPressed, isNull);
    expect(find.text('Applying pairing decision...'), findsOneWidget);

    rejection.complete();
    await tester.pump();
    expect(gateway.rejectedAttempts, <String>['attempt-1']);
  });

  testWidgets('shows pairing rejection and failure as distinct public results',
      (WidgetTester tester) async {
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway();
    await _pumpApp(tester, gateway);
    gateway.emitVerticalSlice(const NearbyPeerUpserted(_peer));
    await tester.pump();

    await tester.tap(find.byKey(const ValueKey<String>('pair-peer-1')));
    gateway.emitVerticalSlice(PairingAttemptUpdated(_pairing()));
    gateway.emitVerticalSlice(
      PairingAttemptUpdated(
        _pairing(
          state: PairingGatewayState.closed,
          error: PairingGatewayError.rejected,
        ),
      ),
    );
    await tester.pump();
    expect(find.text('Pairing was rejected.'), findsOneWidget);

    await tester.tap(find.byKey(const ValueKey<String>('pair-peer-1')));
    gateway.emitVerticalSlice(
      PairingAttemptUpdated(_pairing(id: 'attempt-2')),
    );
    gateway.emitVerticalSlice(
      PairingAttemptUpdated(
        _pairing(
          id: 'attempt-2',
          state: PairingGatewayState.closed,
          error: PairingGatewayError.failed,
        ),
      ),
    );
    await tester.pump();
    expect(find.text('Pairing failed.'), findsOneWidget);
  });

  testWidgets('selects one file only for an authenticated visible peer',
      (WidgetTester tester) async {
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway();
    await _pumpApp(tester, gateway);
    gateway.emitVerticalSlice(const NearbyPeerUpserted(_peer));
    gateway.emitVerticalSlice(
      const PeerTrustUpdated(peerId: 'peer-1', isActive: true),
    );
    await tester.pump();

    await tester.tap(find.byKey(const ValueKey<String>('send-peer-1')));
    await tester.pump();

    expect(gateway.selectedPeers, <String>[_peer.id]);
    expect(
      find.text('The selected file was offered to ${_peer.displayName}.'),
      findsOneWidget,
    );
  });

  testWidgets('requires an explicit decision for each incoming offer',
      (WidgetTester tester) async {
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway()
      ..acceptedTransfer = const TransferEntry(
        id: 'transfer-1',
        direction: TransferDirection.incoming,
        peerName: 'Authenticated desktop',
        fileCount: 1,
        totalBytes: 100,
        transferredBytes: 0,
        status: TransferStatus.queued,
      );
    await _pumpApp(tester, gateway);
    gateway.emitTransfer(const IncomingOfferReceived(_offer));
    gateway.emitTransfer(
      const IncomingOfferReceived(
        IncomingTransferOffer(
          id: 'offer-2',
          peerName: 'Authenticated workstation',
          fileCount: 1,
          totalBytes: 200,
        ),
      ),
    );
    await tester.pump();

    expect(find.text('Incoming file offer'), findsNWidgets(2));
    await tester.tap(
      find.byKey(const ValueKey<String>('accept-offer-offer-1')),
    );
    await tester.pump();
    await tester.tap(
      find.byKey(const ValueKey<String>('reject-offer-offer-2')),
    );
    await tester.pump();

    expect(gateway.acceptedOffers, <String>['offer-1']);
    expect(gateway.rejectedOffers, <String>['offer-2']);
    expect(find.text('Incoming file offer'), findsNothing);
  });

  testWidgets('labels a running incoming transfer as receiving',
      (WidgetTester tester) async {
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway();
    await _pumpApp(tester, gateway);

    gateway.emitTransfer(
      TransferUpdated(
        _transfer(
          id: 'incoming-1',
          direction: TransferDirection.incoming,
        ),
      ),
    );
    gateway.emitTransfer(
      TransferUpdated(
        _transfer(
          id: 'incoming-1',
          direction: TransferDirection.incoming,
          transferredBytes: 40,
          status: TransferStatus.running,
        ),
      ),
    );
    await tester.pump();

    expect(find.text('Receiving • 40 B / 100 B'), findsOneWidget);
    expect(find.text('Sending • 40 B / 100 B'), findsNothing);
  });

  testWidgets('renders progress, cancellation, and completion',
      (WidgetTester tester) async {
    final FakeVerticalSliceGateway gateway = FakeVerticalSliceGateway();
    await _pumpApp(tester, gateway);

    gateway.emitTransfer(TransferUpdated(_transfer(id: 'transfer-1')));
    gateway.emitTransfer(
      TransferUpdated(
        _transfer(
          id: 'transfer-1',
          transferredBytes: 50,
          status: TransferStatus.running,
        ),
      ),
    );
    await tester.pump();
    expect(find.text('Sending • 50 B / 100 B'), findsOneWidget);

    await tester.tap(
      find.byKey(const ValueKey<String>('cancel-transfer-1')),
    );
    await tester.pump();
    expect(gateway.cancelledTransfers, <String>['transfer-1']);
    expect(find.text('Cancelled • 50 B / 100 B'), findsOneWidget);

    gateway.emitTransfer(TransferUpdated(_transfer(id: 'transfer-2')));
    gateway.emitTransfer(
      TransferUpdated(
        _transfer(
          id: 'transfer-2',
          transferredBytes: 20,
          status: TransferStatus.running,
        ),
      ),
    );
    gateway.emitTransfer(
      TransferUpdated(
        _transfer(
          id: 'transfer-2',
          transferredBytes: 100,
          status: TransferStatus.completed,
        ),
      ),
    );
    await tester.pump();

    expect(find.text('Completed • 100 B / 100 B'), findsOneWidget);
    expect(
      find.byKey(const ValueKey<String>('progress-transfer-2')),
      findsOneWidget,
    );
  });
}
