//
// by soniced@sina.com.
// All rights reserved.
//
#include "qtrpc.h"


namespace trpc
{

    void QtRpcClient::connectServer(QString ip, int port, SocketCb cb)
    {
        close();

        socket = new QTcpSocket();
        mIsConnected = false;

        socket->connect(socket, &QTcpSocket::connected, [this,cb] {
            mIsConnected = true;
            output.device()->reset();
            cb(true, QTcpSocket::UnknownSocketError);
        });

        socket->connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), [this, cb](QAbstractSocket::SocketError err) {
            socketError = err;
            cb(false, err);
        });

        socket->connect(socket, &QAbstractSocket::disconnected, [this] {
            mIsConnected = false;
        });

        socket->connect(socket, &QTcpSocket::readyRead, [this] {
            QDataStream input{ socket };
            while (socket) {
                if ( packageSize == 0 && socket->bytesAvailable() >= sizeof(packageSize) ) {
                    input >> packageSize;   // read block size.
                }
                if ( packageSize > 0 && socket->bytesAvailable() >= packageSize ) {
                    QDataStream s(socket->read(packageSize));
                    onReceive(s);
                    assert(s.status() == QDataStream::Ok);

                    if (onRead) onRead(packageSize);
                    packageSize = 0;                    
                }
                else {
                    break;
                }
            }
        });

        send = [this]() {
            int sz = block.size();
            QByteArray b;
            QDataStream s(&b, QIODevice::WriteOnly);
            s << sz;
            socket->write(b);
            socket->write(block);
            socket->flush(); // 可能触发disconnect
            block.clear();

            if (mIsConnected) {                
                output.device()->reset();
            }            
        };

        socket->connectToHost(ip, port);
    }

    void QtRpcClient::close()
    {
        if (socket) {
            socket->close();
            delete socket;
            socket = nullptr;
        }        
        mIsConnected = false;
    }

    QtRpcServer::~QtRpcServer()
    {
        close();
    }

    void QtRpcServer::close()
    {
        if (socket) {
            socket->close();
            delete socket;
            socket = nullptr;
        }
    }

    void QtRpcServer::startListen(QString ip, int port, SocketCb cb)
    {        
        socket = new QTcpServer();

        socket->connect(socket, &QTcpServer::acceptError, [this, cb](QAbstractSocket::SocketError err) {
            cb(false, err);
        });

        socket->connect(socket, &QTcpServer::newConnection, [this] {

            auto& session = sessions[sessionID];
            session.sid = sessionID++;
            session.client = socket->nextPendingConnection();
            addSession(session.sid, session.output);

            socket->connect(session.client, &QTcpSocket::readyRead, [this, &session] {
                QDataStream input(session.client);                
                while ( session.client->bytesAvailable() ) {
                    if ( !session.packageSize && session.client->bytesAvailable() >= sizeof(session.packageSize) ) {
                        input >> session.packageSize;
                    }
                    if ( session.packageSize && session.client->bytesAvailable() >= session.packageSize ) {
                        QDataStream s(session.client->read(session.packageSize));
                        auto sid_copy = session.sid;
                        onReceive(session.sid, s);
                        assert(s.status() == QDataStream::Ok);

                        if (onRead) onRead(session.packageSize);
                        session.packageSize = 0;
                        if (sessions.find(sid_copy) == sessions.end()) return;
                    }
                    else {
                        break;
                    }
                }
            });

            socket->connect(session.client, &QAbstractSocket::disconnected, [this, &session] {
                removeSession(session.sid);
                sessions.erase(session.sid);
            });
        });

        send = [this](SessionID sid) {
            if ( sessions.find(sid) == sessions.end() ) return;

            auto& session = sessions[sid];
            int sz = session.block.size();
            QByteArray b;
            QDataStream s(&b, QIODevice::WriteOnly);
            s << sz;
            session.client->write(b);
            session.client->write(session.block);
            session.client->flush(); // 可能触发disconnect 
            if (sessions.find(sid) == sessions.end()) return;

            session.block.clear();
            session.output.device()->reset();
        };

        initHandlers();

        if (!socket->listen(QHostAddress(ip), port)) {
            return cb(false, socket->serverError());
        }

        cb(true, QTcpSocket::UnknownSocketError);
    }
}

