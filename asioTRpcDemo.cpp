#include "asioTRpc.h"

#include <assert.h>
#include <chrono>
#include <thread>

using namespace trpc;

class MyHandler : public AsioRpcHandler<MyHandler> {
 public:
  MyHandler() : RpcHandler("MyHandler") {}

  TRPC(foo)
  void foo(SessionID sid, int a, int b, RespCb<int> cb) { cb(a + b); }

  TRPC(bar)
  void bar(SessionID sid, map<int, double> data, RespCb<vector<double>> cb) {
    vector<double> r;
    for (auto [k, v] : data) {
      r.push_back(v);
    }
    cb(r);
  }
};

bool quit = false;

void test(shared_ptr<AsioClient> c, int i, shared_ptr<Action<>> cb) {
  int a = rand(), b = rand();
  c->call("MyHandler.foo", a, b, [=](int r) {
    assert(r == a + b);
    printf("[%2d] %d + %d = %d\n", i, a, b, r);

    map<int, double> d{
        {11, 1.1},
        {22, 2.2},
    };
    c->call("MyHandler.bar", d, [=](vector<double> r) {
      assert(r[0] == 1.1);
      assert(r[1] == 2.2);
      (*cb)();
    });
  });
}

int main() {
  auto s = make_shared<AsioServer>();
  auto c = make_shared<AsioClient>();

  s->addHandlers({new MyHandler});

  int port = 9999;
  int cnt = 2000;
  s->start(port, [=](bool) {
    c->connect("127.0.0.1", port, [=](bool) {
      auto i = make_shared<int>(0);
      auto done = make_shared<Action<>>();
      *done = [=] {
        if (++*i < cnt) {
          test(c, *i, done);
        } else {
          quit = true;
        }
      };
      for (int j = 0; j < 20; j++)
        test(c, *i, done);
    });
  });

  while (!quit) {
    s->update();
    c->update();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  return 0;
}