#ifndef BATTERYPLUGIN_H
#define BATTERYPLUGIN_H

#include "PluginBase.h"
#include <QHash>
#include <QTimer>

// Заряд батареи — свой наружу и чужой на экран. На Linux источник — UPower через DBus (как и остальная
// сборка NameLess UI); на других системах класс компилируется, но isAvailable() == false, пока не появится
// свой источник (Windows — GetSystemPowerStatus, macOS — IOKit power sources; оставлено на следующий шаг).
class BatteryPlugin : public PluginBase
{
    Q_OBJECT
    Q_PROPERTY(bool available READ isAvailable CONSTANT)
    Q_PROPERTY(int percent READ percent NOTIFY localChanged)
    Q_PROPERTY(bool charging READ charging NOTIFY localChanged)

public:
    explicit BatteryPlugin(PeerManager *manager, QObject *parent = nullptr);

    bool isAvailable() const;
    int percent() const { return m_percent; }
    bool charging() const { return m_charging; }

    Q_INVOKABLE int remotePercent(const QString &deviceId) const { return m_remote.value(deviceId).first; }
    Q_INVOKABLE bool hasRemote(const QString &deviceId) const { return m_remote.contains(deviceId); }
    Q_INVOKABLE bool remoteCharging(const QString &deviceId) const { return m_remote.value(deviceId).second; }

    void handleMessage(const QString &deviceId, const QJsonObject &payload) override;

public slots:
    void refreshLocal();
    void broadcastLocal();

signals:
    void localChanged();
    void remoteChanged(const QString &deviceId);

private:
    int m_percent = -1;
    bool m_charging = false;
    QTimer m_pollTimer;
    QHash<QString, QPair<int, bool>> m_remote;
};

#endif // BATTERYPLUGIN_H
