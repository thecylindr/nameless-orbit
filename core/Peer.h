#ifndef PEER_H
#define PEER_H

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QJsonObject>
#include "CryptoUtils.h"

class Identity;

// Одно TCP-соединение с другим устройством Orbit: рукопожатие (см. CryptoUtils.h), затем обмен строками JSON
// внутри защищённого канала. Один объект на соединение — PeerManager создаёт его как для исходящих, так и для
// входящих подключений; состояние пары (доверие, разрешения на плагины) Peer не хранит — это TrustStore.
class Peer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString deviceId READ deviceId NOTIFY handshakeDone)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY handshakeDone)
    Q_PROPERTY(QString deviceKind READ deviceKind NOTIFY handshakeDone)
    Q_PROPERTY(QString address READ addressString CONSTANT)
    Q_PROPERTY(bool authenticated READ authenticated NOTIFY handshakeDone)

public:
    // исходящее соединение: адрес узнан через Discovery
    static Peer *connectTo(const QHostAddress &addr, quint16 port, Identity *identity, QObject *parent = nullptr);
    // входящее соединение: сокет уже принят TcpServer'ом
    static Peer *fromIncoming(QTcpSocket *socket, Identity *identity, QObject *parent = nullptr);

    QString deviceId() const { return m_peerDeviceId; }
    QString deviceName() const { return m_peerDeviceName; }
    QString deviceKind() const { return m_peerDeviceKind; }
    QString deviceModel() const { return m_peerDeviceModel; }
    QString sasCode() const { return m_channel.sas; }      // код сверки при сопряжении: одинаковый у обеих сторон
    qint64 pendingBytes() const { return m_socket ? m_socket->bytesToWrite() : 0; }   // сколько ещё не ушло в сеть (для отбрасывания кадров)
    QByteArray peerPublicKey() const { return m_peerStaticPublic; }
    QString addressString() const;
    quint16 remotePort() const { return m_socket ? m_socket->peerPort() : 0; }
    bool authenticated() const { return m_channel.ready; }
    bool isInitiator() const { return m_isInitiator; }

    // отправка уже готового JSON-сообщения (см. Protocol.h); не действует до handshakeDone
    void send(const QJsonObject &message);
    void close(const QString &reason = QString());

signals:
    // рукопожатие завершено; identityKnownBefore — false, если этого deviceId раньше не было в TrustStore
    // (вызывающая сторона решает — это сопряжение или подключение уже доверенного устройства)
    void handshakeDone();
    void handshakeFailed(const QString &reason);
    void messageReceived(const QJsonObject &message);
    void disconnected();

private slots:
    void onConnected();
    void onReadyRead();
    void onSocketError();
    void onPing();

private:
    explicit Peer(QTcpSocket *socket, Identity *identity, bool isInitiator, QObject *parent = nullptr);
    void sendHello();
    void tryParseHello(const QByteArray &line);
    void finishHandshake();

    QTcpSocket *m_socket;
    Identity *m_identity;
    bool m_isInitiator;

    QByteArray m_ephemeralSecret, m_ephemeralPublic;
    QByteArray m_peerEphemeralPublic, m_peerStaticPublic;
    QByteArray m_transcript;       // конкатенация обеих строк HELLO — входит в вывод ключа (защита от подмены)
    QByteArray m_plainBuffer;      // байты до HELLO от собеседника (ещё не зашифровано)

    QString m_peerDeviceId, m_peerDeviceName, m_peerDeviceKind, m_peerDeviceModel;
    QByteArray m_ownHello;         // своя строка HELLO: в запись для вывода ключа обе строки идут в ОДНОМ порядке (HELLO1, затем HELLO2)

    SecureChannel m_channel;
    QByteArray m_cipherBuffer;

    QTimer m_handshakeTimeout;
    QTimer m_pingTimer;
    bool m_handshakeComplete = false;
    qint64 m_lastRxMs = 0;           // когда последний раз приходили данные: тишина дольше срока — соединение мертво
};

#endif // PEER_H
