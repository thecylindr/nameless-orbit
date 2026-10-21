#include "NotificationsPlugin.h"
#include "../core/PeerManager.h"
#include <QRegularExpression>

namespace {
// текст уведомления: без управляющих символов, пробелы схлопнуты, края обрезаны
QString clean(const QString &t, int limit)
{
    QString out = t;
    out.replace(QRegularExpression(QStringLiteral("[\\x00-\\x08\\x0B-\\x1F\\x7F]")), QStringLiteral(" "));
    out.replace(QRegularExpression(QStringLiteral("[ \\t]+")), QStringLiteral(" "));
    return out.trimmed().left(limit);
}
}

NotificationsPlugin::NotificationsPlugin(PeerManager *manager, QObject *parent)
    : PluginBase(manager, QStringLiteral("notifications"), parent)
{
}

void NotificationsPlugin::sendTo(const QString &deviceId, const QString &title, const QString &body, const QString &appName)
{
    if (!isEnabled()) return;
    const QString t = clean(title, 200), b = clean(body, 2000);
    if (t.isEmpty() && b.isEmpty()) return;               // пустое уведомление не отправляется
    QJsonObject payload;
    payload[QStringLiteral("title")] = t;
    payload[QStringLiteral("body")] = b;
    payload[QStringLiteral("app")] = clean(appName, 60);
    m_manager->sendPlugin(deviceId, name(), payload);
}

void NotificationsPlugin::broadcast(const QString &title, const QString &body, const QString &appName)
{
    if (!isEnabled()) return;
    const QString t = clean(title, 200), b = clean(body, 2000);
    if (t.isEmpty() && b.isEmpty()) return;
    QJsonObject payload;
    payload[QStringLiteral("title")] = t;
    payload[QStringLiteral("body")] = b;
    payload[QStringLiteral("app")] = clean(appName, 60);
    m_manager->broadcastPlugin(name(), payload, name());
}

void NotificationsPlugin::handleMessage(const QString &deviceId, const QJsonObject &payload)
{
    if (!isEnabled()) return;
    QString t = clean(payload.value(QStringLiteral("title")).toString(), 200);
    QString b = clean(payload.value(QStringLiteral("body")).toString(), 2000);
    const QString app = clean(payload.value(QStringLiteral("app")).toString(), 60);
    if (t.isEmpty() && b.isEmpty()) return;               // присланное пустым не показывается ни у кого
    if (t.isEmpty()) t = app.isEmpty() ? QStringLiteral("NameLess Orbit") : app;
    emit notificationReceived(deviceId, m_manager->trustStore()->device(deviceId).name, t, b, app);
}
