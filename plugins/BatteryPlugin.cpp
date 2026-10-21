#include "BatteryPlugin.h"
#include "../core/PeerManager.h"
#include "../platform/PlatformBridge.h"

#ifdef ORBIT_DESKTOP_LINUX
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusConnection>
#endif

namespace {
constexpr int POLL_INTERVAL_MS = 30000;
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
    if (!dev.isValid()) return;
    QDBusReply<QVariant> pct = dev.call(QStringLiteral("Get"), QStringLiteral("org.freedesktop.UPower.Device"), QStringLiteral("Percentage"));
    QDBusReply<QVariant> state = dev.call(QStringLiteral("Get"), QStringLiteral("org.freedesktop.UPower.Device"), QStringLiteral("State"));
    if (pct.isValid()) m_percent = int(pct.value().toDouble());
    if (state.isValid()) m_charging = (state.value().toUInt() == 1);   // 1 = Charging (enum UPower.Device.State)
    emit localChanged();
#else
    const PlatformBridge::BatteryState b = PlatformBridge::battery();
    if (b.percent < 0) return;
    m_percent = b.percent;
    m_charging = b.charging;
    emit localChanged();
#endif
}

void BatteryPlugin::broadcastLocal()
{
    if (!isEnabled() || m_percent < 0) return;
    QJsonObject payload;
    payload[QStringLiteral("percent")] = m_percent;
    payload[QStringLiteral("charging")] = m_charging;
    m_manager->broadcastPlugin(name(), payload, name());
}

void BatteryPlugin::handleMessage(const QString &deviceId, const QJsonObject &payload)
{
    if (!isEnabled()) return;
    m_remote.insert(deviceId, {payload.value(QStringLiteral("percent")).toInt(-1), payload.value(QStringLiteral("charging")).toBool()});
    emit remoteChanged(deviceId);
}
