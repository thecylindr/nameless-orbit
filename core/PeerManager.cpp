#include "PeerManager.h"
#include "Identity.h"
#include "TrustStore.h"
#include "Peer.h"
#include "Protocol.h"
#include "plugins/PluginBase.h"
#include <QJsonArray>
#include <QVariantMap>
#include <QHostAddress>
#include <QDateTime>
#include <QDebug>

namespace {
constexpr qint64 PAIRING_TTL_MS = 90000;      // запрос на сопряжение без ответа живёт полторы минуты
constexpr int MAX_PENDING = 3;                // сколько незнакомых устройств могут ждать решения одновременно
constexpr int MAINTAIN_MS = 5000;
constexpr qint64 BASE_RETRY_MS = 6000;
constexpr qint64 MAX_RETRY_MS = 60000;
constexpr qint64 SECOND_SIDE_WAIT_MS = 8000;  // «старшая» сторона подключается позже: сначала шанс у «младшей»
inline qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
}

PeerManager::PeerManager(Identity *identity, TrustStore *trustStore, QObject *parent)
    : QObject(parent), m_identity(identity), m_trustStore(trustStore), m_discovery(identity, this)
{
    connect(&m_discovery, &Discovery::deviceFound, this, [this](const DiscoveredDevice &d) {
        m_nearby.insert(d.deviceId, d);
        // адрес и модель запоминаются сразу: ими можно воспользоваться и тогда, когда маяк уже не слышен
        if (m_trustStore->isPaired(d.deviceId)) m_trustStore->setLastContact(d.deviceId, d.model, d.address.toString(), d.port);
        emit nearbyChanged();
        autoReconnect();
    });
    connect(&m_discovery, &Discovery::deviceLost, this, [this](const QString &id) {
        if (m_nearby.remove(id) > 0) emit nearbyChanged();
    });
    connect(&m_discovery, &Discovery::activeChanged, this, &PeerManager::discoveryActiveChanged);

    connect(&m_server, &QTcpServer::newConnection, this, &PeerManager::onNewConnection);
    if (!m_server.listen(QHostAddress::Any, Discovery::PEER_PORT))
        qWarning() << "PeerManager: не удалось слушать порт" << Discovery::PEER_PORT << m_server.errorString();
    emit listeningChanged();

    m_maintainTimer.setInterval(MAINTAIN_MS);
    connect(&m_maintainTimer, &QTimer::timeout, this, [this]() {
        // просроченные запросы на сопряжение закрываются сами — иначе забытое окно висело бы вечно
        const qint64 now = nowMs();
        const QStringList ids = m_pending.keys();
        for (const QString &id : ids) {
            const PendingPairing pp = m_pending.value(id);
            if (now - pp.startedMs > PAIRING_TTL_MS) {
                emit pairingDeclined(id, pp.name);
                rejectPairing(id);
            }
        }
        autoReconnect();
    });
    m_maintainTimer.start();
}

bool PeerManager::discoveryActive() const { return m_discovery.active(); }
void PeerManager::setDiscoveryActive(bool on) { m_discovery.setActive(on); if (on) autoReconnect(); }
void PeerManager::refresh() { m_discovery.probeNow(); autoReconnect(); }

qint64 PeerManager::backlog(const QString &deviceId) const
{
    const Peer *p = m_peers.value(deviceId, nullptr);
    return p ? p->pendingBytes() : 0;
}

