#include "pch.h"

#include <QtCore/QCoreApplication>

#include "coroutine.h"  // from https://github.com/crazybie/co
#include "qtrpc.h"

using namespace std;
using namespace trpc;
using namespace co;

enum class ErrorCode : int {
  OK,
  SocketError,
  RpcError,
};

struct CoException : std::runtime_error {
  ErrorCode code;

  CoException(const char* msg, ErrorCode _code)
      : std::runtime_error(msg), code(_code) {}
};

//////////////////////////////////////////////////////////////////////////
// promise adapter

template <typename R, typename F>
PromisePtr<R> promised(F f) {
  CoBegin(R);
  {
    __state++;
    f([=](ErrorCode err, R r) {
      if (err != ErrorCode::OK) {
        __onErr(std::make_exception_ptr(CoException("Promised Error", err)));
      } else {
        __onOk(r);
      }
    });
  }
  CoEnd()
}

#define P(R, call) promised<R>([&](auto cb) { call; })

template <typename F>
auto toNetworkCb(F f) {
  return [=](bool ok, QAbstractSocket::SocketError err) {
    if (!ok) {
      f(ErrorCode::SocketError, false);
    } else {
      f(ErrorCode::OK, true);
    }
  };
}

//////////////////////////////////////////////////////////////////////////

class MyRpc : public QtRpcHandler<MyRpc> {
 public:
  MyRpc() : QtRpcHandler("MyRpc") {}

  TRPC(add)
  void add(SessionID sid, int a, int b, RespCb<ErrorCode, int> cb) {
    return cb(ErrorCode::OK, a + b);
  }

  TRPC(multiple)
  void multiple(SessionID sid, float a, float b, RespCb<ErrorCode, float> cb) {
    return cb(ErrorCode::OK, a * b);
  }
};

int pass = 0;

class Test {
  QtRpcServer rpcServer;
  QtRpcClient rpcClient;
  int port = 5555;

 public:
  //////////////////////////////////////////////////////////////////////////
  // call RPC by adapter: complicated

  PromisePtr<bool> run() {
    CoBegin(bool) {
      rpcServer.addHandlers({new MyRpc});

      CoAwait(
          P(bool, rpcServer.startListen("localhost", port, toNetworkCb(cb))));
      puts("server started");

      CoAwait(
          P(bool, rpcClient.connectServer("localhost", port, toNetworkCb(cb))));
      puts("client connected");

      CoAwait(testAdapterCo());
      CoAwait(testNativeCo());
      CoReturn(true);
    }
    CoEnd();
  }

  PromisePtr<bool> testAdapterCo() {
    PromisePtr<int> intPro;
    int result;

    PromisePtr<float> floatPro;
    float floatResult;

    CoBegin(bool) {
      intPro = P(int, rpcClient.call("MyRpc.add", 1, 2, cb));
      CoAwaitData(result, intPro);
      assert(result == 1 + 2);
      pass++;

      floatPro = P(float, rpcClient.call("MyRpc.multiple", 3.0f, 4.0f, cb));
      CoAwaitData(floatResult, floatPro);
      assert(floatResult == 3.0f * 4.0f);
      pass++;

      CoReturn(true);
    }
    CoEnd();
  }

  //////////////////////////////////////////////////////////////////////////
  // call RPC from promise: much simpler

  PromisePtr<bool> testNativeCo() {
    int intResult;
    float floatResult;

    CoBegin(bool) {
      CoAwaitData(intResult, myRpcAdd(1, 2));
      assert(intResult == 1 + 2);
      pass++;

      CoAwaitData(floatResult, myRpcMultiple(3.0f, 4.0f));
      assert(floatResult == 3.0f * 4.0f);
      pass++;

      CoReturn(true);
    }
    CoEnd();
  }

  PromisePtr<int> myRpcAdd(int a, int b) {
    return P(int, rpcClient.call("MyRpc.add", a, b, cb));
  }

  PromisePtr<float> myRpcMultiple(float a, float b) {
    return P(float, rpcClient.call("MyRpc.multiple", a, b, cb));
  }
};

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);

  co::Executor executor;

  Test t;
  auto r = t.run();
  r->onError([](exception& e) {
    printf("Unexpected Exception: %s\n", e.what());
    return;
  });

  while (executor.updateAll())
    app.processEvents();

  printf("pass: %d\n", pass);
}