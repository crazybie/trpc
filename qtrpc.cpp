#include "qtrpc.h"

using namespace std;

namespace trpc
{

    void QtRpcClient::connectServer(string ip, int port, function<void()> connected)
    {
        clientSocket = new QTcpSocket(this);

        connect(clientSocket, &QTcpSocket::connected, connected);

        connect(clientSocket, &QTcpSocket::readyRead, [this] {
            QDataStream input{ clientSocket };
            while ( true ) {
                if ( packageSize == 0 && clientSocket->bytesAvailable() >= sizeof(packageSize) ) {
                    input >> packageSize;   // read block size.
                }
                if ( packageSize > 0 && clientSocket->bytesAvailable() >= packageSize ) {
                    rpc.onReceive(input);
                    packageSize = 0;
                }
                else {
                    break;
                }
            }
        });

        rpc.send = [this]() {
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

        clientSocket->connectToHost(ip.c_str(), port);
    }


    QtRpcServer* QtRpcServer::instance;


    void QtRpcServer::startListen(int port)
    {
        instance = this;

        serverSocket = new QTcpServer(this);

        connect(serverSocket, &QTcpServer::newConnection, [this] {

            auto& session = sessions[sessionID];
            session.sid = sessionID++;
            session.client = serverSocket->nextPendingConnection();
            rpcServer.addSession(session.sid, session.output);

            connect(session.client, &QTcpSocket::readyRead, [this, &session] {
                QDataStream input(session.client);
                while ( session.client->bytesAvailable() ) {
                    if ( !session.packageSize && session.client->bytesAvailable() >= sizeof(quint32) ) {
                        input >> session.packageSize;   // read block size.
                    }
                    if ( session.packageSize && session.client->bytesAvailable() >= session.packageSize ) {
                        rpcServer.onReceive(session.sid, input);
                        session.packageSize = 0;
                    }
                    else {
                        break;
                    }
                }
            });

            connect(session.client, &QAbstractSocket::disconnected, [this, &session] {
                if ( destoryed )return;
                rpcServer.removeSession(session.sid);
                sessions.erase(session.sid);
            });
        });

        rpcServer.send = [this](SessionID sid) {
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

        rpcServer.init();
        serverSocket->listen(QHostAddress::Any, port);
    }

}

