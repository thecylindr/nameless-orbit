#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QHash>
#include <QHostAddress>

class Identity;

// Маяк в локальной сети: каждое устройство с Orbit раз в несколько секунд рассылает широковещательный пакет
// со своим именем, видом и портом — так все находят друг друга без сервера и настройки адресов, как в
// KDE Connect / drones ServerDiscovery. В пакете нет ничего секретного (как и в самом обнаружении Bluetooth) —
// сопряжение и доверие начинаются только после TCP-рукопожатия в Peer.
struct DiscoveredDevice
{
    QString deviceId;
    QString name;
    QString kind;
    QString model;
    QHostAddress address;
    quint16 port = 0;
    qint64 lastSeenAt = 0;
};

class Discovery : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)

public:
    static constexpr quint16 DISCOVERY_PORT = 17893;     // свободный порт, не пересекается с drones (8378)
    static constexpr quint16 PEER_PORT = 17894;
    static constexpr char MAGIC[] = "NAMELESS-ORBIT-1";

    explicit Discovery(Identity *identity, QObject *parent = nullptr);

    bool active() const { return m_active; }
    void setActive(bool on);

public slots:
    void probeNow();     // внеочередная рассылка — сразу после запуска или по нажатию «Обновить»

signals:
    void activeChanged();
    void deviceFound(const DiscoveredDevice &device);
    void deviceLost(const QString &deviceId);

private slots:
    void readPendingDatagrams();
    void sendBeacon();
    void pruneExpired();

private:
    Identity *m_identity;
    QUdpSocket m_socket;
    QTimer m_beaconTimer;
    QTimer m_pruneTimer;
    QHash<QString, DiscoveredDevice> m_seen;
    bool m_active = false;
};

#endif // DISCOVERY_H
