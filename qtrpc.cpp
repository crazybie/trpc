#include "qtrpc.h"


namespace trpc
{

    void QtRpcClient::connectServer(QString ip, int port, SocketCb cb)
    {
        socket = new QTcpSocket(this);
        mIsConnected = false;

        connect(socket, &QTcpSocket::connected, [this,cb] {
            mIsConnected = true;
            output.device()->reset();
            cb(true, QTcpSocket::UnknownSocketError);
        });

        connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), [this, cb](QAbstractSocket::SocketError err) {
            socketError = err;
            cb(false, err);
        });

        connect(socket, &QTcpSocket::readyRead, [this] {
            QDataStream input{ socket };
            while ( true ) {
                if ( packageSize == 0 && socket->bytesAvailable() >= sizeof(packageSize) ) {
                    input >> packageSize;   // read block size.
                }
                if ( packageSize > 0 && socket->bytesAvailable() >= packageSize ) {
                    onReceive(input);
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
            socket->flush();
            block.clear();
            output.device()->reset();
        };

        socket->connectToHost(ip, port);
    }

    void QtRpcClient::close()
    {
        socket->close();
        delete socket;
        mIsConnected = false;
    }

    void QtRpcServer::startListen(QHostAddress addr, int port, SocketCb cb)
    {
        socket = new QTcpServer(this);

        connect(socket, &QTcpServer::acceptError, [this, cb](QAbstractSocket::SocketError err) {
            cb(false, err);
        });

        connect(socket, &QTcpServer::newConnection, [this] {

            auto& session = sessions[sessionID];
            session.sid = sessionID++;
            session.client = socket->nextPendingConnection();
            addSession(session.sid, session.output);

            connect(session.client, &QTcpSocket::readyRead, [this, &session] {
                QDataStream input(session.client);
                while ( session.client->bytesAvailable() ) {
                    if ( !session.packageSize && session.client->bytesAvailable() >= sizeof(quint32) ) {
                        input >> session.packageSize;   // read block size.
                    }
                    if ( session.packageSize && session.client->bytesAvailable() >= session.packageSize ) {
                        onReceive(session.sid, input);
                        session.packageSize = 0;
                    }
                    else {
                        break;
                    }
                }
            });

            connect(session.client, &QAbstractSocket::disconnected, [this, &session] {
                if ( destoryed ) return;
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
            session.client->flush();
            session.block.clear();
            session.output.device()->reset();
        };

        initHandlers();

        if (!socket->listen(addr, port)) {
            return cb(false, socket->serverError());
        }

        cb(true, QTcpSocket::UnknownSocketError);
    }
}

