#include <QRegularExpression>
#include "Peer.h"
#include "Identity.h"
#include "Protocol.h"
#include "../platform/PlatformBridge.h"
#include <QDebug>
#include <QDateTime>

namespace {
constexpr int HANDSHAKE_TIMEOUT_MS = 6000;
constexpr int PING_INTERVAL_MS = 10000;
constexpr qint64 DEAD_AFTER_MS = 30000;   // три пропущенных проверки подряд
constexpr int MAX_HELLO_LEN = 1024;   // защита от чужого сервиса на порту, слающего мусор без конца строки
}

Peer *Peer::connectTo(const QHostAddress &addr, quint16 port, Identity *identity, QObject *parent)
{
    auto *socket = new QTcpSocket();
    auto *peer = new Peer(socket, identity, /*isInitiator=*/true, parent);
    socket->setParent(peer);
    QObject::connect(socket, &QTcpSocket::connected, peer, &Peer::onConnected);
    socket->connectToHost(addr, port);
    return peer;
}

Peer *Peer::fromIncoming(QTcpSocket *socket, Identity *identity, QObject *parent)
{
    auto *peer = new Peer(socket, identity, /*isInitiator=*/false, parent);
    socket->setParent(peer);
    peer->sendHello();   // отвечающая сторона шлёт HELLO2 сразу, как только сокет принят
    return peer;
}

Peer::Peer(QTcpSocket *socket, Identity *identity, bool isInitiator, QObject *parent)
    : QObject(parent), m_socket(socket), m_identity(identity), m_isInitiator(isInitiator)
{
    m_ephemeralSecret = CryptoUtils::randomBytes(CryptoUtils::KEY_SIZE);
    m_ephemeralPublic = CryptoUtils::x25519Public(m_ephemeralSecret);

    // без алгоритма Нейгла: сообщения (кадры экрана, подтверждения, ввод) уходят сразу, а не ждут до 40 мс, пока накопится сегмент или придёт ACK
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connect(m_socket, &QTcpSocket::readyRead, this, &Peer::onReadyRead);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &Peer::onSocketError);
    connect(m_socket, &QTcpSocket::disconnected, this, [this]() { emit disconnected(); });

    m_handshakeTimeout.setSingleShot(true);
    m_handshakeTimeout.setInterval(HANDSHAKE_TIMEOUT_MS);
    connect(&m_handshakeTimeout, &QTimer::timeout, this, [this]() {
        if (!m_handshakeComplete) { emit handshakeFailed(QStringLiteral("истекло время рукопожатия")); close(); }
    });
    m_handshakeTimeout.start();

    m_pingTimer.setInterval(PING_INTERVAL_MS);
    connect(&m_pingTimer, &QTimer::timeout, this, &Peer::onPing);
}

void Peer::onConnected()
{
    sendHello();   // инициатор шлёт HELLO1 сразу после установления TCP
}

void Peer::sendHello()
{
    const QByteArray nameB64 = m_identity->deviceName().toUtf8().toBase64();
    const QByteArray line = QByteArray(m_isInitiator ? "ORBIT-HELLO1 " : "ORBIT-HELLO2 ")
        + m_identity->staticPublicKey().toHex() + ' '
        + m_ephemeralPublic.toHex() + ' '
        + m_identity->deviceId().toUtf8() + ' '
        + nameB64 + ' '
        + m_identity->deviceKind().toUtf8() + ' '
        + m_identity->deviceModel().toUtf8().toBase64() + '\n';
    m_ownHello = line;      // в запись попадёт при разборе чужой строки — иначе у отвечающего порядок был бы обратным
    m_socket->write(line);
}

void Peer::onReadyRead()
{
    if (!m_channel.ready) {
        // до готовности канала буфер — открытый текст, строго одна строка HELLO
        m_plainBuffer.append(m_socket->readAll());
        if (m_plainBuffer.size() > MAX_HELLO_LEN) { emit handshakeFailed(QStringLiteral("слишком длинное приветствие")); close(); return; }
        const int nl = m_plainBuffer.indexOf('\n');
        if (nl < 0) return;
        const QByteArray line = m_plainBuffer.left(nl);
        m_plainBuffer.remove(0, nl + 1);
        tryParseHello(line);
        if (!m_channel.ready) return;            // ошибка уже отправлена сигналом внутри tryParseHello
        if (!m_plainBuffer.isEmpty()) {           // к зашифрованному потоку могли успеть прилипнуть байты
            m_cipherBuffer.append(m_plainBuffer);
            m_plainBuffer.clear();
        }
    } else {
        m_cipherBuffer.append(m_socket->readAll());
    }

    m_lastRxMs = QDateTime::currentMSecsSinceEpoch();
    QList<QByteArray> lines;
    if (!CryptoUtils::open(m_channel, m_cipherBuffer, lines)) {
        qWarning() << "Peer: канал повреждён, разрыв" << addressString();
        close();
        return;
    }
    for (const QByteArray &line : lines) {
        bool ok = false;
        const QJsonObject msg = Protocol::decode(line, &ok);
        if (!ok) continue;
        const QString type = msg.value(QStringLiteral("type")).toString();
        if (type == QLatin1String(Protocol::TYPE_PING)) { QJsonObject pong; pong[QStringLiteral("type")] = QString::fromLatin1(Protocol::TYPE_PONG); send(pong); continue; }
        if (type == QLatin1String(Protocol::TYPE_PONG)) continue;
        emit messageReceived(msg);
    }
}

