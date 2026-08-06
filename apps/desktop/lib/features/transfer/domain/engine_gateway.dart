abstract interface class EngineGateway {
  void initialize();

  void start();

  void stop();

  void dispose();
}

typedef EngineGatewayFactory = EngineGateway Function();