QVariantList PeerManager::nearbyDevices() const
{
    QVariantList out;
    QSet<QString> shown;
    for (const DiscoveredDevice &d : m_nearby) {
        // та же машина под другим deviceId (прежняя установка, второй запуск, имя «localhost» до загрузки настроек) не должна дублироваться в списке:
        // непарное устройство с адресом уже сопряжённого, которое слышно или на связи, скрывается
        if (!m_trustStore->isPaired(d.deviceId)) {
            bool dup = false;
            for (const DiscoveredDevice &o : m_nearby)
                if (o.deviceId != d.deviceId && o.address == d.address && m_trustStore->isPaired(o.deviceId)) { dup = true; break; }
            if (!dup) for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it)
                if (it.key() != d.deviceId && it.value()->addressString() == d.address.toString()) { dup = true; break; }
            if (dup) continue;
        }
        QVariantMap m;
        m[QStringLiteral("deviceId")] = d.deviceId;
        m[QStringLiteral("name")] = d.name;
        m[QStringLiteral("kind")] = d.kind;
        m[QStringLiteral("model")] = d.model;
        m[QStringLiteral("address")] = d.address.toString();
        m[QStringLiteral("paired")] = m_trustStore->isPaired(d.deviceId);
        m[QStringLiteral("connected")] = m_peers.contains(d.deviceId);
        m[QStringLiteral("confirmed")] = m_remoteCapabilities.contains(d.deviceId);   // второе устройство тоже считает нас доверенными
        m[QStringLiteral("connecting")] = m_pending.contains(d.deviceId) || m_dialing.values().contains(d.deviceId);
        m[QStringLiteral("seen")] = true;
        out.append(m);
        shown.insert(d.deviceId);
    }
    // сопряжённые, которых сейчас не слышно: остаются в списке (на связи или нет), по ним можно подключиться по последнему адресу
    for (const QVariant &v : m_trustStore->devicesForQml()) {
        const QVariantMap t = v.toMap();
        const QString id = t.value(QStringLiteral("deviceId")).toString();
        if (shown.contains(id)) continue;
        QVariantMap m;
        m[QStringLiteral("deviceId")] = id;
        m[QStringLiteral("name")] = t.value(QStringLiteral("name"));
        m[QStringLiteral("kind")] = t.value(QStringLiteral("kind"));
        m[QStringLiteral("model")] = t.value(QStringLiteral("model"));
        m[QStringLiteral("address")] = m_peers.contains(id) ? m_peers.value(id)->addressString() : t.value(QStringLiteral("address"));
        m[QStringLiteral("paired")] = true;
        m[QStringLiteral("connected")] = m_peers.contains(id);
        m[QStringLiteral("confirmed")] = m_remoteCapabilities.contains(id);
        m[QStringLiteral("connecting")] = m_pending.contains(id) || m_dialing.values().contains(id);
        m[QStringLiteral("seen")] = false;
        out.append(m);
    }
    return out;
}

QVariantList PeerManager::connectedDevicesForQml() const
{
    QVariantList out;
    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        QVariantMap m;
        m[QStringLiteral("deviceId")] = it.key();
        m[QStringLiteral("name")] = it.value()->deviceName();
        m[QStringLiteral("kind")] = it.value()->deviceKind();
        m[QStringLiteral("model")] = it.value()->deviceModel();
        m[QStringLiteral("address")] = it.value()->addressString();
        QVariantList caps;
        for (const QString &c : m_remoteCapabilities.value(it.key())) caps.append(c);
        m[QStringLiteral("remoteCapabilities")] = caps;
        m[QStringLiteral("confirmed")] = m_remoteCapabilities.contains(it.key());
        out.append(m);
    }
    return out;
}

QVariantList PeerManager::pairingRequestsForQml() const
{
    QVariantList out;
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it) {
        QVariantMap m;
        m[QStringLiteral("deviceId")] = it.key();
        m[QStringLiteral("name")] = it->name;
        m[QStringLiteral("kind")] = it->kind;
        m[QStringLiteral("model")] = it->peer->deviceModel();
        m[QStringLiteral("address")] = it->peer->addressString();
        m[QStringLiteral("code")] = it->code;
        m[QStringLiteral("outgoing")] = it->outgoing;
        m[QStringLiteral("remoteCommands")] = it->remoteCommands;            // быстрые команды второго устройства (вы сможете их запускать)
        m[QStringLiteral("allowCommands")] = it->allowCommands;
        m[QStringLiteral("localAccepted")] = it->localAccepted;      // ответил я — ждём вторую сторону
        m[QStringLiteral("remoteAccepted")] = it->remoteAccepted;    // вторая сторона уже согласна
        m[QStringLiteral("secondsLeft")] = int(qMax<qint64>(0, (PAIRING_TTL_MS - (nowMs() - it->startedMs)) / 1000));
        out.append(m);
    }
    return out;
}

