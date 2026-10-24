#include "MediaPlugin.h"
#include "../platform/PlatformBridge.h"
#include "../core/PeerManager.h"
#include <QBuffer>
#include <QDateTime>
#include <QImage>
#include <QJsonDocument>
#include <QSet>
#include <QUrl>

#ifdef ORBIT_DESKTOP_LINUX
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusArgument>
#include <QDBusVariant>
#endif

namespace {
constexpr int POLL_INTERVAL_MS = 2000;
constexpr int RESYNC_MS = 15000;          // играет — позиция пересылается раз в 15 с (получатель между ними досчитывает сам)
inline qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

#ifdef ORBIT_DESKTOP_LINUX
// свойство DBus: Get отдаёт значение, завёрнутое в QDBusVariant
QVariant prop(QDBusInterface &props, const char *iface, const char *name)
{
    const QVariant v = props.call(QStringLiteral("Get"), QString::fromLatin1(iface), QString::fromLatin1(name)).arguments().value(0);
    return v.canConvert<QDBusVariant>() ? v.value<QDBusVariant>().variant() : v;
}

// играющий плеер MPRIS, иначе первый; NameLess UI (Quickshell) решает ту же задачу встроенным типом Mpris
QString pickMprisService()
{
    QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
    if (!bus) return {};
    QString first;
    for (const QString &svc : bus->registeredServiceNames().value()) {
        if (!svc.startsWith(QStringLiteral("org.mpris.MediaPlayer2."))) continue;
        if (first.isEmpty()) first = svc;
        QDBusInterface props(svc, QStringLiteral("/org/mpris/MediaPlayer2"), QStringLiteral("org.freedesktop.DBus.Properties"), QDBusConnection::sessionBus());
        if (prop(props, "org.mpris.MediaPlayer2.Player", "PlaybackStatus").toString() == QLatin1String("Playing")) return svc;
    }
    return first;
}

// обложка с диска — маленький JPEG в base64
QString artFromFile(const QString &path)
{
    QImage img(path);
    if (img.isNull()) return {};
    img = img.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray b;
    QBuffer buf(&b);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", 78);
    return QString::fromLatin1(b.toBase64());
}
#endif
}

MediaPlugin::MediaPlugin(PeerManager *manager, QObject *parent)
    : PluginBase(manager, QStringLiteral("media"), parent), m_pollTimer(new QTimer(this))
{
    m_pollTimer->setInterval(POLL_INTERVAL_MS);
    connect(m_pollTimer, &QTimer::timeout, this, &MediaPlugin::refreshLocal);
    connect(this, &PluginBase::enabledChanged, this, [this]() { if (isEnabled()) { refreshLocal(); m_pollTimer->start(); } else m_pollTimer->stop(); });
    // новому подключению сразу отдаётся текущее состояние (с обложкой): иначе открытая позже страница была пустой
    connect(manager, &PeerManager::deviceConnected, this, [this](const QString &deviceId, const QString &) {
        QTimer::singleShot(900, this, [this, deviceId]() { sendStateTo(deviceId, true); });
    });
    connect(manager, &PeerManager::deviceDisconnected, this, [this](const QString &id) { if (m_remote.remove(id)) emit remoteMediaChanged(id); });
}

bool MediaPlugin::access() const
{
#ifdef Q_OS_ANDROID
    return PlatformBridge::mediaAccess();
#else
    return true;
#endif
}

void MediaPlugin::openAccessSettings() const
{
#ifdef Q_OS_ANDROID
    PlatformBridge::openMediaAccess();
#endif
}

QJsonObject MediaPlugin::payload(bool withArt) const
{
    QJsonObject p;
    for (const char *k : {"title", "artist", "album", "app", "artKey", "artUrl"}) p[QString::fromLatin1(k)] = m_local.value(QString::fromLatin1(k)).toString();
    p[QStringLiteral("playing")] = m_local.value(QStringLiteral("playing")).toBool();
    p[QStringLiteral("pos")] = double(m_local.value(QStringLiteral("pos")).toLongLong());
    p[QStringLiteral("len")] = double(m_local.value(QStringLiteral("len")).toLongLong());
    if (withArt) p[QStringLiteral("art")] = m_local.value(QStringLiteral("art")).toString();
    return p;
}

void MediaPlugin::sendStateTo(const QString &deviceId, bool withArt)
{
    if (!isEnabled() || (trackTitle().isEmpty() && !playing())) return;
    m_manager->sendPlugin(deviceId, name(), payload(withArt));
}

