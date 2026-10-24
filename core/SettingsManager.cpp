#include "SettingsManager.h"
#include "../platform/PlatformBridge.h"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QCoreApplication>

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
bool SettingsManager::batteryUnrestricted() const { return PlatformBridge::batteryUnrestricted(); }
void SettingsManager::requestBatteryUnrestricted() const { PlatformBridge::requestBatteryUnrestricted(); }
void SettingsManager::moveToBack() const { PlatformBridge::moveToBack(); }
void SettingsManager::testNotification() const { PlatformBridge::postNotification(QStringLiteral("NameLess Orbit"), QStringLiteral("Так выглядят уведомления от других устройств"), true); }

QString SettingsManager::configureCodexNotifications()
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    QFile script(QStringLiteral(":/orbit/packaging/configure-codex.py"));
    if (!script.open(QIODevice::ReadOnly)) return QStringLiteral("Не найден установщик уведомлений Codex");
    QProcess process;
    process.start(QStringLiteral("python3"), {QStringLiteral("-"), QCoreApplication::applicationFilePath()});
    if (!process.waitForStarted(2000)) return QStringLiteral("Для подключения Codex нужен Python 3.11 или новее");
    process.write(script.readAll());
    process.closeWriteChannel();
    if (!process.waitForFinished(3000)) { process.kill(); process.waitForFinished(); return QStringLiteral("Настройка Codex не завершилась вовремя"); }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return QString::fromUtf8(process.readAllStandardError()).trimmed().left(500);
    return QString();
#else
    return QStringLiteral("Автоматическое подключение Codex доступно на Linux");
#endif
}
