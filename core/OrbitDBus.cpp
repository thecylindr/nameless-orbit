#include "OrbitDBus.h"
#include "../plugins/MediaPlugin.h"
#include "PeerManager.h"
#include "Identity.h"
#include "../plugins/BatteryPlugin.h"
#include "../plugins/FindDevicePlugin.h"
#include "../plugins/ClipboardPlugin.h"
#include "../plugins/NotificationsPlugin.h"
#include "../plugins/SharePlugin.h"
#include "../plugins/RunCommandPlugin.h"
#include "../remote/EdgeCrossing.h"

#ifdef ORBIT_DESKTOP_LINUX
#include <QDBusConnection>
#include <QDBusMessage>
#endif
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

namespace {
constexpr auto kService = "org.glassrice.NamelessOrbit";
constexpr auto kPath = "/org/glassrice/NamelessOrbit";
}

OrbitDBus::OrbitDBus(PeerManager *pm, BatteryPlugin *battery, FindDevicePlugin *find, ClipboardPlugin *clipboard,
                     NotificationsPlugin *notifications, SharePlugin *share, RunCommandPlugin *runCommand, QObject *parent)
    : QObject(parent), m_pm(pm), m_battery(battery), m_find(find), m_clipboard(clipboard), m_notifications(notifications), m_share(share), m_run(runCommand)
{
    // изменения сыплются пачками (маяк, рукопожатие) — сигнал склеивается в один за 150 мс
    connect(pm, &PeerManager::nearbyChanged, this, &OrbitDBus::notifyChanged);
    connect(pm, &PeerManager::connectedChanged, this, &OrbitDBus::notifyChanged);
    connect(pm, &PeerManager::pairingRequestsChanged, this, &OrbitDBus::notifyChanged);
    connect(pm, &PeerManager::discoveryActiveChanged, this, &OrbitDBus::notifyChanged);
    connect(battery, &BatteryPlugin::remoteChanged, this, &OrbitDBus::notifyChanged);
    connect(share, &SharePlugin::transfersChanged, this, &OrbitDBus::notifyChanged);
    connect(runCommand, &RunCommandPlugin::remoteCommandsUpdated, this, &OrbitDBus::notifyChanged);
}

bool OrbitDBus::registerOnBus()
{
#ifndef ORBIT_DESKTOP_LINUX
    return false;       // D-Bus есть только в настольном Linux
#else
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return false;
    // повторный вызов (имя было занято) — объект уже на месте, остаётся занять имя
    if (bus.objectRegisteredAt(QString::fromLatin1(kPath)) != this &&
        !bus.registerObject(QString::fromLatin1(kPath), this, QDBusConnection::ExportAllSlots | QDBusConnection::ExportScriptableSignals))
        return false;
    return bus.registerService(QString::fromLatin1(kService));
#endif
}

void OrbitDBus::notifyChanged()
{
#ifdef ORBIT_DESKTOP_LINUX
    static bool pending = false;
    if (pending) return;
    pending = true;
    QTimer::singleShot(150, this, [this]() {
        pending = false;
        QDBusMessage sig = QDBusMessage::createSignal(QString::fromLatin1(kPath), QString::fromLatin1(kService), QStringLiteral("Changed"));
        QDBusConnection::sessionBus().send(sig);
    });
#endif
}

