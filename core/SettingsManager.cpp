#include "SettingsManager.h"
#include "../platform/PlatformBridge.h"
#include <QStandardPaths>
#include <QDir>

namespace {
QString settingsPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/settings.ini");
}
}

SettingsManager::SettingsManager(QObject *parent)
    : QObject(parent), m_settings(settingsPath(), QSettings::IniFormat)
{
}

bool SettingsManager::notificationsAllowed() const { return PlatformBridge::notificationsAllowed(); }
void SettingsManager::requestNotificationPermission() const { PlatformBridge::requestNotificationPermission(); }
void SettingsManager::openNotificationSettings() const { PlatformBridge::openNotificationSettings(); }
void SettingsManager::openBatterySettings() const { PlatformBridge::openBatterySettings(); }
void SettingsManager::testNotification() const { PlatformBridge::postNotification(QStringLiteral("NameLess Orbit"), QStringLiteral("Так выглядят уведомления от других устройств"), true); }