Peer *PeerManager::trustedPeer(const QString &deviceId) const { return m_peers.value(deviceId, nullptr); }

void PeerManager::registerPlugin(const QString &name, PluginBase *plugin) { m_plugins.insert(name, plugin); }

void PeerManager::sendPlugin(const QString &deviceId, const QString &plugin, const QJsonObject &payload)
{
    Peer *p = m_peers.value(deviceId);
    if (!p) return;
    p->send(Protocol::pluginEnvelope(plugin, payload));
}

void PeerManager::broadcastPlugin(const QString &plugin, const QJsonObject &payload, const QString &requiredCapability)
{
    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        if (requiredCapability.isEmpty() || m_trustStore->pluginAllowed(it.key(), requiredCapability))
            it.value()->send(Protocol::pluginEnvelope(plugin, payload));
    }
}

// ---------- исходящие подключения ----------

void PeerManager::dial(const QString &deviceId, const QHostAddress &addr, quint16 port)
{
    Peer *peer = Peer::connectTo(addr, port, m_identity, this);
    if (!deviceId.isEmpty()) m_dialing.insert(peer, deviceId);
    wireUpPeer(peer);
    emit nearbyChanged();
}

void PeerManager::connectToDevice(const QString &deviceId)
{
    if (m_peers.contains(deviceId) || m_pending.contains(deviceId) || m_dialing.values().contains(deviceId)) return;
    const auto it = m_nearby.constFind(deviceId);
    if (it != m_nearby.constEnd()) { dial(deviceId, it->address, it->port); return; }
    // маяк не слышен (другая сеть, VPN, устройство только что проснулось) — пробуем последний известный адрес
    const TrustedDevice t = m_trustStore->device(deviceId);
    if (!t.lastAddress.isEmpty()) dial(deviceId, QHostAddress(t.lastAddress), t.lastPort ? t.lastPort : Discovery::PEER_PORT);
}

void PeerManager::connectByAddress(const QString &host, int port)
{
    const QHostAddress addr(host);
    if (addr.isNull() || port <= 0 || port > 65535) return;
    dial(QString(), addr, quint16(port));
}

// Сопряжённые устройства подключаются сами: оба конца по умолчанию пытаются одновременно, поэтому «младший» по deviceId
// идёт сразу, а «старший» — только если за несколько секунд соединения так и нет; задержка при неудачах растёт до минуты
void PeerManager::autoReconnect()
{
    if (!m_discovery.active()) return;
    const qint64 now = nowMs();
    const QString me = m_identity->deviceId();
    for (const QVariant &v : m_trustStore->devicesForQml()) {
        const QString id = v.toMap().value(QStringLiteral("deviceId")).toString();
        if (m_peers.contains(id) || m_pending.contains(id) || m_dialing.values().contains(id)) { m_wantedSinceMs.remove(id); continue; }
        if (!m_nearby.contains(id) && v.toMap().value(QStringLiteral("address")).toString().isEmpty()) continue;
        if (!m_wantedSinceMs.contains(id)) m_wantedSinceMs.insert(id, now);
        if (me > id && now - m_wantedSinceMs.value(id) < SECOND_SIDE_WAIT_MS) continue;
        if (now < m_nextAttemptMs.value(id, 0)) continue;
        const int n = m_attempts.value(id, 0);
        m_nextAttemptMs.insert(id, now + qMin(MAX_RETRY_MS, BASE_RETRY_MS << qMin(n, 4)));
        m_attempts.insert(id, n + 1);
        connectToDevice(id);
    }
}

