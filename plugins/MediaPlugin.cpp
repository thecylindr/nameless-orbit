#include "MediaPlugin.h"
#include "../platform/PlatformBridge.h"
#include "../core/PeerManager.h"

#ifdef ORBIT_DESKTOP_LINUX
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#endif

namespace {
constexpr int POLL_INTERVAL_MS = 2000;
}

MediaPlugin::MediaPlugin(PeerManager *manager, QObject *parent)
    : PluginBase(manager, QStringLiteral("media"), parent), m_pollTimer(new QTimer(this))
{
    m_pollTimer->setInterval(POLL_INTERVAL_MS);
    connect(m_pollTimer, &QTimer::timeout, this, &MediaPlugin::refreshLocal);
    connect(this, &PluginBase::enabledChanged, this, [this]() { if (isEnabled()) { refreshLocal(); m_pollTimer->start(); } else m_pollTimer->stop(); });
    // новому подключению сразу отдаётся текущее состояние: раньше оно уходило только при смене трека, и открытая позже страница была пустой
    connect(manager, &PeerManager::deviceConnected, this, [this](const QString &deviceId, const QString &) {
        QTimer::singleShot(900, this, [this, deviceId]() { sendStateTo(deviceId); });
    });
}

void MediaPlugin::sendStateTo(const QString &deviceId)
{
    if (!isEnabled() || (m_title.isEmpty() && !m_playing)) return;
    QJsonObject payload;
    payload[QStringLiteral("title")] = m_title;
    payload[QStringLiteral("artist")] = m_artist;
    payload[QStringLiteral("playing")] = m_playing;
    m_manager->sendPlugin(deviceId, name(), payload);
}

#ifdef ORBIT_DESKTOP_LINUX
namespace {
// первый работающий плеер MPRIS в сессии; NameLess UI (Quickshell) решает ту же задачу через встроенный тип Mpris —
// здесь то же самое, но вручную через DBus, так как это отдельное Qt-приложение без Quickshell
QString firstMprisService()
{
    QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
    if (!bus) return {};
    for (const QString &svc : bus->registeredServiceNames().value())
        if (svc.startsWith(QStringLiteral("org.mpris.MediaPlayer2."))) return svc;
    return {};
}
}
#endif

void MediaPlugin::refreshLocal()
{
#ifdef ORBIT_DESKTOP_LINUX
    const QString service = firstMprisService();
    if (service.isEmpty()) { if (!m_title.isEmpty() || m_playing) { m_title.clear(); m_artist.clear(); m_playing = false; emit localTrackChanged(); } return; }

    QDBusInterface props(service, QStringLiteral("/org/mpris/MediaPlayer2"), QStringLiteral("org.freedesktop.DBus.Properties"), QDBusConnection::sessionBus());
    const QVariantMap meta = props.call(QStringLiteral("Get"), QStringLiteral("org.mpris.MediaPlayer2.Player"), QStringLiteral("Metadata")).arguments().value(0).toMap();
    const QString status = props.call(QStringLiteral("Get"), QStringLiteral("org.mpris.MediaPlayer2.Player"), QStringLiteral("PlaybackStatus")).arguments().value(0).toString();

    const QString title = meta.value(QStringLiteral("xesam:title")).toString();
    const QStringList artists = meta.value(QStringLiteral("xesam:artist")).toStringList();
    const bool playing = (status == QLatin1String("Playing"));

    if (title != m_title || artists.join(QStringLiteral(", ")) != m_artist || playing != m_playing) {
        m_title = title; m_artist = artists.join(QStringLiteral(", ")); m_playing = playing;
        emit localTrackChanged();
        if (isEnabled()) {
            QJsonObject payload;
            payload[QStringLiteral("title")] = m_title;
            payload[QStringLiteral("artist")] = m_artist;
            payload[QStringLiteral("playing")] = m_playing;
            m_manager->broadcastPlugin(name(), payload, name());
        }
    }
#elif defined(Q_OS_ANDROID)
    // у телефона нет MPRIS: название трека недоступно без особого разрешения на уведомления, зато видно, играет ли звук
    const bool playing = PlatformBridge::musicActive();
    const QString title = playing ? QStringLiteral("Играет на телефоне") : QString();
    if (title != m_title || playing != m_playing) {
        m_title = title; m_artist.clear(); m_playing = playing;
        emit localTrackChanged();
        if (isEnabled()) {
            QJsonObject payload;
            payload[QStringLiteral("title")] = m_title;
            payload[QStringLiteral("artist")] = m_artist;
            payload[QStringLiteral("playing")] = m_playing;
            m_manager->broadcastPlugin(name(), payload, name());
        }
    }
#endif
}

void MediaPlugin::applyLocalCommand(const QString &action)
{
#ifdef ORBIT_DESKTOP_LINUX
    const QString service = firstMprisService();
    if (service.isEmpty()) return;
    QDBusInterface player(service, QStringLiteral("/org/mpris/MediaPlayer2"), QStringLiteral("org.mpris.MediaPlayer2.Player"), QDBusConnection::sessionBus());
    if (action == QLatin1String("playpause")) player.call(QStringLiteral("PlayPause"));
    else if (action == QLatin1String("next")) player.call(QStringLiteral("Next"));
    else if (action == QLatin1String("previous")) player.call(QStringLiteral("Previous"));
#elif defined(Q_OS_ANDROID)
    PlatformBridge::mediaKey(action);
#else
    Q_UNUSED(action);
#endif
}

void MediaPlugin::sendCommand(const QString &deviceId, const QString &action)
{
    static const QSet<QString> allowed = {QStringLiteral("playpause"), QStringLiteral("next"), QStringLiteral("previous")};
    if (!allowed.contains(action)) return;
    QJsonObject payload;
    payload[QStringLiteral("cmd")] = action;
    m_manager->sendPlugin(deviceId, name(), payload);
}

void MediaPlugin::handleMessage(const QString &deviceId, const QJsonObject &payload)
{
    if (!isEnabled()) return;
    if (payload.contains(QStringLiteral("cmd"))) {
        applyLocalCommand(payload.value(QStringLiteral("cmd")).toString());
        return;
    }
    emit remoteTrackChanged(deviceId, payload.value(QStringLiteral("title")).toString(),
                            payload.value(QStringLiteral("artist")).toString(), payload.value(QStringLiteral("playing")).toBool());
}
