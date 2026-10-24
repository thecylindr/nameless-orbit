#ifndef BATTERYPLUGIN_H
#define BATTERYPLUGIN_H

#include "PluginBase.h"
#include <QHash>
#include <QTimer>

// Заряд батареи и яркость экрана — свои наружу и чужие на экран. На Linux заряд — UPower через DBus (только если батарея
// действительно есть: у настольного ПК UPower отдаёт пустое сводное устройство с 0 %), яркость — /sys/class/backlight;
// на Android — системные BATTERY_CHANGED и настройка яркости. Нет батареи — percent = -1 и заряд не показывается.
class BatteryPlugin : public PluginBase
{
    Q_OBJECT
    Q_PROPERTY(bool available READ isAvailable CONSTANT)
    Q_PROPERTY(int percent READ percent NOTIFY localChanged)
    Q_PROPERTY(bool charging READ charging NOTIFY localChanged)
    Q_PROPERTY(int brightness READ brightness NOTIFY localChanged)

public:
    explicit BatteryPlugin(PeerManager *manager, QObject *parent = nullptr);

    bool isAvailable() const;
    int percent() const { return m_percent; }
    bool charging() const { return m_charging; }
    int brightness() const { return m_brightness; }

    // у устройства есть батарея (оно прислало заряд >= 0)
    Q_INVOKABLE bool hasRemote(const QString &deviceId) const { return m_remote.value(deviceId).percent >= 0; }
    Q_INVOKABLE int remotePercent(const QString &deviceId) const { return m_remote.value(deviceId).percent; }
    Q_INVOKABLE bool remoteCharging(const QString &deviceId) const { return m_remote.value(deviceId).charging; }
    Q_INVOKABLE int remoteBrightness(const QString &deviceId) const { return m_remote.value(deviceId).brightness; }      // 0..100, -1 — неизвестна

    void handleMessage(const QString &deviceId, const QJsonObject &payload) override;

public slots:
    void refreshLocal();
    void broadcastLocal();

signals:
    void localChanged();
    void remoteChanged(const QString &deviceId);

private:
    struct State { int percent = -1; bool charging = false; int brightness = -1; };
    int m_percent = -1;
    bool m_charging = false;
    int m_brightness = -1;
    QTimer m_pollTimer;
    QHash<QString, State> m_remote;
};

#endif // BATTERYPLUGIN_H