void PeerManager::onNewConnection()
{
    while (m_server.hasPendingConnections()) {
        QTcpSocket *socket = m_server.nextPendingConnection();
        // незавершённых рукопожатий с чужих адресов немного: так одно устройство не забьёт приём тысячами пустых соединений
        int open = 0, sameAddr = 0;
        for (const Peer *p : findChildren<Peer *>()) {
            if (p->isInitiator() || p->authenticated()) continue;
            ++open;
            if (p->addressString() == QHostAddress(socket->peerAddress().toIPv4Address()).toString()) ++sameAddr;
        }
        if (open >= 12 || sameAddr >= 3) { socket->abort(); socket->deleteLater(); continue; }
        Peer *peer = Peer::fromIncoming(socket, m_identity, this);
        wireUpPeer(peer);
    }
}

void PeerManager::wireUpPeer(Peer *peer)
{
    connect(peer, &Peer::handshakeDone, this, [this, peer]() { onPeerHandshakeDone(peer); });
    connect(peer, &Peer::handshakeFailed, this, [this, peer](const QString &reason) { onPeerFailed(peer, reason); });
    connect(peer, &Peer::messageReceived, this, [this, peer](const QJsonObject &msg) { onPeerMessage(peer, msg); });
    connect(peer, &Peer::disconnected, this, [this, peer]() { onPeerDisconnected(peer); });
}

void PeerManager::onPeerFailed(Peer *peer, const QString &reason)
{
    qWarning() << "PeerManager: рукопожатие не удалось" << peer->addressString() << reason;
    m_dialing.remove(peer);
    emit nearbyChanged();
    peer->deleteLater();
}

// из двух встречных соединений с одним устройством остаётся то, которое открыл «младший» по deviceId: правило одно на оба
// конца, поэтому оба закрывают одно и то же соединение, а не каждый «чужое»
bool PeerManager::prefers(Peer *p) const
{
    const QString me = m_identity->deviceId(), other = p->deviceId();
    return p->isInitiator() ? (me < other) : (other < me);
}

void PeerManager::onPeerHandshakeDone(Peer *peer)
{
    const QString id = peer->deviceId();
    m_dialing.remove(peer);

    if (Peer *old = m_peers.value(id, nullptr)) {
        if (old == peer) return;
        if (prefers(old) || !prefers(peer)) { peer->close(); peer->deleteLater(); return; }
        m_peers.remove(id);                 // новое лучше: старое закрываем тихо, без сигнала «отключено»
        m_remoteCapabilities.remove(id);
        old->close();
    }
    if (m_pending.contains(id)) { peer->close(); peer->deleteLater(); return; }

    if (m_trustStore->isPaired(id)) {
        if (!m_trustStore->verifyKey(id, peer->peerPublicKey())) {
            // ключ не совпал с запомненным — либо у собеседника переустановлен Orbit, либо это подмена.
            // Соединение НЕ продолжается молча: пользователь должен явно решить, обновлять ли доверие
            const TrustedDevice known = m_trustStore->device(id);
            emit keyMismatch(id, known.name, QStringLiteral("…"), QStringLiteral("новый ключ"));
            peer->close(QStringLiteral("ключ устройства изменился"));
            peer->deleteLater();
            return;
        }
        finalizeConnected(peer);
        return;
    }

    // устройство незнакомо — запрос на сопряжение; чужих запросов не больше нескольких, остальные отклоняются сразу.
    // Отклонённое устройство 5 минут не может спрашивать снова, и всего принимается не больше четырёх запросов в минуту: перебрать код сверки не выйдет
    {
        const qint64 t = nowMs();
        if (m_blockedUntil.value(id) > t) { peer->close(QStringLiteral("запросы временно отклонены")); peer->deleteLater(); return; }
        m_pairStarts.removeIf([t](qint64 s) { return t - s > 60000; });
        if (m_pairStarts.size() >= 4) { peer->close(QStringLiteral("слишком много запросов")); peer->deleteLater(); return; }
        m_pairStarts.append(t);
    }
    if (m_pending.size() >= MAX_PENDING) { peer->close(QStringLiteral("слишком много запросов")); peer->deleteLater(); return; }
    // код сверки выводится из общего секрета канала и потому одинаковый на обоих экранах (раньше у каждой стороны был код
    // ключа собеседника — экраны показывали разные числа, сверять было нечего)
    PendingPairing pp{peer, peer->deviceName(), peer->deviceKind(), peer->sasCode(), peer->isInitiator()};
    pp.startedMs = nowMs();
    m_pending.insert(id, pp);
    // знакомство: наши быстрые команды уходят второму устройству до доверия — по ним оно покажет, что будет запускать у нас
    if (m_introProvider) {
        QJsonObject intro;
        intro[QStringLiteral("type")] = QString::fromLatin1(Protocol::TYPE_INTRO);
        QJsonArray list;
        for (const QVariant &v : m_introProvider()) {
            const QVariantMap c = v.toMap();
            QJsonObject o;
            o[QStringLiteral("name")] = c.value(QStringLiteral("name")).toString().left(60);
            o[QStringLiteral("icon")] = c.value(QStringLiteral("icon")).toString().left(24);
            list.append(o);
        }
        intro[QStringLiteral("commands")] = list;
        peer->send(intro);
    }
    emit pairingRequestsChanged();
    emit pairingRequested(id, peer->deviceName(), peer->deviceKind(), pp.code);
}

