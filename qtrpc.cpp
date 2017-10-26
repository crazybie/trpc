#include "qtrpc.h"

//QDataStream& operator<<(QDataStream& s, QVariant& v)
//{
//	s << (int)v.type();
//	switch (v.type()) {
//	case QVariant::Int: s << v.toInt(); break;
//	case QVariant::Double: s << v.toFloat(); break;
//	case QVariant::String: s << v.toString(); break;
//	case QVariant::Bool: s << v.toBool(); break;
//	}
//	return s;
//}
//
//QDataStream& operator >> (QDataStream& s, QVariant& v)
//{
//	int type;
//	s >> type;
//	switch (type) {
//	case QVariant::Int: { int i; s >> i; v.setValue(i); break; }
//	case QVariant::Double: { double i; s >> i; v.setValue(i); break; }
//	case QVariant::String: { QString i; s >> i; v.setValue(i); break; }
//	case QVariant::Bool: { bool i; s >> i; v.setValue(i); break; }
//	}
//	return s;
//}

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

        clientSocket->connectToHost(ip.c_str(), port);
    }

    void QtRpcServer::startListen(int port)
    {
        serverSocket = new QTcpServer(this);

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
        serverSocket->listen(QHostAddress::Any, port);
    }

}