QByteArray OrbitDBus::statusJson() const
{
    QJsonObject o;
    o[QStringLiteral("discoveryActive")] = m_pm->discoveryActive();
    o[QStringLiteral("listening")] = m_pm->listening();
    o[QStringLiteral("deviceName")] = m_pm->identity()->deviceName();
    o[QStringLiteral("deviceId")] = m_pm->identity()->deviceId();
    o[QStringLiteral("nearby")] = QJsonArray::fromVariantList(m_pm->nearbyDevices());

    // подключённым добавляем заряд батареи (-1 — неизвестен)
    QJsonArray connected;
    for (const QVariant &v : m_pm->connectedDevicesForQml()) {
        QJsonObject d = QJsonObject::fromVariantMap(v.toMap());
        const QString id = d.value(QStringLiteral("deviceId")).toString();
        d[QStringLiteral("battery")] = m_battery->hasRemote(id) ? m_battery->remotePercent(id) : -1;
        d[QStringLiteral("charging")] = m_battery->remoteCharging(id);
        d[QStringLiteral("brightness")] = m_battery->remoteBrightness(id);       // яркость экрана, 0..100; -1 — неизвестна
        // что играет: обложка — data URL (JPEG) или ссылка; at — когда получено (позиция досчитывается от него)
        if (m_media) {
            const QVariantMap m = m_media->remoteMedia(id);
            if (!m.isEmpty() && !m.value(QStringLiteral("title")).toString().isEmpty()) {
                QJsonObject mo = QJsonObject::fromVariantMap(m);
                const QString art = m.value(QStringLiteral("art")).toString();
                mo[QStringLiteral("art")] = !art.isEmpty() ? QStringLiteral("data:image/jpeg;base64,") + art : m.value(QStringLiteral("artUrl")).toString();
                d[QStringLiteral("media")] = mo;
            }
        }
        d[QStringLiteral("commands")] = QJsonArray::fromVariantList(m_run->remoteCommands(id));      // быстрые команды этого устройства
        connected.append(d);
    }
    o[QStringLiteral("connected")] = connected;
    o[QStringLiteral("pairingRequests")] = QJsonArray::fromVariantList(m_pm->pairingRequestsForQml());
    o[QStringLiteral("transfers")] = QJsonArray::fromVariantList(m_share->transfers());
    o[QStringLiteral("crossing")] = m_crossing ? m_crossing->activeDeviceId() : QString();      // с кем сейчас общий курсор
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

void OrbitDBus::setMedia(MediaPlugin *media)
{
    m_media = media;
    connect(media, &MediaPlugin::remoteMediaChanged, this, &OrbitDBus::notifyChanged);
}

void OrbitDBus::MediaCommand(const QString &deviceId, const QString &action)
{
    if (m_media) m_media->sendCommand(deviceId, action);
}

void OrbitDBus::setCrossing(EdgeCrossing *crossing)
{
    m_crossing = crossing;
    connect(crossing, &EdgeCrossing::crossing, this, &OrbitDBus::Crossing);
    connect(crossing, &EdgeCrossing::activeChanged, this, &OrbitDBus::notifyChanged);
}

bool OrbitDBus::CrossTo(const QString &deviceId, const QString &side, double pos, const QString &screen)
{
    return m_crossing && m_crossing->crossTo(deviceId, side, pos, screen);
}

void OrbitDBus::CrossStop() { if (m_crossing) m_crossing->stop(); }

void OrbitDBus::Scan() { m_pm->refresh(); }
void OrbitDBus::SetDiscovery(bool on) { m_pm->setDiscoveryActive(on); }
void OrbitDBus::Connect(const QString &deviceId) { m_pm->connectToDevice(deviceId); }
void OrbitDBus::ConnectAddress(const QString &host, int port) { m_pm->connectByAddress(host, port); }
void OrbitDBus::Disconnect(const QString &deviceId) { m_pm->disconnectDevice(deviceId); }
void OrbitDBus::AcceptPairing(const QString &deviceId) { m_pm->acceptPairing(deviceId); }
void OrbitDBus::RejectPairing(const QString &deviceId) { m_pm->rejectPairing(deviceId); }
void OrbitDBus::Ring(const QString &deviceId) { m_find->ring(deviceId); }
void OrbitDBus::StopRing(const QString &deviceId) { m_find->stop(deviceId); }
void OrbitDBus::SendClipboard(const QString &deviceId) { m_clipboard->sendClipboardTo(deviceId); }
void OrbitDBus::RunCommand(const QString &deviceId, const QString &commandId) { m_run->request(deviceId, commandId); }
void OrbitDBus::SendFile(const QString &deviceId, const QString &path) { m_share->sendFile(deviceId, path); }
void OrbitDBus::NotifyReply(const QString &title, const QString &body)
{
    if (m_repliesAllowed && !m_repliesAllowed()) return;
    m_notifications->broadcast(title, body, QStringLiteral("Claude"));      // источник «Claude» позволяет принимающей стороне отфильтровать такие сообщения
}

void OrbitDBus::NotifyCodexReply(const QString &title, const QString &body)
{
    if (!m_repliesAllowed || !m_repliesAllowed()) return;
    if (!m_codexAllowed || !m_codexAllowed()) return;
    m_notifications->broadcast(title, body, QStringLiteral("Codex"));
}

void OrbitDBus::Notify(const QString &title, const QString &body, const QString &deviceId)
{
    if (deviceId.isEmpty()) m_notifications->broadcast(title, body, QStringLiteral("Orbit"));
    else m_notifications->sendTo(deviceId, title, body, QStringLiteral("Orbit"));
}