void PeerManager::acceptPairing(const QString &deviceId)
{
    const auto it = m_pending.find(deviceId);
    if (it == m_pending.end() || it->localAccepted) return;
    it->localAccepted = true;
    QJsonObject o;
    o[QStringLiteral("type")] = QString::fromLatin1(Protocol::TYPE_PAIR);
    o[QStringLiteral("ok")] = true;
    it->peer->send(o);
    if (it->remoteAccepted) completePairing(deviceId);
    else emit pairingRequestsChanged();                 // «ждём подтверждения на другом устройстве»
}

void PeerManager::rejectPairing(const QString &deviceId)
{
    const auto it = m_pending.constFind(deviceId);
    if (it == m_pending.constEnd()) return;
    Peer *peer = it->peer;
    const bool incoming = !it->outgoing;          // блокируются только чужие запросы: свою попытку человек может повторить сразу
    QJsonObject o;
    o[QStringLiteral("type")] = QString::fromLatin1(Protocol::TYPE_PAIR);
    o[QStringLiteral("ok")] = false;
    peer->send(o);
    if (incoming) m_blockedUntil.insert(deviceId, nowMs() + 5 * 60 * 1000);
    peer->close(QStringLiteral("сопряжение отклонено"));
    peer->deleteLater();
    m_pending.remove(deviceId);
    emit pairingRequestsChanged();
}

void PeerManager::setPairingCommandsAllowed(const QString &deviceId, bool allowed)
{
    const auto it = m_pending.find(deviceId);
    if (it == m_pending.end()) return;
    it->allowCommands = allowed;
    emit pairingRequestsChanged();
}

