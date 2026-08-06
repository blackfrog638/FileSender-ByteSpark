import 'package:flutter/material.dart';
import 'package:xnn_transfer/core/native/native_engine_gateway.dart';
import 'package:xnn_transfer/features/transfer/domain/engine_gateway.dart';
import 'package:xnn_transfer/features/transfer/presentation/home_page.dart';

class XnnTransferApp extends StatelessWidget {
  const XnnTransferApp({this.gatewayFactory, super.key});

  final EngineGatewayFactory? gatewayFactory;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'XnnTransfer',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: const Color(0xFF315CFF)),
        useMaterial3: true,
      ),
      home: TransferHomePage(
        gatewayFactory: gatewayFactory ?? NativeEngineGateway.new,
      ),
    );
  }
}
