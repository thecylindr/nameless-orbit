#include "RunCommandPlugin.h"
#include "../core/PeerManager.h"
#include "../core/CommandStore.h"
#include "../platform/PlatformBridge.h"
#ifndef Q_OS_ANDROID
#include <QProcess>
#endif
#include <QUuid>
#include <QVariantMap>
#include <QJsonArray>

RunCommandPlugin::RunCommandPlugin(PeerManager *manager, CommandStore *store, QObject *parent)
    : PluginBase(manager, QStringLiteral("runcommand"), parent), m_store(store)
{
    // свой список изменился — его получают все подключённые устройства (кому разрешено; остальные сообщение отбросят)
    connect(store, &CommandStore::changed, this, [this]() {
        for (const QVariant &v : m_manager->connectedDevicesForQml())
            sendAvailableCommands(v.toMap().value(QStringLiteral("deviceId")).toString());
    });
}

void RunCommandPlugin::requestCommands(const QString &deviceId)
{
    QJsonObject payload;
    payload[QStringLiteral("kind")] = QStringLiteral("listRequest");
    m_manager->sendPlugin(deviceId, name(), payload);
}

void RunCommandPlugin::sendAvailableCommands(const QString &deviceId)
{
    if (!isEnabled()) return;
    QJsonObject payload;
    QJsonArray arr;
    for (const QVariant &v : m_store->commandsForQml()) {
        const QVariantMap m = v.toMap();
        QJsonObject o;
        o[QStringLiteral("id")] = m.value(QStringLiteral("id")).toString();
        o[QStringLiteral("name")] = m.value(QStringLiteral("name")).toString();
        o[QStringLiteral("icon")] = m.value(QStringLiteral("icon")).toString();
        arr.append(o);
    }
    payload[QStringLiteral("list")] = arr;
    payload[QStringLiteral("kind")] = QStringLiteral("list");
    m_manager->sendPlugin(deviceId, name(), payload);
}

void RunCommandPlugin::request(const QString &deviceId, const QString &commandId)
{
    QJsonObject payload;
    payload[QStringLiteral("kind")] = QStringLiteral("run");
    payload[QStringLiteral("id")] = commandId;
    m_manager->sendPlugin(deviceId, name(), payload);
}

void RunCommandPlugin::execute(const QString &commandId)
{
    const OrbitCommand *c = m_store->find(commandId);
#ifdef Q_OS_ANDROID
    // на Android QProcess нет, зато есть встроенные действия: команда вида «orbit:flashlight»
    const bool ok = c && c->command.startsWith(QLatin1String("orbit:")) && PlatformBridge::runAction(c->command.mid(6));
#else
    const bool ok = c != nullptr;
    if (ok) QProcess::startDetached(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), c->command});
#endif
    emit executed(commandId, ok);
}

void RunCommandPlugin::confirmPending(const QString &requestId, bool run)
{
    const auto it = m_pendingConfirm.constFind(requestId);
    if (it == m_pendingConfirm.constEnd()) return;
    if (run) execute(it->commandId);
    m_pendingConfirm.remove(requestId);
}

void RunCommandPlugin::handleMessage(const QString &deviceId, const QJsonObject &payload)
{
    if (!isEnabled()) return;
    const QString kind = payload.value(QStringLiteral("kind")).toString();

    if (kind == QLatin1String("listRequest")) { sendAvailableCommands(deviceId); return; }

    if (kind == QLatin1String("list")) {
        QVariantList commands;
        for (const QJsonValue &v : payload.value(QStringLiteral("list")).toArray()) {
            const QJsonObject o = v.toObject();
            QVariantMap m;
            m[QStringLiteral("id")] = o.value(QStringLiteral("id")).toString();
            m[QStringLiteral("name")] = o.value(QStringLiteral("name")).toString();
            m[QStringLiteral("icon")] = o.value(QStringLiteral("icon")).toString();
            commands.append(m);
        }
        m_remote.insert(deviceId, commands);
        emit remoteCommandsUpdated(deviceId, commands);
        return;
    }

    if (kind == QLatin1String("run")) {
        const QString commandId = payload.value(QStringLiteral("id")).toString();
        const OrbitCommand *c = m_store->find(commandId);
        if (!c) return;
        // быстрые команды выполняются сразу: доверие к ним (и к тому, что они делают) даётся один раз — при сопряжении, где виден их список
        execute(commandId);
    }
}
