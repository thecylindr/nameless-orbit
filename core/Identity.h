#ifndef IDENTITY_H
#define IDENTITY_H

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringList>

// Личность этого устройства в Orbit: статическая пара ключей X25519 (создаётся один раз, хранится в конфиге
// приложения — аналог ключа хоста SSH), её отпечаток — то, что другие устройства сверяют при сопряжении.
// Имя и тип видны всем в локальной сети до сопряжения (как в KDE Connect) — секрета в них нет.
class Identity : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString deviceId READ deviceId CONSTANT)
    Q_PROPERTY(QString deviceName READ deviceName WRITE setDeviceName NOTIFY deviceNameChanged)
    Q_PROPERTY(QString deviceKind READ deviceKind WRITE setDeviceKind NOTIFY deviceKindChanged)
    Q_PROPERTY(QString fingerprint READ fingerprint CONSTANT)
    Q_PROPERTY(QString shortCode READ shortCode CONSTANT)
    Q_PROPERTY(QString deviceModel READ deviceModel CONSTANT)
    Q_PROPERTY(QStringList localAddresses READ localAddresses NOTIFY deviceNameChanged)

public:
    explicit Identity(QObject *parent = nullptr);

    QString deviceId() const { return m_deviceId; }
    QString deviceName() const { return m_deviceName; }
    QString deviceModel() const;                               // «Xiaomi 14», «Machenike L15»
    QStringList localAddresses() const;                       // IPv4 адреса в локальной сети (без туннелей и loopback)
    QString deviceKind() const { return m_deviceKind; }       // "desktop" | "laptop" | "phone" | "tablet" | "server"
    QString fingerprint() const;
    QString shortCode() const;

    QByteArray staticPublicKey() const { return m_staticPublic; }
    QByteArray staticSecretKey() const { return m_staticSecret; }

public slots:
    void setDeviceName(const QString &name);
    void setDeviceKind(const QString &kind);

signals:
    void deviceNameChanged();
    void deviceKindChanged();

private:
    void load();
    void save() const;
    static QString defaultDeviceName();
    static QString guessDeviceKind();
    static QString identityFilePath();

    QString m_deviceId;        // UUID, не меняется — используется как ключ в списке доверенных
    QString m_deviceName;
    bool m_nameAuto = true;        // имя берётся у системы (название телефона, имя узла) и обновляется само; своё имя это отключает
    QString m_deviceKind;
    QByteArray m_staticSecret;
    QByteArray m_staticPublic;
};

#endif // IDENTITY_H
