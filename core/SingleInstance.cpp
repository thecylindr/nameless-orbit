#include "SingleInstance.h"
#include <QLocalServer>
#include <QLocalSocket>

SingleInstance::SingleInstance(const QString &name, QObject *parent)
    : QObject(parent), m_name(name)
{
}

bool SingleInstance::ask(const QString &name, const QByteArray &command, QByteArray *reply, int timeoutMs)
{
    QLocalSocket socket;
    socket.connectToServer(name);
    if (!socket.waitForConnected(timeoutMs)) return false;
    socket.write(command + '\n');
    socket.flush();
    QByteArray data;
    while (!data.contains('\n') && socket.waitForReadyRead(timeoutMs))
        data += socket.readAll();
    if (reply) *reply = data.trimmed();
    return true;
}

bool SingleInstance::listen()
{
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);   // только этот пользователь
    if (!m_server->listen(m_name)) {
        // имя занято: отвечают — работает другой экземпляр, не трогаем; молчит — канал остался от аварийного
        // завершения (Linux не убирает файл сокета сам) — убрать и занять снова
        if (ask(m_name, QByteArrayLiteral("ping"), nullptr, 500)) return false;
        QLocalServer::removeServer(m_name);
        if (!m_server->listen(m_name)) return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, [this]() {
        while (QLocalSocket *s = m_server->nextPendingConnection()) {
            connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
            connect(s, &QLocalSocket::readyRead, this, [this, s]() {
                if (!s->canReadLine()) return;
                const QByteArray command = s->readLine().trimmed().left(256);
                const QByteArray answer = m_handler ? m_handler(command) : QByteArray("{}");
                s->write(answer + '\n');
                s->flush();
                s->disconnectFromServer();
            });
        }
    });
    return true;
}
