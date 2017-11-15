//
// qt binding for trpc
// by soniced@sina.com
//
#pragma once
#include <QtNetwork/QtNetwork>
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

    using SocketCb = std::function<void(bool, QAbstractSocket::SocketError)>;

    class QtRpcClient : public QObject, public RpcClient<QDataStream>
    {
    public:
        QtRpcClient(QObject* parent=nullptr): QObject(parent), RpcClient(output){}
        void connectServer(QString ip, int port, SocketCb cb);
        bool isConnected() {
            return mIsConnected;
        }
        QAbstractSocket::SocketError getSocketError() { return socketError; }
    private:
        bool mIsConnected = false;
        QAbstractSocket::SocketError socketError;
        int packageSize = 0;
        QTcpSocket* clientSocket;
        QByteArray  block;
        QDataStream output{ &block, QIODevice::WriteOnly };        
    };


    class QtRpcServer : public QObject, public RpcServer<QDataStream>
    {
    public:
        using QObject::QObject;
        void startListen(QHostAddress addr, int port, SocketCb cb);
        ~QtRpcServer()
        {
            destoryed = true;
        }
        auto& getSession(int sid)
        {
            return sessions[sid];
        }
        struct Session
        {
            int sid = 0;
            QTcpSocket* client;
            QByteArray  block;
            QDataStream output{ &block, QIODevice::WriteOnly };
            int packageSize = 0;
            map<string, QVariant> data;

            QVariant& operator[](string k) { return data[k]; }
        };

    private:
        QTcpServer* serverSocket;
        bool destoryed = false;        
        map<int, Session> sessions;
        int sessionID = 100;
    };

    

    template<typename T>
    class QtRpcHandler : public RpcHandler<T, QDataStream>
    {
    public:
        using RpcHandler<T,QDataStream>::RpcHandler;

        QtRpcHandler(string name): RpcHandler(name){}

        QtRpcServer* getServer() {
            return static_cast<QtRpcServer*>(server);
        }
        auto& getSession(SessionID sid) { 
            return getServer()->getSession(sid); 
        }
    };

}