#include "BatteryPlugin.h"
#include "../core/PeerManager.h"
#include "../platform/PlatformBridge.h"

#ifdef ORBIT_DESKTOP_LINUX
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusConnection>
#include <QDir>
#include <QFile>
#endif

namespace {
constexpr int POLL_INTERVAL_MS = 30000;

#ifdef ORBIT_DESKTOP_LINUX
// яркость встроенного экрана: первая подсветка в /sys/class/backlight (у ПК её нет — -1)
int backlightPercent()
{
    const QDir dir(QStringLiteral("/sys/class/backlight"));
    for (const QString &n : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile cur(dir.filePath(n) + QStringLiteral("/brightness")), max(dir.filePath(n) + QStringLiteral("/max_brightness"));
        if (!cur.open(QIODevice::ReadOnly) || !max.open(QIODevice::ReadOnly)) continue;
        const int c = cur.readAll().trimmed().toInt(), m = max.readAll().trimmed().toInt();
        if (m > 0) return qBound(0, qRound(c * 100.0 / m), 100);
    }
    return -1;
}
#endif
}

BatteryPlugin::BatteryPlugin(PeerManager *manager, QObject *parent)
    : PluginBase(manager, QStringLiteral("battery"), parent)
{
    m_pollTimer.setInterval(POLL_INTERVAL_MS);
    connect(&m_pollTimer, &QTimer::timeout, this, [this]() { refreshLocal(); broadcastLocal(); });
    connect(this, &PluginBase::enabledChanged, this, [this]() {
        if (isEnabled()) { refreshLocal(); m_pollTimer.start(); broadcastLocal(); }
        else m_pollTimer.stop();
    });
}

bool BatteryPlugin::isAvailable() const
{
#ifdef ORBIT_DESKTOP_LINUX
    return QDBusConnection::systemBus().isConnected();
#else
    return PlatformBridge::battery().percent >= 0;      // Android, Windows; у настольного ПК без батареи — нет
#endif
}

void BatteryPlugin::refreshLocal()
{
#ifdef ORBIT_DESKTOP_LINUX
    // UPower: DisplayDevice агрегирует все батареи системы в одно значение (так же делает Quickshell-UPower в NameLess UI)
    QDBusInterface dev(QStringLiteral("org.freedesktop.UPower"), QStringLiteral("/org/freedesktop/UPower/devices/DisplayDevice"),
                       QStringLiteral("org.freedesktop.DBus.Properties"), QDBusConnection::systemBus());
    m_brightness = backlightPercent();
    if (dev.isValid()) {
        // у ПК без батареи сводное устройство есть, но IsPresent = false и заряд 0 — тогда заряд не показывается
        QDBusReply<QVariant> present = dev.call(QStringLiteral("Get"), QStringLiteral("org.freedesktop.UPower.Device"), QStringLiteral("IsPresent"));
        QDBusReply<QVariant> pct = dev.call(QStringLiteral("Get"), QStringLiteral("org.freedesktop.UPower.Device"), QStringLiteral("Percentage"));
        QDBusReply<QVariant> state = dev.call(QStringLiteral("Get"), QStringLiteral("org.freedesktop.UPower.Device"), QStringLiteral("State"));
        const bool has = present.isValid() && present.value().toBool();
        m_percent = has && pct.isValid() ? int(pct.value().toDouble()) : -1;
        m_charging = has && state.isValid() && (state.value().toUInt() == 1 || state.value().toUInt() == 4);   // 1 — заряжается, 4 — заряжена
    }
    emit localChanged();
#else
    const PlatformBridge::BatteryState b = PlatformBridge::battery();
    m_percent = b.percent;
    m_charging = b.charging;
    m_brightness = PlatformBridge::screenBrightness();
    emit localChanged();
#endif
}

void BatteryPlugin::broadcastLocal()
{
    if (!isEnabled() || (m_percent < 0 && m_brightness < 0)) return;
    QJsonObject payload;
    payload[QStringLiteral("percent")] = m_percent;            // -1 — батареи нет (ПК): у получателя заряд не показывается
    payload[QStringLiteral("charging")] = m_charging;
    payload[QStringLiteral("brightness")] = m_brightness;
    m_manager->broadcastPlugin(name(), payload, name());
}

void BatteryPlugin::handleMessage(const QString &deviceId, const QJsonObject &payload)
{
    if (!isEnabled()) return;
    State st;
    st.percent = payload.value(QStringLiteral("percent")).toInt(-1);
    st.charging = payload.value(QStringLiteral("charging")).toBool();
    st.brightness = payload.value(QStringLiteral("brightness")).toInt(-1);
    m_remote.insert(deviceId, st);
    emit remoteChanged(deviceId);
}