void MediaPlugin::refreshLocal()
{
    QVariantMap s;
#ifdef ORBIT_DESKTOP_LINUX
    const QString service = pickMprisService();
    if (!service.isEmpty()) {
        QDBusInterface props(service, QStringLiteral("/org/mpris/MediaPlayer2"), QStringLiteral("org.freedesktop.DBus.Properties"), QDBusConnection::sessionBus());
        QVariantMap meta;
        const QVariant mv = prop(props, "org.mpris.MediaPlayer2.Player", "Metadata");
        if (mv.canConvert<QDBusArgument>()) mv.value<QDBusArgument>() >> meta; else meta = mv.toMap();
        s[QStringLiteral("title")] = meta.value(QStringLiteral("xesam:title")).toString();
        s[QStringLiteral("artist")] = meta.value(QStringLiteral("xesam:artist")).toStringList().join(QStringLiteral(", "));
        s[QStringLiteral("album")] = meta.value(QStringLiteral("xesam:album")).toString();
        s[QStringLiteral("len")] = meta.value(QStringLiteral("mpris:length")).toLongLong() / 1000;
        s[QStringLiteral("playing")] = prop(props, "org.mpris.MediaPlayer2.Player", "PlaybackStatus").toString() == QLatin1String("Playing");
        s[QStringLiteral("pos")] = prop(props, "org.mpris.MediaPlayer2.Player", "Position").toLongLong() / 1000;
        s[QStringLiteral("app")] = prop(props, "org.mpris.MediaPlayer2", "Identity").toString();
        const QString artUrl = meta.value(QStringLiteral("mpris:artUrl")).toString();
        s[QStringLiteral("artKey")] = service + QLatin1Char('|') + s.value(QStringLiteral("title")).toString() + QLatin1Char('|') + artUrl;
        if (artUrl.startsWith(QLatin1String("http"))) s[QStringLiteral("artUrl")] = artUrl;
        else if (s.value(QStringLiteral("artKey")) == m_local.value(QStringLiteral("artKey"))) s[QStringLiteral("art")] = m_local.value(QStringLiteral("art"));
        else if (artUrl.startsWith(QLatin1String("file://"))) s[QStringLiteral("art")] = artFromFile(QUrl(artUrl).toLocalFile());
    }
#elif defined(Q_OS_ANDROID)
    const QJsonObject j = QJsonDocument::fromJson(PlatformBridge::mediaJson()).object();
    if (j.value(QStringLiteral("access")).toBool() && !j.value(QStringLiteral("title")).toString().isEmpty()) {
        s = j.toVariantMap();
        s.remove(QStringLiteral("access"));
        s[QStringLiteral("pos")] = qint64(j.value(QStringLiteral("pos")).toDouble());
        s[QStringLiteral("len")] = qint64(j.value(QStringLiteral("len")).toDouble());
    } else if (PlatformBridge::musicActive()) {
        // без доступа к медиасессиям видно только, что звук идёт
        s[QStringLiteral("title")] = QStringLiteral("Играет на телефоне");
        s[QStringLiteral("playing")] = true;
    }
#endif
    // рассылка: смена трека или состояния — сразу (обложка — только при смене трека); играет — позиция раз в 15 с
    const QString key = s.value(QStringLiteral("title")).toString() + QLatin1Char('|') + s.value(QStringLiteral("artist")).toString() + QLatin1Char('|')
                        + (s.value(QStringLiteral("playing")).toBool() ? QLatin1Char('1') : QLatin1Char('0'));
    const bool artChanged = s.value(QStringLiteral("artKey")) != m_local.value(QStringLiteral("artKey"));
    const bool changed = key != m_sentKey;
    m_local = s;
    if (changed) emit localTrackChanged();
    if (!isEnabled()) return;
    if (changed || artChanged || (playing() && nowMs() - m_sentAt > RESYNC_MS)) {
        if (s.isEmpty() && m_sentKey.isEmpty()) return;
        m_sentKey = key; m_sentAt = nowMs();
        m_manager->broadcastPlugin(name(), payload(artChanged), name());
    }
}

void MediaPlugin::applyLocalCommand(const QString &action)
{
#ifdef ORBIT_DESKTOP_LINUX
    const QString service = pickMprisService();
    if (service.isEmpty()) return;
    QDBusInterface player(service, QStringLiteral("/org/mpris/MediaPlayer2"), QStringLiteral("org.mpris.MediaPlayer2.Player"), QDBusConnection::sessionBus());
    if (action == QLatin1String("playpause")) player.call(QStringLiteral("PlayPause"));
    else if (action == QLatin1String("next")) player.call(QStringLiteral("Next"));
    else if (action == QLatin1String("previous")) player.call(QStringLiteral("Previous"));
#elif defined(Q_OS_ANDROID)
    if (!PlatformBridge::mediaCommand(action)) PlatformBridge::mediaKey(action);      // сессии нет — медиа-клавиша
#else
    Q_UNUSED(action);
#endif
    QTimer::singleShot(400, this, &MediaPlugin::refreshLocal);      // новое состояние — сразу, а не через 2 с
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
    QVariantMap m = payload.toVariantMap();
    // обложка приходит только при смене трека — у того же трека остаётся прежняя
    const QVariantMap prev = m_remote.value(deviceId);
    if (!payload.contains(QStringLiteral("art")) && prev.value(QStringLiteral("artKey")) == m.value(QStringLiteral("artKey"))) m[QStringLiteral("art")] = prev.value(QStringLiteral("art"));
    m[QStringLiteral("at")] = double(nowMs());
    m_remote.insert(deviceId, m);
    emit remoteMediaChanged(deviceId);
    emit remoteTrackChanged(deviceId, m.value(QStringLiteral("title")).toString(), m.value(QStringLiteral("artist")).toString(), m.value(QStringLiteral("playing")).toBool());
}
