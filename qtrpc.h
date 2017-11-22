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
        bool isConnected() { return mIsConnected; }
        QAbstractSocket::SocketError getSocketError() { return socketError; }
        void close();
        QTcpSocket* getSocket() { return socket; }
    private:
        bool mIsConnected = false;
        QAbstractSocket::SocketError socketError;
        int packageSize = 0;
        QTcpSocket* socket = nullptr;
        QByteArray  block;
        QDataStream output{ &block, QIODevice::WriteOnly };        
    };


    class QtRpcServer : public QObject, public RpcServer<QDataStream>
    {
    public:
        using QObject::QObject;
        ~QtRpcServer();
        void startListen(QHostAddress addr, int port, SocketCb cb);        
        QVariant& getSessionField(int sid, QString k) { return sessions[sid].data[k]; }
        void setSessionField(int sid, QString k, QVariant v) { sessions[sid].data[k] = v; }
    private:
        struct Session
        {
            int sid = 0;
            QTcpSocket* client;
            QByteArray  block;
            QDataStream output{ &block, QIODevice::WriteOnly };
            int packageSize = 0;
            map<QString, QVariant> data;
        };
        QTcpServer* socket = nullptr;
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
        QtRpcServer* getServer() {  return static_cast<QtRpcServer*>(server); }
        QVariant& getSessionField(SessionID sid, QString f) {  return getServer()->getSessionField(sid, f);  }
        void setSessionField(int sid, QString k, QVariant v) { getServer()->setSessionField(sid, k, v); }
    };

}