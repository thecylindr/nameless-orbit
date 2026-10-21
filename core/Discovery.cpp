#include "Discovery.h"
#include "../platform/PlatformBridge.h"
#include "Identity.h"
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QDebug>
#include <QSet>

namespace {
constexpr int BEACON_INTERVAL_MS = 4000;
constexpr int PRUNE_INTERVAL_MS = 1000;
constexpr int DEVICE_TTL_MS = 11000;        // пропустили 2 маяка подряд — считаем устройство ушедшим из сети
constexpr int MAX_PACKET_SIZE = 512;
}

Discovery::Discovery(Identity *identity, QObject *parent) : QObject(parent), m_identity(identity)
{
    m_beaconTimer.setInterval(BEACON_INTERVAL_MS);
    connect(&m_beaconTimer, &QTimer::timeout, this, &Discovery::sendBeacon);

    m_pruneTimer.setInterval(PRUNE_INTERVAL_MS);
    connect(&m_pruneTimer, &QTimer::timeout, this, &Discovery::pruneExpired);

    connect(&m_socket, &QUdpSocket::readyRead, this, &Discovery::readPendingDatagrams);
}

void Discovery::setActive(bool on)
{
    if (on == m_active) return;
    m_active = on;
    if (on) {
        PlatformBridge::acquireMulticastLock();     // Android: без блокировки Wi-Fi не пропускает широковещательные пакеты
        if (m_socket.state() == QAbstractSocket::UnconnectedState)
            m_socket.bind(DISCOVERY_PORT, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
        m_beaconTimer.start();
        m_pruneTimer.start();
        probeNow();
    } else {
        m_beaconTimer.stop();
        m_pruneTimer.stop();
        m_socket.close();
        m_seen.clear();
    }
    emit activeChanged();
}

void Discovery::probeNow()
{
    if (m_active) sendBeacon();
}

void Discovery::sendBeacon()
{
    // открытый текст: имя, вид, deviceId (для показа до сопряжения) и порт, на котором этот узел слушает Peer-соединения.
    // Публичный ключ сюда не кладём — он передаётся только в рукопожатии Peer, после явного выбора «подключиться»
    QJsonObject o;
    o[QStringLiteral("magic")] = QString::fromLatin1(MAGIC);
    o[QStringLiteral("id")] = m_identity->deviceId();
    o[QStringLiteral("name")] = m_identity->deviceName();
    o[QStringLiteral("kind")] = m_identity->deviceKind();
    o[QStringLiteral("model")] = m_identity->deviceModel();
    o[QStringLiteral("port")] = PEER_PORT;
    const QByteArray packet = QJsonDocument(o).toJson(QJsonDocument::Compact);
    // маяк уходит на широковещательный адрес КАЖДОЙ подсети (192.168.3.255 и т. п.), а не только на 255.255.255.255:
    // при включённом VPN с туннелем (политика маршрутов, как у Incy) общий адрес идёт по маршруту «по умолчанию» —
    // то есть в туннель, и до устройств в локальной сети не доходит, а адрес подсети остаётся на своём интерфейсе.
    // Туннели и точка-точка (tun, VPN) пропускаются
    QSet<QHostAddress> sent;
    for (const QNetworkInterface &ni : QNetworkInterface::allInterfaces()) {
        const auto f = ni.flags();
        if (!(f & QNetworkInterface::IsUp) || !(f & QNetworkInterface::IsRunning) || !(f & QNetworkInterface::CanBroadcast)
            || (f & QNetworkInterface::IsLoopBack) || (f & QNetworkInterface::IsPointToPoint)) continue;
        for (const QNetworkAddressEntry &e : ni.addressEntries()) {
            const QHostAddress b = e.broadcast();
            if (e.ip().protocol() != QAbstractSocket::IPv4Protocol || b.isNull() || sent.contains(b)) continue;
            sent.insert(b);
            m_socket.writeDatagram(packet, b, DISCOVERY_PORT);
        }
    }
    // запасной вариант — общий широковещательный адрес (сети без перечисленных выше записей)
    m_socket.writeDatagram(packet, QHostAddress::Broadcast, DISCOVERY_PORT);
}

void Discovery::readPendingDatagrams()
{
    while (m_socket.hasPendingDatagrams()) {
        const QNetworkDatagram dg = m_socket.receiveDatagram(MAX_PACKET_SIZE);
        if (!dg.isValid()) continue;
        const QJsonObject o = QJsonDocument::fromJson(dg.data()).object();
        if (o.value(QStringLiteral("magic")).toString() != QLatin1String(MAGIC)) continue;

        const QString id = o.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || id == m_identity->deviceId()) continue;   // свой же маяк, пришедший эхом по broadcast

        DiscoveredDevice d;
        d.deviceId = id;
        d.name = o.value(QStringLiteral("name")).toString().left(40);
        d.kind = o.value(QStringLiteral("kind")).toString();
        d.model = o.value(QStringLiteral("model")).toString().left(60);
        d.address = dg.senderAddress();
        { bool ok = false; const quint32 v4 = d.address.toIPv4Address(&ok); if (ok) d.address = QHostAddress(v4); }   // без «::ffff:»
        d.port = quint16(o.value(QStringLiteral("port")).toInt(PEER_PORT));
        d.lastSeenAt = QDateTime::currentMSecsSinceEpoch();

        const bool isNew = !m_seen.contains(id);
        m_seen.insert(id, d);
        if (isNew) emit deviceFound(d);
    }
}

void Discovery::pruneExpired()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_seen.begin(); it != m_seen.end();) {
        if (now - it->lastSeenAt > DEVICE_TTL_MS) {
            const QString id = it.key();
            it = m_seen.erase(it);
            emit deviceLost(id);
        } else {
            ++it;
        }
    }
}
