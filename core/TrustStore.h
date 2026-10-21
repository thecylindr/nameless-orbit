#ifndef TRUSTSTORE_H
#define TRUSTSTORE_H

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QVariantList>
#include <QHash>
#include <QSet>

// Список сопряжённых устройств: публичный ключ (то, чем устройство доказывает себя при каждом подключении),
// имя на момент сопряжения, какие плагины ему разрешены. Файл — JSON в конфиге приложения, как known_hosts
// у SSH, только с правами по функциям, а не просто «доверять / не доверять» целиком.
struct TrustedDevice
{
    QString deviceId;
    QByteArray publicKey;
    QString name;             // имя на момент сопряжения; текущее живое имя при подключении берётся отдельно
    QString kind;
    qint64 pairedAt = 0;
    qint64 lastSeenAt = 0;
    QString model;            // модель устройства (для списка, когда оно не на связи)
    QString lastAddress;      // последний известный адрес: по нему можно подключиться, даже когда маяк не слышен (другая сеть, VPN)
    quint16 lastPort = 0;
    int trust = 0;            // доверие: 0 — спрашивать каждый раз; 1 — «доверенное» (файлы, показ экрана — без вопросов); 2 — полное (ещё и управление мышью и клавиатурой)
    QSet<QString> allowedPlugins;   // "clipboard" | "notifications" | "battery" | "find" | "media" | "runcommand" | "remoteDesktop"
};

class TrustStore : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList devices READ devicesForQml NOTIFY changed)

public:
    explicit TrustStore(QObject *parent = nullptr);

    bool isPaired(const QString &deviceId) const { return m_devices.contains(deviceId); }
    // ключ должен совпадать с запомненным — иначе это либо переустановка у пира, либо подмена
    bool verifyKey(const QString &deviceId, const QByteArray &publicKey) const;
    TrustedDevice device(const QString &deviceId) const { return m_devices.value(deviceId); }
    bool pluginAllowed(const QString &deviceId, const QString &plugin) const;

    Q_INVOKABLE void addOrUpdate(const QString &deviceId, const QByteArray &publicKey,
                                  const QString &name, const QString &kind);
    Q_INVOKABLE void forget(const QString &deviceId);
    int trustLevel(const QString &deviceId) const { return m_devices.value(deviceId).trust; }
    Q_INVOKABLE int trustLevelOf(const QString &deviceId) const { return trustLevel(deviceId); }
    Q_INVOKABLE void setTrustLevel(const QString &deviceId, int level);
    Q_INVOKABLE void setPluginAllowed(const QString &deviceId, const QString &plugin, bool allowed);
    bool anyAllows(const QString &plugin) const;         // разрешён ли плагин хоть одному устройству
    Q_INVOKABLE void touchLastSeen(const QString &deviceId);
    void setLastContact(const QString &deviceId, const QString &model, const QString &address, quint16 port);
    Q_INVOKABLE QVariantList devicesForQml() const;

signals:
    void changed();
    void pluginAllowedChanged(const QString &deviceId, const QString &plugin, bool allowed);

private:
    QString storePath() const;
    void load();
    void save() const;

    QHash<QString, TrustedDevice> m_devices;
};

#endif // TRUSTSTORE_H
