//
// tiny rpc framework
// by soniced@sina.com
//
#pragma once
#include <string>
#include <cassert>
#include <vector>
#include <map>
#include <functional>
#include <tuple>

#ifndef TPRC_DELIMITER
#define TPRC_DELIMITER(n)  n
//#define TPRC_DELEMER(n)  n << ' '
#endif

namespace trpc
{
    using namespace std;

    namespace imp
    {
        template<typename R, typename C, typename F, size_t... idx, typename... A>
        R invoke(C* c, F f, index_sequence<idx...>, tuple<A...>& a)
        {
            return ( c->*f )( get<idx>(a)... );
        }

        template<typename R, typename F, size_t... idx, typename... A>
        R invoke(F f, index_sequence<idx...>, tuple<A...>& a)
        {
            return f(get<idx>(a)...);
        }

        template<int offset, typename istream, typename Tuple, size_t... idx>
        void readTuple(istream& i, Tuple& d, index_sequence<idx...>)
        {
            auto t = { ( i >> get<idx + offset>(d), 1 )... };
            (void)t;
        }

        template<typename ostream, typename Tuple, size_t... idx >
        void writeTuple(ostream& o, Tuple& d, index_sequence<idx...>)
        {
            auto t = { ( o << TPRC_DELIMITER(get<idx>(d)), 1 )... };
            (void)t;
        }

        template<typename Idx, typename... A>
        struct tuple_elems;

        template<typename... A, size_t... I>
        struct tuple_elems<index_sequence<I...>, A...>
        {
            using T = tuple<A...>;
            using type = tuple<tuple_element_t<I, T>...>;
        };

        template<typename T>
        struct FuncTrait : FuncTrait<decltype( &T::operator() )>
        {};

        template<typename R, typename C, typename... A>
        struct FuncTrait<R(C::*)( A... )const>
        {
            using Args = tuple<A...>;
        };

        template<typename ostream, typename CB, typename CBArg, typename...A>
        auto genTupleSerializer(tuple<A...>, int tag, ostream& o, CB& cb, CBArg cbArg)
        {
            return [&, tag, cb, cbArg](A...a) {
                o << TPRC_DELIMITER(tag);
                auto i = { ( o << TPRC_DELIMITER(a), 1 )... };
                (void)i;
                if ( cb ) cb(cbArg);
            };
        }
    }

    using SessionID = int;
    using Signal = function<void(SessionID)>;

    template<typename... A>
    using Resp = function<void(A...)>;
    

    template<typename istream, typename ostream = istream>
    class Handler
    {
    public:
        using Func = function<void(SessionID, istream&, ostream&, Signal)>;

        Handler()
        {
            getCurrent() = this;
        }
        virtual void init()
        {}
        static Handler*& getCurrent()
        {
            static Handler* s;
            return s;
        }
        void onRequest(SessionID sid, string name, istream& data, ostream& o, Signal done)
        {
            funcs[name](sid, data, o, done);
        }
        template<typename C, typename R, typename... A>
        void addFunction(string name, R(C::*mf)( A... ))
        {
            using namespace imp;
            using AllArgs = tuple<A...>;
            using Args = tuple_elems<make_index_sequence<sizeof...(A)-1>, A...>::type;
            using CB = tuple_element_t<sizeof...(A)-1, AllArgs>;
            using CBArgs = typename FuncTrait<CB>::Args;
            using MF2 = R(Handler::*) ( A... );

            funcs[name] = [=](SessionID sid, istream& i, ostream& o, Signal done) {
                int reqID;
                i >> reqID;
                Args args;
                get<0>(args) = sid;
                readTuple<1>(i, args, make_index_sequence<sizeof...(A)-2>());
                CB cb = genTupleSerializer(CBArgs(), reqID, o, done, sid);
                auto args2 = tuple_cat(args, tie(cb));
                invoke<void>(this, (MF2)mf, index_sequence_for<A...>(), args2);
            };
        }