void PeerManager::completePairing(const QString &deviceId)
{
    const auto it = m_pending.constFind(deviceId);
    if (it == m_pending.constEnd()) return;
    Peer *peer = it->peer;
    const bool allowCommands = it->allowCommands;
    // то же устройство под новым deviceId (переустановка приложения стирает ключи): старая запись с той же моделью и видом, которой сейчас нет на связи,
    // передаёт доверие и разрешения новой и убирается из списка — иначе приходилось заново включать «Демонстрацию экрана» и доверие, а старое «устройство» висело дублем
    QString oldId;
    for (const QVariant &v : m_trustStore->devicesForQml()) {
        const QString id = v.toMap().value(QStringLiteral("deviceId")).toString();
        if (id == deviceId || m_peers.contains(id)) continue;
        const TrustedDevice t = m_trustStore->device(id);
        if (!t.model.isEmpty() && t.model == peer->deviceModel() && t.kind == peer->deviceKind()) oldId = id;
    }
    m_trustStore->addOrUpdate(deviceId, peer->peerPublicKey(), peer->deviceName(), peer->deviceKind());
    if (!oldId.isEmpty()) {
        const TrustedDevice old = m_trustStore->device(oldId);
        m_trustStore->setTrustLevel(deviceId, old.trust);
        for (const QString &pl : {QStringLiteral("clipboard"), QStringLiteral("notifications"), QStringLiteral("battery"), QStringLiteral("find"), QStringLiteral("media"),
                                   QStringLiteral("runcommand"), QStringLiteral("remoteDesktop"), QStringLiteral("share")})
            m_trustStore->setPluginAllowed(deviceId, pl, old.allowedPlugins.contains(pl));
        m_trustStore->forget(oldId);
    }
    if (!allowCommands) m_trustStore->setPluginAllowed(deviceId, QStringLiteral("runcommand"), false);       // человек не согласился на запуск команд
    m_pending.remove(deviceId);
    emit pairingRequestsChanged();
    finalizeConnected(peer);
}

void PeerManager::finalizeConnected(Peer *peer)
{
    const QString id = peer->deviceId();
    m_peers.insert(id, peer);
    m_attempts.remove(id);
    m_nextAttemptMs.remove(id);
    m_wantedSinceMs.remove(id);
    // порт запоминаем только если подключались мы сами (у входящего порт отправителя случайный) — иначе стандартный
    m_trustStore->setLastContact(id, peer->deviceModel(), peer->addressString(), peer->isInitiator() ? peer->remotePort() : Discovery::PEER_PORT);
    sendCapabilities(peer);
    emit connectedChanged();
    emit nearbyChanged();
    emit deviceConnected(id, peer->deviceName());
}

QStringList PeerManager::enabledCapabilities() const
{
    QStringList out;
    for (auto it = m_plugins.constBegin(); it != m_plugins.constEnd(); ++it)
        if (it.value()->isEnabled()) out.append(it.key());
    return out;
}

void PeerManager::sendCapabilities(Peer *peer)
{
    QJsonObject o;
    o[QStringLiteral("type")] = QString::fromLatin1(Protocol::TYPE_CAPABILITIES);
    QJsonArray arr;
    for (const QString &c : enabledCapabilities()) arr.append(c);
    o[QStringLiteral("plugins")] = arr;
    peer->send(o);
}

