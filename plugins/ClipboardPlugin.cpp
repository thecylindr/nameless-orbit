#include "ClipboardPlugin.h"
#include "../core/PeerManager.h"
#include <QGuiApplication>
#include <QClipboard>

ClipboardPlugin::ClipboardPlugin(PeerManager *manager, QObject *parent)
    : PluginBase(manager, QStringLiteral("clipboard"), parent)
{
}

void ClipboardPlugin::sendClipboardTo(const QString &deviceId)
{
    if (!isEnabled()) return;
    const QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty()) return;
    QJsonObject payload;
    payload[QStringLiteral("text")] = text.left(100000);   // с запасом, но не безгранично — чужой текст в памяти
    m_manager->sendPlugin(deviceId, name(), payload);
}

void ClipboardPlugin::handleMessage(const QString &deviceId, const QJsonObject &payload)
{
    if (!isEnabled()) return;
    const QString text = payload.value(QStringLiteral("text")).toString();
    if (text.isEmpty()) return;
    QGuiApplication::clipboard()->setText(text);
    emit textReceived(deviceId, m_manager->trustStore()->device(deviceId).name, text);
}
