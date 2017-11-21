//
// tiny rpc framework
// by soniced@sina.com
//
#pragma once
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <tuple>
#include <assert.h>

//#define TPRC_DELEMER(n)  n << ' '
#ifndef TPRC_DELIMITER
#define TPRC_DELIMITER(n)  n
#endif

namespace trpc
{
    using namespace std;

    namespace imp
    {

        template<typename F, size_t... idx, typename... A>
        auto invoke(F f, index_sequence<idx...>, tuple<A...>& a)
        {
            return f(get<idx>(a)...);
        }

        template<int offset, typename istream, typename Tuple, size_t... idx>
        void readTuple(istream& i, Tuple& d, index_sequence<idx...>)
        {
            initializer_list<int> t = { (i >> get<idx + offset>(d), 1)... };
            (void)t;
        }

        template<typename ostream, typename Tuple, size_t... idx >
        void writeTuple(ostream& o, Tuple& d, index_sequence<idx...>)
        {
            initializer_list<int> t = { (o << TPRC_DELIMITER(get<idx>(d)), 1)... };
            (void)t;
        }

        template<typename Idx, typename Tuple>
        struct tuple_elems;

        template<typename Tuple, size_t... I>
        struct tuple_elems<index_sequence<I...>, Tuple>
        {
            using type = tuple<tuple_element_t<I, Tuple>...>;
        };

        template<typename T>
        struct FuncTrait : FuncTrait<decltype(&T::operator())>
        {};

        template<typename R, typename C, typename... A>
        struct FuncTrait<R(C::*)(A...)const>
        {
            using Args = tuple<A...>;
        };

        template<typename Tuple>
        struct ArgsTrait
        {
            static const int AllArgCnt = tuple_size<Tuple>::value;
            using Args = typename tuple_elems<make_index_sequence<AllArgCnt - 1>, Tuple>::type;
            using CB = tuple_element_t<AllArgCnt - 1, Tuple>;
            using CBArgs = typename FuncTrait<CB>::Args;
        };

        template<typename ostream, typename CB, typename CBArg, typename...A>
        auto genTupleSerializer(tuple<A...>, int tag, ostream& o, CB& cb, CBArg cbArg)
        {
            return [&, tag, cb, cbArg](A...a) {
                o << TPRC_DELIMITER(tag);
                auto i = { (o << TPRC_DELIMITER(a), 1)... };
                (void)i;
                if (cb) cb(cbArg);
            };
        }
    }

    using SessionID = int;
    using Signal = function<void(SessionID)>;

    template<typename... A>
    using Resp = function<void(A...)>;

    template<typename istream, typename ostream>
    class RpcServer;

    template<typename istream, typename ostream = istream>
    class Handler
    {
    public:
        using Func = function<void(SessionID, istream&, ostream&, Signal)>;
        using Server = RpcServer<istream, ostream>;

        string name;

        Handler(string n) :name(n) {}
        virtual ~Handler() {}

        virtual void init(){}
        virtual void onDisconnected(SessionID sid){}

        void onRequest(SessionID sid, string name, istream& data, ostream& o, Signal done)
        {
            funcs[name](sid, data, o, done);
        }
        template<typename C, typename R, typename... A>
        void addFunction(string name, R(C::*mf)(A...))
        {
            addFunction(name, [this, mf](A... a) {
                return (static_cast<C*>(this)->*mf)(a...);
            });
        }
        template<typename Func>
        void addFunction(string name, Func f)
        {
            using namespace imp;
            using F = ArgsTrait<typename FuncTrait<Func>::Args>;

            funcs[name] = [=](SessionID sid, istream& i, ostream& o, Signal done) {
                int reqID;
                i >> reqID;
                F::Args args;
                get<0>(args) = sid;
                readTuple<1>(i, args, make_index_sequence<F::AllArgCnt - 2>());
                F::CB cb = genTupleSerializer(F::CBArgs(), reqID, o, done, sid);
                auto args2 = tuple_cat(args, tie(cb));
                invoke(f, make_index_sequence<F::AllArgCnt>(), args2);
            };
        }
        void setServer(Server* s) { server = s; }

    protected:
        Server * server;
    private:
        map<string, Func> funcs;
    };




    const int PUSH_REQUEST_ID = 1;
    const int NORMAL_REUQEST_ID = 2;

