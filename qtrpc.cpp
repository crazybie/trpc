#include "qtrpc.h"


namespace trpc
{

    void QtRpcClient::connectServer(QString ip, int port, SocketCb cb)
    {
        clientSocket = new QTcpSocket(this);
        mIsConnected = false;

        connect(clientSocket, &QTcpSocket::connected, [this,cb] {
            mIsConnected = true;
            output.device()->reset();
            cb(true, QTcpSocket::UnknownSocketError);
        });

        connect(clientSocket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), [this, cb](QAbstractSocket::SocketError err) {
            socketError = err;
            cb(false, err);
        });

        connect(clientSocket, &QTcpSocket::readyRead, [this] {
            QDataStream input{ clientSocket };
            while ( true ) {
                if ( packageSize == 0 && clientSocket->bytesAvailable() >= sizeof(packageSize) ) {
                    input >> packageSize;   // read block size.
                }
                if ( packageSize > 0 && clientSocket->bytesAvailable() >= packageSize ) {
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
            clientSocket->write(b);
            clientSocket->write(block);
            clientSocket->flush();
            block.clear();
            output.device()->reset();
        };

        clientSocket->connectToHost(ip, port);
    }

    void QtRpcServer::startListen(QHostAddress addr, int port, SocketCb cb)
    {
        serverSocket = new QTcpServer(this);

        connect(serverSocket, &QTcpServer::acceptError, [this, cb](QAbstractSocket::SocketError err) {
            cb(false, err);
        });

        connect(serverSocket, &QTcpServer::newConnection, [this] {

            auto& session = sessions[sessionID];
            session.sid = sessionID++;
            session.client = serverSocket->nextPendingConnection();
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

        if (!serverSocket->listen(addr, port)) {
            return cb(false, serverSocket->serverError());
        }

        cb(true, QTcpSocket::UnknownSocketError);
    }
}