void Peer::tryParseHello(const QByteArray &line)
{
    const QByteArray expectedTag = m_isInitiator ? "ORBIT-HELLO2 " : "ORBIT-HELLO1 ";
    if (!line.startsWith(expectedTag)) {
        emit handshakeFailed(QStringLiteral("неожиданное приветствие (не Orbit на этом порту?)"));
        close();
        return;
    }
    const QList<QByteArray> parts = line.mid(expectedTag.size()).split(' ');
    // шестая часть — модель устройства (старые версии её не шлют)
    if (parts.size() != 5 && parts.size() != 6) { emit handshakeFailed(QStringLiteral("повреждённое приветствие")); close(); return; }

    const QByteArray peerStatic = QByteArray::fromHex(parts[0]);
    const QByteArray peerEphemeral = QByteArray::fromHex(parts[1]);
    if (peerStatic.size() != CryptoUtils::KEY_SIZE || peerEphemeral.size() != CryptoUtils::KEY_SIZE) {
        emit handshakeFailed(QStringLiteral("неверная длина ключа")); close(); return;
    }
    m_peerStaticPublic = peerStatic;
    m_peerEphemeralPublic = peerEphemeral;
    m_peerDeviceId = QString::fromUtf8(parts[2]);
    m_peerDeviceName = QString::fromUtf8(QByteArray::fromBase64(parts[3])).left(40);
    m_peerDeviceKind = QString::fromUtf8(parts[4]);
    // поля приветствия попадают в интерфейс и в файлы: допускается только ожидаемый вид (идентификатор — буквы, цифры и дефис; вид — из известных)
    static const QRegularExpression idRe(QStringLiteral("^[0-9A-Za-z-]{8,64}$"));
    if (!idRe.match(m_peerDeviceId).hasMatch()) { emit handshakeFailed(QStringLiteral("некорректный идентификатор устройства")); close(); return; }
    static const QStringList kinds{QStringLiteral("phone"), QStringLiteral("tablet"), QStringLiteral("laptop"), QStringLiteral("desktop"), QStringLiteral("server")};
    if (!kinds.contains(m_peerDeviceKind)) m_peerDeviceKind = QStringLiteral("desktop");
    if (parts.size() == 6) m_peerDeviceModel = QString::fromUtf8(QByteArray::fromBase64(parts[5])).left(60);
    // запись для вывода ключа: HELLO1 затем HELLO2 у обеих сторон (раньше у каждой стороны был свой порядок — HMAC не сходился)
    m_transcript = m_isInitiator ? (m_ownHello + line + '\n') : (line + '\n' + m_ownHello);

    if (m_peerDeviceId.isEmpty() || m_peerDeviceId == m_identity->deviceId()) {
        emit handshakeFailed(QStringLiteral("некорректный идентификатор устройства")); close(); return;
    }

    // dh_ee — эфемерный × эфемерный; dh_es/dh_se — доказательство владения статическим ключом каждой стороной
    const QByteArray dhEE = CryptoUtils::x25519(m_ephemeralSecret, m_peerEphemeralPublic);
    const QByteArray dhES = m_isInitiator
        ? CryptoUtils::x25519(m_ephemeralSecret, m_peerStaticPublic)
        : CryptoUtils::x25519(m_identity->staticSecretKey(), m_peerEphemeralPublic);
    const QByteArray dhSE = m_isInitiator
        ? CryptoUtils::x25519(m_identity->staticSecretKey(), m_peerEphemeralPublic)
        : CryptoUtils::x25519(m_ephemeralSecret, m_peerStaticPublic);

    if (!CryptoUtils::deriveChannel(m_channel, dhEE, dhES, dhSE, m_transcript, m_isInitiator)) {
        emit handshakeFailed(QStringLiteral("не удалось вывести ключ канала")); close(); return;
    }
    finishHandshake();
}

void Peer::finishHandshake()
{
    m_handshakeComplete = true;
    m_lastRxMs = QDateTime::currentMSecsSinceEpoch();
    m_handshakeTimeout.stop();
    m_pingTimer.start();
    emit handshakeDone();
}

void Peer::onPing()
{
    if (!m_channel.ready) return;
    if (QDateTime::currentMSecsSinceEpoch() - m_lastRxMs > DEAD_AFTER_MS) {      // сеть пропала без разрыва (Wi-Fi выключили, ноутбук уснул)
        qWarning() << "Peer: нет ответа, соединение закрыто" << addressString();
        m_socket->abort();      // не вежливое закрытие: собеседник всё равно не услышит
        return;
    }
    QJsonObject ping;
    ping[QStringLiteral("type")] = QString::fromLatin1(Protocol::TYPE_PING);
    send(ping);
}

void Peer::send(const QJsonObject &message)
{
    if (!m_channel.ready || m_socket->state() != QAbstractSocket::ConnectedState) return;
    m_socket->write(CryptoUtils::seal(m_channel, Protocol::encode(message)));
}

void Peer::onSocketError()
{
    if (!m_handshakeComplete) emit handshakeFailed(m_socket->errorString());
}

void Peer::close(const QString &reason)
{
    if (m_channel.ready && m_socket->state() == QAbstractSocket::ConnectedState && !reason.isEmpty()) {
        QJsonObject bye;
        bye[QStringLiteral("type")] = QString::fromLatin1(Protocol::TYPE_BYE);
        bye[QStringLiteral("reason")] = reason;
        send(bye);
        m_socket->flush();
    }
    m_socket->disconnectFromHost();
}

QString Peer::addressString() const
{
    // IPv4 без «::ffff:» (двойной стек сокета отдаёт адрес в отображённом виде) и без порта — это то, что показывается в списке
    QHostAddress a = m_socket->peerAddress();
    bool ok = false;
    const quint32 v4 = a.toIPv4Address(&ok);
    return ok ? QHostAddress(v4).toString() : a.toString();
}
