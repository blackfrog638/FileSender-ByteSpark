import 'package:flutter/material.dart';
import 'package:xnn_transfer/app/native_vertical_slice_gateway.dart';
import 'package:xnn_transfer/features/transfer/domain/engine_gateway.dart';
import 'package:xnn_transfer/features/transfer/domain/vertical_slice_gateway.dart';
import 'package:xnn_transfer/features/transfer/presentation/home_page.dart';

class XnnTransferApp extends StatelessWidget {
  const XnnTransferApp({
    this.gatewayFactory,
    this.verticalSliceGatewayFactory,
    super.key,
  }) : assert(
          gatewayFactory == null || verticalSliceGatewayFactory == null,
          'Only one gateway factory may be supplied',
        );

  final EngineGatewayFactory? gatewayFactory;
  final VerticalSliceGatewayFactory? verticalSliceGatewayFactory;

  @override
  Widget build(BuildContext context) {
    final EngineGatewayFactory? lifecycleFactory = gatewayFactory;
    final Widget home = lifecycleFactory == null
        ? TransferHomePage(
            gatewayFactory:
                verticalSliceGatewayFactory ?? NativeVerticalSliceGateway.new,
          )
        : EngineLifecycleHarnessPage(gatewayFactory: lifecycleFactory);
    return MaterialApp(
      title: 'XnnTransfer',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: const Color(0xFF315CFF)),
        useMaterial3: true,
      ),
      home: home,
    );
  }
}
