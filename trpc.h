//
// tiny rpc framework
//
// by soniced@sina.com.
// All rights reserved.
//
#pragma once
#include <functional>
#include <map>
#include <string>
#include <tuple>

//#define TPRC_DELIMITER(n)  n << ' '
#ifndef TPRC_DELIMITER
#define TPRC_DELIMITER(n) n
#endif

namespace trpc {

using std::function;
using std::map;
using std::string;
using std::tuple;

namespace imp {
using namespace std;

//////////////////////////////////////////////////////////////////////////
/// tuple helpers

constexpr auto tuple_for = [](auto&& t, auto&& f) {
  return apply([&f](auto&... x) { (..., f(x)); }, t);
};

template <size_t Ofst, class Tuple, size_t... I>
constexpr auto tuple_slice_impl(Tuple&& t, index_sequence<I...>) {
  return forward_as_tuple(get<I + Ofst>(forward<Tuple>(t))...);
}

template <size_t I1, size_t I2, class Cont>
constexpr auto tuple_slice(Cont&& t) {
  static_assert(I2 >= I1, "invalid slice");
  static_assert(tuple_size<decay_t<Cont>>::value >= I2,
                "slice index out of bounds");
  return tuple_slice_impl<I1>(forward<Cont>(t), make_index_sequence<I2 - I1>{});
}

template <typename Idx, typename Tuple>
struct tuple_elems;

template <typename Tuple, size_t... I>
struct tuple_elems<index_sequence<I...>, Tuple> {
  using type = tuple<tuple_element_t<I, Tuple>...>;
};

//////////////////////////////////////////////////////////////////////////
/// function helpers

template <typename T>
struct FuncTrait : FuncTrait<decltype(&T::operator())> {};

template <typename R, typename C, typename... A>
struct FuncTrait<R (C::*)(A...) const> {
  using Args = tuple<A...>;
  using Result = R;
};

template <typename R, typename C, typename... A>
struct FuncTrait<R (C::*)(A...)> {
  using Args = tuple<A...>;
  using Result = R;
};

template <typename T, class = void_t<>>
struct is_lambda : false_type {};

template <typename T>
struct is_lambda<T, void_t<decltype(&T::operator())>> : true_type {};

template <typename T>
constexpr bool is_lambda_v = is_lambda<T>::value;

template <typename Tuple>
struct ArgsTrait {
  static constexpr auto Cnt = tuple_size_v<Tuple>;
  using ArgsNoCb =
      typename tuple_elems<make_index_sequence<Cnt - 1>, Tuple>::type;
  using Cb = tuple_element_t<Cnt - 1, Tuple>;
  using CbArgs = typename FuncTrait<Cb>::Args;
};

//////////////////////////////////////////////////////////////////////////
template <typename E,
          typename S,
          typename = std::enable_if_t<std::is_enum_v<E>>>
S& operator>>(S& s, E& e) {
  return s >> (std::underlying_type_t<E>&)(e);
}

template <typename E,
          typename S,
          typename = std::enable_if_t<std::is_enum_v<E>>>
S& operator<<(S& s, E e) {
  return s << (std::underlying_type_t<E>)(e);
}

}  // namespace imp

using SessionID = int;
using SessionCb = function<void(SessionID)>;

enum class RequestType : int {
  Notify = 1,
  Call,
  CallResponse,
  UserRequest,
};

template <typename... A>
using RespCb = function<void(A...)>;

template <typename istream, typename ostream>
class RpcServer;

template <typename istream, typename ostream = istream>
class Handler {
 public:
  using Func = function<void(SessionID, int, istream&, ostream&)>;
  using Server = RpcServer<istream, ostream>;

  string name;

  Handler(string n) : name(n) {}
  virtual ~Handler() {}

  virtual void init() {}
  virtual void onDisconnected(SessionID sid) {}

  void onRequest(SessionID sid, string name, int rid, istream& i, ostream& o) {
    funcs[name](sid, rid, i, o);
  }

  template <typename C, typename R, typename... A>
  void addFunction(string name, R (C::*mf)(A...)) {
    addFunction(name, [this, mf](A... a) {
      return (static_cast<C*>(this)->*mf)(a...);
    });
  }