    template<typename istream, typename ostream = istream>
    class RpcServer
    {
    public:
        using Handler = Handler<istream, ostream>;

        Signal send;
        Signal disconnected;

        ~RpcServer() {
            for (auto i : handlers) {
                delete i.second;
            }
        }
        void addHandler(Handler* h)
        {
            handlers[h->name] = h;
        }
        void addSession(SessionID sid, ostream& o)
        {
            sessions[sid] = { sid, &o };
        }
        void removeSession(SessionID sid)
        {
            for (auto i : handlers) {
                i.second->onDisconnected(sid);
            }
            if (disconnected) disconnected(sid);
            sessions.erase(sid);
        }
        void onReceive(SessionID sid, istream& i)
        {
            if (sessions.find(sid) == sessions.end())return;

            auto& session = sessions[sid];
            auto& o = *session.output;
            string func, handler;
            i >> handler >> func;
            handlers[handler]->onRequest(sid, func, i, o, send);
        }
        void initHandlers()
        {
            for (auto i : handlers) {
                i.second->setServer(this);
                i.second->init();
            }
        }
        template<typename... A>
        void pushMsg(SessionID sid, string event, A... a)
        {
            if (sessions.find(sid) == sessions.end())return;

            auto& o = *sessions[sid].output;
            o << TPRC_DELIMITER(PUSH_REQUEST_ID) << TPRC_DELIMITER(event);
            auto t = { (o << a, 1)... };
            if (send) send(sid);
        }

    private:
        struct Session
        {
            SessionID sid;
            ostream* output;
        };
        map<SessionID, Session> sessions;
        map<string, Handler*> handlers;
    };



    template<typename istream, typename ostream = istream>
    class RpcClient
    {
    public:
        function<void()> send;
        function<bool(string, string, void*)> beforeResp;

        RpcClient(ostream& o) :output(o)
        {}

        // call('Auth.Login', loginName, password, [](<ArgsFromServer...>){ });
        template<typename... A>
        void call(string name, A... a)
        {
            using namespace imp;
            using F = ArgsTrait<tuple<A...>>;

            auto dot = name.find_first_of('.');
            auto handler = name.substr(0, dot);
            auto func = name.substr(dot + 1);
            auto req = nextRequestID++;
            output << TPRC_DELIMITER(handler) << TPRC_DELIMITER(func) << TPRC_DELIMITER(req);

            auto args = make_tuple(a...);
            auto cb = get<F::AllArgCnt - 1>(args);
            requests[req] = [=](istream& i) {
                F::CBArgs args;
                auto idx = make_index_sequence<tuple_size<F::CBArgs>::value>();
                readTuple<0>(i, args, idx);

                bool callUser = true;
                if (beforeResp) callUser = beforeResp(handler, func, &args);
                if (callUser) invoke(cb, idx, args);
            };
            writeTuple(output, args, make_index_sequence<F::AllArgCnt - 1>());
            if (send) send();
        }

        void onReceive(istream& i)
        {
            int requestID;
            i >> requestID;
            if (requestID == PUSH_REQUEST_ID) {
                string name;
                i >> name;
                pushHandlers[name](i);
            }
            else {
                requests[requestID](i);
            }
            requests.erase(requestID);
        }

        template<typename Func>
        void onEvent(string name, Func f)
        {
            pushHandlers[name] = [=](istream& input) {
                using namespace imp;
                using Args = typename FuncTrait<Func>::Args;
                Args args;
                auto idx = make_index_sequence<tuple_size<Args>::value>();
                readTuple<0>(input, args, idx);
                invoke(f, idx, args);
            };
        }

    private:
        using Func = function<void(istream& i)>;
        map<int, Func> requests;
        map<string, function<void(istream&)>> pushHandlers;
        int nextRequestID = NORMAL_REUQEST_ID;
        ostream& output;
    };

    //-----------------------------------------------------------------

    template<typename T, typename istream, typename ostream = istream>
    class RpcHandler : public Handler<istream, ostream>
    {
    public:
        using Sub = T;
        using RpcServer = RpcServer<istream, ostream>;

        RpcHandler(string name) : Handler(name) {}

        struct Reg
        {
            template<typename U>
            Reg(Handler<istream, ostream>* h, string n, U u)
            {
                h->addFunction(n, u);
            }
        };
    };

#define TRPC(name) Reg __##name{this, #name, &Sub::name};
}