        struct Reg
        {
            template<typename T>
            Reg(string n, T t)
            {
                getCurrent()->addFunction(n, t);
            }
        };
    private:
        string name;
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

        void addHandler(string n, Handler* h)
        {
            m_handlers[n] = h;
        }
        void addSession(SessionID sid, ostream& o)
        {
            sessions[sid] = { sid, &o };
        }
        void removeSession(SessionID sid)
        {
            if ( disconnected) disconnected(sid);
            sessions.erase(sid);
        }
        void onReceive(SessionID sid, istream& i)
        {
            if ( sessions.find(sid) == sessions.end() )return;

            auto& session = sessions[sid];            
            auto& o = session.output;
            string func, handler;
            i >> handler >> func;            
            m_handlers[handler]->onRequest(sid, func, i, *o, send);
        }
        void init()
        {
            for ( auto i : m_handlers )
                i.second->init();
        }
        template<typename... A>
        void pushMsg(SessionID sid, string event, A... a)
        {
            if ( sessions.find(sid) == sessions.end() )return;

            auto& o = *sessions[sid].output;
            o << TPRC_DELIMITER(PUSH_REQUEST_ID) << TPRC_DELIMITER(event);
            auto t = { ( o << a, 1 )... };
            if ( send ) send(sid);
        }
        static RpcServer& get()
        {
            static RpcServer s;
            return s;
        }
    private:
        RpcServer() = default;
        struct Session
        {
            SessionID sid;
            ostream* output;
        };
        map<SessionID, Session> sessions;
        map<string, Handler*> m_handlers;
    };


    template<typename T, typename istream, typename ostream = istream>
    class RpcHandler : public Handler<istream, ostream>
    {
    public:
        using Sub = T;
        using RpcServer = RpcServer<istream, ostream>;
        RpcHandler(string name)
        {
            RpcServer::get().addHandler(name, &instance);
        }
        static T& get()
        {
            return instance;
        }
    private:
        static T instance;
    };

    template<typename T, typename istream, typename ostream>
    T RpcHandler<T, istream, ostream>::instance;

#define TRPC(name) Reg __##name{#name, &Sub::name};


    template<typename istream, typename ostream = istream>
    class RpcClient
    {
    public:
        function<void()> send;

        RpcClient(ostream& o) :output(o)
        {
        }
        template<typename... A>
        void call(string name, A... a)
        {
            using namespace imp;
            using AllArgs = tuple<A...>;
            constexpr int argCnt = sizeof...(A)-1;
            using Args = tuple_elems<make_index_sequence<argCnt>, A...>::type;
            using CB = tuple_element_t<argCnt, AllArgs>;
            using CBArgs = typename FuncTrait<CB>::Args;

            auto dot = name.find_first_of('.');
            auto handler = name.substr(0, dot);
            auto func = name.substr(dot + 1);
            auto req = nextRequestID++;
            output << TPRC_DELIMITER(handler) << TPRC_DELIMITER(func) << TPRC_DELIMITER(req);

            auto args = make_tuple(a...);
            auto cb = get<argCnt>(args);
            requests[req] = [=](istream& i) {
                CBArgs args;
                auto idx = make_index_sequence<tuple_size<CBArgs>::value>();
                readTuple<0>(i, args, idx);
                invoke<void>(cb, idx, args);
            };
            writeTuple(output, args, make_index_sequence<argCnt>());
            if ( send ) send();
        }
        void onReceive(istream& i)
        {
            int requestID;
            i >> requestID;
            if ( requestID == PUSH_REQUEST_ID ) {
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
                invoke<void>(f, idx, args);
            };
        }

    private:
        using Func = function<void(istream& i)>;        
        map<int, Func> requests;
        map<string, function<void(istream&)>> pushHandlers;
        int nextRequestID = NORMAL_REUQEST_ID;
        ostream& output;
    };
}
