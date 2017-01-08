//
// qt binding for trpc
// by soniced@sina.com
//
#pragma once
#include <QtNetwork>
#include "trpc.h"

namespace trpc
{
    inline QDataStream& operator >> (QDataStream& s, std::string& v)
    {
        char* p;
        s >> p;
        v = p;
        delete[]p;
        return s;
    }

    inline QDataStream& operator<< (QDataStream& s, std::string& v)
    {
        return s << v.c_str();
    }


    class QtRpcClient : public QObject, public RpcClient<QDataStream>
    {
        Q_OBJECT
    public:
        QtRpcClient(QObject* parent): RpcClient(output){}
        void connectServer(string ip, int port, function<void()> connected);
    private:
        int packageSize = 0;
        QTcpSocket* clientSocket;
        QByteArray  block;
        QDataStream output{ &block, QIODevice::WriteOnly };        
    };


    class QtRpcServer : public QObject, public RpcServer<QDataStream>
    {
        Q_OBJECT
    public:
        using QObject::QObject;
        void startListen(int port);
        ~QtRpcServer()
        {
            destoryed = true;
        }
        static QtRpcServer* get()
        {
            return (QtRpcServer*)RpcServer::get();
        }
        auto& getSessoins()
        {
            return sessions;
        }
    private:
        QTcpServer* serverSocket;
        bool destoryed = false;

        struct Session
        {
            int sid = 0;
            QTcpSocket* client;
            QByteArray  block;
            QDataStream output{ &block, QIODevice::WriteOnly };
            int packageSize = 0;
            map<string, QVariant> data;

            QVariant& operator[](string k)
            {
                return data[k];
            }
        };
        map<int, Session> sessions;
        int sessionID = 100;
    };

    inline auto& getSession(SessionID sid) { return QtRpcServer::get()->getSessoins()[sid]; }

    template<typename T>
    class QtRpcHandler : public RpcHandler<T, QDataStream>
    {
    public:
        using RpcHandler::RpcHandler;
    };

}