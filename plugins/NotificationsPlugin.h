#ifndef NOTIFICATIONSPLUGIN_H
#define NOTIFICATIONSPLUGIN_H

#include "PluginBase.h"

// Уведомления между устройствами. Пока — ручная пересылка (relay): то, что приложение явно попросило
// отправить, показывается на другом устройстве как всплывающая карточка; обратное направление (снимать
// системные уведомления этого компьютера и слать их телефону само по себе) — отдельная, более инвазивная
// задача (перехват шины уведомлений рабочего стола), оставлена на следующий шаг и описана в README.
class NotificationsPlugin : public PluginBase
{
    Q_OBJECT

public:
    explicit NotificationsPlugin(PeerManager *manager, QObject *parent = nullptr);

    Q_INVOKABLE void sendTo(const QString &deviceId, const QString &title, const QString &body, const QString &appName = QString());
    Q_INVOKABLE void broadcast(const QString &title, const QString &body, const QString &appName = QString());

    void handleMessage(const QString &deviceId, const QJsonObject &payload) override;

signals:
    void notificationReceived(const QString &deviceId, const QString &fromName, const QString &title, const QString &body, const QString &appName);
};

#endif // NOTIFICATIONSPLUGIN_H
