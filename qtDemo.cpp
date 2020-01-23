#include "pch.h"

#include <QtWidgets/QApplication>
#include "qtrpc.h"

using namespace trpc;

class MyRpc : public QtRpcHandler<MyRpc> {
 public:
  MyRpc() : QtRpcHandler("MyRpc") {}

  TRPC(add) void add(SessionID sid, int a, int b, RespCb<string, int> cb) {
    return cb("OK", a + b);
  }
};

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);

  QtRpcServer server;
  server.addHandlers({new MyRpc});
  server.startListen("localhost", 5555,
                     [](bool ok, QAbstractSocket::SocketError) {

                     });

  QtRpcClient client;
  client.connectServer(
      "localhost", 5555, [&](bool ok, QAbstractSocket::SocketError) {
        if (!ok)
          return;

        client.call("MyRpc.add", 1, 2, [](string msg, int result) {
          assert(msg == "OK");
          assert(result == 1 + 2);
        });
      });

  return app.exec();
}