  template <typename Func>
  void addFunction(string name, Func&& f) {
    using namespace imp;
    using Args = typename FuncTrait<Func>::Args;
    using F = ArgsTrait<Args>;

    static_assert(is_same_v<tuple_element_t<0, Args>, SessionID>,
                  "first param should be a SessionID");
    static_assert(is_lambda_v<tuple_element_t<tuple_size_v<Args> - 1, Args>>,
                  "last param should be a lambda");

    funcs[name] = [=](SessionID sid, int reqID, istream& i, ostream& o) {
      typename F::ArgsNoCb args;

      get<0>(args) = sid;
      tuple_for(tuple_slice<1, F::Cnt - 1>(args), [&](auto& e) { i >> e; });

      auto&& cb = [=, &o](auto... a) {
        o << TPRC_DELIMITER(reqID);
        (..., (o << TPRC_DELIMITER(a)));
        server->flush(sid);
      };
      apply(f, tuple_cat(args, make_tuple(cb)));
    };
  }

  void setServer(Server* s) { server = s; }

 protected:
  Server* server;

 private:
  map<string, Func> funcs;
};

template <typename istream, typename ostream = istream>
class RpcServer {
 public:
  using Handler = Handler<istream, ostream>;

  SessionCb flush;
  SessionCb disconnected;

  virtual ~RpcServer() {
    for (auto i : handlers) {
      delete i.second;
    }
  }
  void addHandlers(std::initializer_list<Handler*> h) {
    for (auto i : h) {
      handlers[i->name] = i;
      i->setServer(this);
    }
    for (auto i : handlers) {
      i.second->init();
    }
  }
  void addSession(SessionID sid, ostream& o) {
    auto& s = sessions[sid];
    s.sid = sid;
    s.output = &o;
  }

  void removeSession(SessionID sid) {
    for (auto i : handlers) {
      i.second->onDisconnected(sid);
    }
    if (disconnected)
      disconnected(sid);
    sessions.erase(sid);
  }

  void onReceive(SessionID sid, istream& i) {
    if (sessions.find(sid) == sessions.end())
      return;

    auto& session = sessions[sid];
    auto& o = *session.output;
    int reqID;
    i >> reqID;
    if (reqID == (int)RequestType::CallResponse) {
      i >> reqID;
      session.requests[reqID](i);
      session.requests.erase(reqID);
    } else {
      i >> handler >> func;
      handlers[handler]->onRequest(sid, func, reqID, i, o);
    }
  }

  template <typename... A>
  void notify(SessionID sid, string msg, A... a) {
    if (sessions.find(sid) == sessions.end())
      return;

    auto& o = *sessions[sid].output;
    o << TPRC_DELIMITER((int)RequestType::Notify) << TPRC_DELIMITER(msg);
    (..., (o << TPRC_DELIMITER(a)));
    flush(sid);
  }

  template <typename... A>
  void call(SessionID sid, string name, A... a) {
    using namespace imp;
    using Args = tuple<A...>;
    using F = ArgsTrait<Args>;

    static_assert(is_lambda_v<tuple_element_t<tuple_size_v<Args> - 1, Args>>,
                  "last param should be a lambda");

    if (sessions.find(sid) == sessions.end())
      return;

    auto& session = sessions[sid];
    auto& o = *session.output;
    auto&& args = make_tuple(a...);
    auto&& cb = get<F::Cnt - 1>(args);

    auto req = session.nextRequestID++;
    o << TPRC_DELIMITER((int)RequestType::Call) << TPRC_DELIMITER(name)
      << TPRC_DELIMITER(req);
    tuple_for(tuple_slice<0, F::Cnt - 1>(args),
              [&](auto& a) { o << TPRC_DELIMITER(a); });

    session.requests[req] = [=](istream& i) {
      typename F::CbArgs cbArgs;
      tuple_for(cbArgs, [&](auto& a) { i >> a; });
      apply(cb, cbArgs);
    };

    flush(sid);
  }

