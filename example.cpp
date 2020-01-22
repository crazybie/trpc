#include "pch.h"

#include <assert.h>
#include <iostream>
#include <sstream>

#define TPRC_DELIMITER(n) n << ' '
#include "trpc.h"

using namespace trpc;
int pass = 0;

class MyRpc : public RpcHandler<MyRpc, std::iostream> {
 public:
  MyRpc() : RpcHandler("MyRpc") {}

  TRPC(add)
  void add(SessionID sid, int a, int b, RespCb<string, int> cb) {
    callClient(sid);
    server->notify(sid, "onAdd", "msgFromServer", a, b);
    cb("OK", a + b);
  }

  TRPC(reduceFloat)
  void reduceFloat(SessionID sid, float a, float b, RespCb<string, float> cb) {
    server->notify(sid, "onReduceFloat", "msgFromServer", a, b);
    cb("OK", a - b);
  }

  void callClient(SessionID sid) {
    server->call(sid, "clientFunc", 11, 2, [](string msg, int r) {
      assert(msg == "fromClient");
      assert(r == 11 * 2);
      pass++;
    });
  }
};

int main() {
  std::stringstream serverStream, clientStream;

  RpcServer<std::iostream> server;
  server.addHandlers({new MyRpc});

  int sessionID = 1;
  server.addSession(sessionID, serverStream);

  RpcClient<std::iostream> client(clientStream);
  client.flush = [&] {
    server.onReceive(sessionID, clientStream);
    /// reset to reuse after eof
    clientStream.clear();
  };
  server.flush = [&](SessionID sid) {
    assert(sid == sessionID);
    client.onReceive(serverStream);
    /// reset to reuse after eof
    serverStream.clear();
  };

  client.onCall("clientFunc", [](int a, int b, RespCb<string, int> cb) {
    cb("fromClient", a * b);
  });

  {
    int data_a = 11, data_b = 22;
    client.onNotify("onAdd", [=](string msg, int a, int b) {
      assert(msg == "msgFromServer");
      assert(a == data_a && b == data_b);
      pass++;
    });

    client.call("MyRpc.add", data_a, data_b, [=](string msg, int result) {
      assert(msg == "OK");
      assert(result == data_a + data_b);
      pass++;
    });
  }

  {
    float data_a = 22.22f, data_b = 10.11f;
    client.onNotify("onReduceFloat", [=](string msg, float a, float b) {
      assert(msg == "msgFromServer");
      assert(a == data_a && b == data_b);
      pass++;
    });

    client.call("MyRpc.reduceFloat", data_a, data_b,
                [=](string msg, float result) {
                  assert(msg == "OK");
                  assert(result == data_a - data_b);
                  pass++;
                });
  }

  std::cout << "PASS:" << pass << std::endl;
}