void PeerManager::onPeerMessage(Peer *peer, const QJsonObject &msg)
{
    const QString id = peer->deviceId();
    const QString type = msg.value(QStringLiteral("type")).toString();

    if (type == QLatin1String(Protocol::TYPE_INTRO)) {
        const auto it = m_pending.find(id);
        if (it == m_pending.end() || it->peer != peer) return;
        QVariantList list;
        const QJsonArray arr = msg.value(QStringLiteral("commands")).toArray();
        for (int i = 0; i < arr.size() && i < 24; ++i) {
            const QJsonObject o = arr.at(i).toObject();
            QVariantMap m;
            m[QStringLiteral("name")] = o.value(QStringLiteral("name")).toString().left(60);
            m[QStringLiteral("icon")] = o.value(QStringLiteral("icon")).toString().left(24);
            list.append(m);
        }
        it->remoteCommands = list;
        emit pairingRequestsChanged();
        return;
    }
    if (type == QLatin1String(Protocol::TYPE_PAIR)) {
        const auto it = m_pending.find(id);
        if (it == m_pending.end() || it->peer != peer) return;
        if (!msg.value(QStringLiteral("ok")).toBool()) {
            const QString name = it->name;
            emit pairingDeclined(id, name);
            peer->close();
            peer->deleteLater();
            m_pending.remove(id);
            emit pairingRequestsChanged();
            return;
        }
        it->remoteAccepted = true;
        if (it->localAccepted) completePairing(id);
        else emit pairingRequestsChanged();                 // «другое устройство уже согласно — сверьте код и подтвердите»
        return;
    }
    if (type == QLatin1String(Protocol::TYPE_CAPABILITIES)) {
        // собеседник сам считает нас доверенными (уже сопряжены с его стороны) — это и есть его согласие
        const auto pit = m_pending.find(id);
        if (pit != m_pending.end() && pit->peer == peer) {
            pit->remoteAccepted = true;
            if (pit->localAccepted) { completePairing(id); } else emit pairingRequestsChanged();
        }
        if (!m_peers.contains(id)) return;      // возможности запоминаются только у подключённых; от ждущих — позже
        QStringList caps;
        for (const QJsonValue &v : msg.value(QStringLiteral("plugins")).toArray()) caps.append(v.toString());
        m_remoteCapabilities.insert(id, caps);
        emit connectedChanged();
        emit nearbyChanged();
        return;
    }
    if (type == QLatin1String(Protocol::TYPE_BYE)) return;   // disconnected придёт следом от сокета

    if (type == QLatin1String(Protocol::TYPE_PLUGIN)) {
        // плагиновый трафик принимается только от уже доверенного (в TrustStore) и подключённого устройства —
        // пир в m_pending (сопряжение ещё не подтверждено человеком) сюда не доходит
        if (m_peers.value(id) != peer || !m_trustStore->verifyKey(id, peer->peerPublicKey())) return;
        const QString plugin = msg.value(QStringLiteral("plugin")).toString();
        const QJsonObject payload = msg.value(QStringLiteral("payload")).toObject();
        PluginBase *p = m_plugins.value(plugin);
        // разрешение могли отозвать уже после подключения; ответы на мои собственные запросы разрешения не требуют
        if ((!p || p->needsPermission(payload)) && !m_trustStore->pluginAllowed(id, plugin)) {
            // запрос молча пропадал, и другая сторона вечно «ждала»: теперь ей сообщается, что разрешения нет (само сообщение об отказе разрешения не требует)
            if (p) sendPlugin(id, plugin, QJsonObject{{QStringLiteral("kind"), QStringLiteral("permissionDenied")}, {QStringLiteral("of"), payload.value(QStringLiteral("kind")).toString()}});
            return;
        }
        emit pluginMessage(id, plugin, payload);
        if (p) p->handleMessage(id, payload);
    }
}

void PeerManager::onPeerDisconnected(Peer *peer)
{
    const QString id = peer->deviceId();
    m_dialing.remove(peer);
    bool changed = false;
    if (m_peers.value(id) == peer) { m_peers.remove(id); m_remoteCapabilities.remove(id); changed = true; emit deviceDisconnected(id); }
    if (m_pending.value(id).peer == peer) { m_pending.remove(id); emit pairingRequestsChanged(); }
    if (changed) { emit connectedChanged(); emit nearbyChanged(); }
    peer->deleteLater();
}

void PeerManager::forgetDevice(const QString &deviceId)
{
    disconnectDevice(deviceId);
    m_trustStore->forget(deviceId);
    m_attempts.remove(deviceId);
    m_nextAttemptMs.remove(deviceId);
    emit nearbyChanged();
}

void PeerManager::disconnectDevice(const QString &deviceId)
{
    // ручное отключение: автоподключение к этому устройству приостанавливается на пару минут, чтобы оно не вернулось сразу
    m_nextAttemptMs.insert(deviceId, nowMs() + 120000);
    if (Peer *p = m_peers.value(deviceId)) { p->close(QStringLiteral("отключено пользователем")); }
}
