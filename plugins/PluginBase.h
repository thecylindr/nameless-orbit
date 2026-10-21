#ifndef PLUGINBASE_H
#define PLUGINBASE_H

#include <QObject>
#include <QJsonObject>
#include <QString>

class PeerManager;

// Общий интерфейс плагина Orbit: получает именованные сообщения ("plugin":"<name>") от PeerManager и
// умеет сказать, включён ли он сейчас (выключенный плагин не попадает в список возможностей, которые
// устройство рассылает при подключении — собеседник просто не увидит соответствующую кнопку).
// Разрешение «можно ли ЭТОМУ устройству пользоваться плагином» хранит TrustStore и проверяет PeerManager
// до того, как сообщение дойдёт сюда — плагину достаточно считать, что раз сообщение пришло, значит можно.
class PluginBase : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ isEnabled WRITE setEnabled NOTIFY enabledChanged)

public:
    explicit PluginBase(PeerManager *manager, QString name, QObject *parent = nullptr)
        : QObject(parent), m_manager(manager), m_name(std::move(name)) {}

    QString name() const { return m_name; }
    virtual bool isEnabled() const { return m_enabled; }
    virtual void setEnabled(bool on) { if (on != m_enabled) { m_enabled = on; emit enabledChanged(); } }

    // входящее сообщение плагина от устройства deviceId; вызывается из PeerManager::onPeerMessage
    virtual void handleMessage(const QString &deviceId, const QJsonObject &payload) = 0;

    // Нужно ли разрешение отправителя на это сообщение. Разрешения защищают от НАЧАЛА действия («пришли файл», «покажи экран»,
    // «выполни команду»); ответы на то, что начал я сам (согласие на мой файл, кадры экрана, который я попросил), идут без них —
    // иначе при выключенном разрешении ответ молча терялся бы и у отправителя вечно висело «ждём ответа»
    virtual bool needsPermission(const QJsonObject &payload) const { Q_UNUSED(payload); return true; }

signals:
    void enabledChanged();

protected:
    PeerManager *m_manager;

private:
    QString m_name;
    bool m_enabled = false;
};

#endif // PLUGINBASE_H