 private:
  struct Session {
    using Func = function<void(istream& i)>;
    SessionID sid;
    ostream* output;
    int nextRequestID = (int)RequestType::UserRequest;
    map<int, Func> requests;
  };
  map<SessionID, Session> sessions;
  map<string, Handler*> handlers;
  string func, handler;  // for debugging
};

template <typename istream, typename ostream = istream>
class RpcClient {
 public:
  function<void()> flush;
  function<bool(string, string, void*)> beforeResp;

  RpcClient(ostream& o) : output(o) {}
  virtual ~RpcClient() {}

  // Usage: call("Auth.Login", loginName, password, [](Result a, ...){ });
  template <typename... A>
  void call(string name, A... a) {
    using namespace imp;
    using Args = tuple<A...>;
    using F = ArgsTrait<Args>;

    static_assert(is_lambda_v<tuple_element_t<tuple_size_v<Args> - 1, Args>>,
                  "last param should be a lambda");

    auto dot = name.find_first_of('.');
    auto handler = name.substr(0, dot);
    auto func = name.substr(dot + 1);
    auto req = nextRequestID++;
    output << TPRC_DELIMITER(req) << TPRC_DELIMITER(handler)
           << TPRC_DELIMITER(func);

    auto args = make_tuple(a...);
    auto cb = get<F::Cnt - 1>(args);
    requests[req] = [=](istream& i) {
      typename F::CbArgs cbArgs;
      tuple_for(cbArgs, [&](auto& a) { i >> a; });
      bool callUser = true;
      if (beforeResp)
        callUser = beforeResp(handler, func, &get<0>(cbArgs));
      if (callUser)
        apply(cb, cbArgs);
    };
    tuple_for(tuple_slice<0, F::Cnt - 1>(args),
              [&](auto& a) { output << TPRC_DELIMITER(a); });
    flush();
  }

  void onReceive(istream& i) {
    int requestID;
    i >> requestID;
    if (requestID == (int)RequestType::Notify) {
      i >> handlerName;
      notifyHandlers[handlerName](i);
    } else if (requestID == (int)RequestType::Call) {
      int req;
      i >> handlerName >> req;
      callHandlers[handlerName](req, i);
    } else {
      requests[requestID](i);
      requests.erase(requestID);
    }
  }

  template <typename Func>
  void onNotify(string name, Func&& f) {
    notifyHandlers[name] = [=](istream& input) {
      using namespace imp;
      typename FuncTrait<Func>::Args args;
      tuple_for(args, [&](auto& a) { input >> a; });
      apply(f, args);
    };
  }

  template <typename Func>
  void onCall(string name, Func&& f) {
    callHandlers[name] = [=](int reqID, istream& input) {
      using namespace imp;
      using Args = typename FuncTrait<Func>::Args;
      using F = ArgsTrait<Args>;

      static_assert(is_lambda_v<tuple_element_t<tuple_size_v<Args> - 1, Args>>,
                    "last param should be a lambda");

      typename F::ArgsNoCb args;
      tuple_for(args, [&](auto& a) { input >> a; });
      auto&& cb = [=](auto... a) {
        output << TPRC_DELIMITER((int)RequestType::CallResponse)
               << TPRC_DELIMITER(reqID);
        (..., (output << TPRC_DELIMITER(a)));
        flush();
      };
      apply(f, tuple_cat(args, make_tuple(cb)));
    };
  }

 private:
  using Func = function<void(istream& i)>;

  map<int, Func> requests;
  map<string, function<void(istream&)>> notifyHandlers;
  map<string, function<void(int, istream&)>> callHandlers;
  int nextRequestID = (int)RequestType::UserRequest;
  ostream& output;
  string handlerName;
};

//-----------------------------------------------------------------
// Helper

template <typename T, typename istream, typename ostream = istream>
class RpcHandler : public Handler<istream, ostream> {
 public:
  using Sub = T;
  using RpcServer = RpcServer<istream, ostream>;

  RpcHandler(string name) : Handler<istream, ostream>(name) {}

  struct Reg {
    template <typename U>
    Reg(Handler<istream, ostream>* h, string n, U u) {
      h->addFunction(n, u);
    }
  };
};

#define TRPC(name) Reg __##name{this, #name, &Sub::name};
}  // namespace trpc
