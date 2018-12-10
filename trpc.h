//
// tiny rpc framework
//
// by soniced@sina.com.
// All rights reserved.
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
        constexpr auto tuple_for = [](auto&& t, auto&& f) 
        {
            return apply([&f](auto&... x) { ( ..., f(x) ); }, t);
        };

		template <size_t Ofst, class Tuple, size_t... I>
		constexpr auto slice_impl(Tuple&& t, index_sequence<I...>)
		{
			return forward_as_tuple(get<I + Ofst>(forward<Tuple>(t))...);
		}

		template <size_t I1, size_t I2, class Cont>
		constexpr auto tuple_slice(Cont&& t)
		{
			static_assert(I2 >= I1, "invalid slice");
			static_assert(tuple_size<decay_t<Cont>>::value >= I2, "slice index out of bounds");
			return slice_impl<I1>(forward<Cont>(t), make_index_sequence<I2 - I1>{});
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

		template<typename R, typename C, typename... A>
		struct FuncTrait<R(C::*)(A...)>
		{
			using Args = tuple<A...>;
		};

		template<typename Tuple>
		struct ArgsTrait
		{
			static constexpr auto AllArgCnt = tuple_size_v<Tuple>;
			using Args = typename tuple_elems<make_index_sequence<AllArgCnt - 1>, Tuple>::type;
			using CB = tuple_element_t<AllArgCnt - 1, Tuple>;
			using CBArgs = typename FuncTrait<CB>::Args;
		};

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

		virtual void init() {}
		virtual void onDisconnected(SessionID sid) {}

		void onRequest(SessionID sid, string name, istream& data, ostream& o, Signal done)
		{
			assert(funcs[name]);
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
		void addFunction(string name, Func&& f)
		{
			using namespace imp;
			using F = ArgsTrait<typename FuncTrait<Func>::Args>;

            funcs[name] = [=](SessionID sid, istream& i, ostream& o, Signal done) {
                int reqID;
                i >> reqID;
                typename F::Args args;
                get<0>(args) = sid;
                tuple_for(tuple_slice<1, F::AllArgCnt - 1>(args), [&](auto& e) { i >> e; });

				auto cb = [=, &o](auto... a) {
					o << TPRC_DELIMITER(reqID);
					(..., (o << TPRC_DELIMITER(a)));
					if (done) done(sid);
				};
				apply(f, tuple_cat(args, make_tuple(cb)));
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

		virtual ~RpcServer()
		{
			for (auto i : handlers) {
				delete i.second;
			}
		}
		void addHandlers(std::initializer_list<Handler*> h)
		{
			for (auto i : h)
				addHandler(i);
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
			if (sessions.find(sid) == sessions.end()) return;

			auto& session = sessions[sid];
			auto& o = *session.output;
			i >> handler >> func;
			assert(handlers[handler]);
			handlers[handler]->onRequest(sid, func, i, o, send);
		}
		void initHandlers()
		{
			assert(handlers.size() && "must call addHandlers before listening.");
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
			(..., (o << a));
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
		string func, handler; // 保存下来好调试
	};



	template<typename istream, typename ostream = istream>
	class RpcClient
	{
	public:
		function<void()> send;
		function<bool(string, string, void*)> beforeResp;

		RpcClient(ostream& o) :output(o)
		{}
		virtual ~RpcClient() {}

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
                typename F::CBArgs cbArgs;
                tuple_for(cbArgs, [&](auto& a) {i >> a; });
                bool callUser = true;
                if ( beforeResp ) callUser = beforeResp(handler, func, &get<0>(cbArgs));
                if ( callUser ) apply(cb, cbArgs);
            };
            tuple_for(tuple_slice<0, F::AllArgCnt - 1>(args), [&](auto& a) {output << a; });
            if ( send ) send();
        }

        void onReceive(istream& i)
        {
            int requestID;
            i >> requestID;
            if ( requestID == PUSH_REQUEST_ID ) {
                i >> pushHandlerName;
                pushHandlers[pushHandlerName](i);
            } else {
                requests[requestID](i);
            }
            requests.erase(requestID);
        }

        template<typename Func>
        void onEvent(string name, Func&& f)
        {
            pushHandlers[name] = [=](istream& input) {
                using namespace imp;
                typename FuncTrait<Func>::Args args;
                tuple_for(args, [&](auto& a) {input >> a; });
                apply(f, args);
            };
        }

	private:
		using Func = function<void(istream& i)>;
		map<int, Func> requests;
		map<string, function<void(istream&)>> pushHandlers;
		int nextRequestID = NORMAL_REUQEST_ID;
		ostream& output;
		string pushHandlerName;
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
