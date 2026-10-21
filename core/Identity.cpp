#include "Identity.h"
#include "CryptoUtils.h"
#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <QUuid>
#include <QSysInfo>
#include <QFile>
#include <QNetworkInterface>
#include "../platform/PlatformBridge.h"

QString Identity::identityFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/identity.ini");
}

QString Identity::deviceModel() const
{
    static const QString model = PlatformBridge::deviceModel();
    return model;
}

QStringList Identity::localAddresses() const
{
    QStringList out;
    for (const QNetworkInterface &ni : QNetworkInterface::allInterfaces()) {
        const auto f = ni.flags();
        if (!(f & QNetworkInterface::IsUp) || !(f & QNetworkInterface::IsRunning) || (f & QNetworkInterface::IsLoopBack) || (f & QNetworkInterface::IsPointToPoint)) continue;
        for (const QNetworkAddressEntry &e : ni.addressEntries())
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol) out << e.ip().toString();
    }
    return out;
}

QString Identity::defaultDeviceName()
{
    // Android: название из настроек телефона; компьютер: имя узла. «localhost» и пустое имя за имя не считаются — берётся модель
    const QString sys = PlatformBridge::systemDeviceName().trimmed();
    if (!sys.isEmpty() && sys != QLatin1String("localhost")) return sys;
    const QString model = PlatformBridge::deviceModel();
    return model.isEmpty() ? QStringLiteral("Безымянное устройство") : model;
}

QString Identity::guessDeviceKind()
{
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    return QStringLiteral("phone");
#else
    // ноутбук против настольного — по наличию батареи узнаём позже через BatteryPlugin; по умолчанию desktop
    return QStringLiteral("desktop");
#endif
}

Identity::Identity(QObject *parent) : QObject(parent)
{
    load();
}

void Identity::load()
{
    QSettings s(identityFilePath(), QSettings::IniFormat);
    const QByteArray secretHex = s.value(QStringLiteral("staticSecret")).toByteArray();

    if (secretHex.isEmpty()) {
        // первый запуск: своя статическая пара ключей создаётся один раз и больше не меняется
        m_staticSecret = CryptoUtils::randomBytes(CryptoUtils::KEY_SIZE);
        m_staticPublic = CryptoUtils::x25519Public(m_staticSecret);
        m_deviceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_deviceName = defaultDeviceName();
        m_nameAuto = true;
        m_deviceKind = guessDeviceKind();
        save();
    } else {
        m_staticSecret = QByteArray::fromHex(secretHex);
        m_staticPublic = CryptoUtils::x25519Public(m_staticSecret);
        m_deviceId = s.value(QStringLiteral("deviceId")).toString();
        m_deviceName = s.value(QStringLiteral("deviceName"), defaultDeviceName()).toString();
        // имя либо взято у системы (обновляется при каждом запуске: переименовали телефон — Orbit подхватит), либо задано человеком;
        // у файлов прежних версий признака нет — «localhost», пустое имя и модель считаются автоматическими
        m_nameAuto = s.value(QStringLiteral("deviceNameAuto"),
                             m_deviceName.isEmpty() || m_deviceName == QLatin1String("localhost") || m_deviceName == PlatformBridge::deviceModel()).toBool();
        if (m_nameAuto) {
            const QString fresh = defaultDeviceName();
            if (fresh != m_deviceName) { m_deviceName = fresh; save(); }
        }
        m_deviceKind = s.value(QStringLiteral("deviceKind"), guessDeviceKind()).toString();
        if (m_deviceId.isEmpty()) { m_deviceId = QUuid::createUuid().toString(QUuid::WithoutBraces); save(); }
    }
}

void Identity::save() const
{
    QSettings s(identityFilePath(), QSettings::IniFormat);
    s.setValue(QStringLiteral("staticSecret"), m_staticSecret.toHex());
    s.setValue(QStringLiteral("deviceId"), m_deviceId);
    s.setValue(QStringLiteral("deviceName"), m_deviceName);
    s.setValue(QStringLiteral("deviceNameAuto"), m_nameAuto);
    s.setValue(QStringLiteral("deviceKind"), m_deviceKind);
    // ключ — секрет уровня «кто угодно с ним выдаст себя за это устройство»: файл только для владельца
    QFile::setPermissions(identityFilePath(), QFile::ReadOwner | QFile::WriteOwner);
}

QString Identity::fingerprint() const { return CryptoUtils::fingerprint(m_staticPublic); }
QString Identity::shortCode() const { return CryptoUtils::shortCode(m_staticPublic); }

void Identity::setDeviceName(const QString &name)
{
    const QString trimmed = name.trimmed().left(40);
    if (trimmed.isEmpty() || trimmed == m_deviceName) return;
    m_deviceName = trimmed;
    m_nameAuto = false;            // своё имя больше не подменяется системным
    save();
    emit deviceNameChanged();
}

void Identity::setDeviceKind(const QString &kind)
{
    static const QStringList known = {QStringLiteral("desktop"), QStringLiteral("laptop"), QStringLiteral("phone"),
                                      QStringLiteral("tablet"), QStringLiteral("server")};
    if (!known.contains(kind) || kind == m_deviceKind) return;
    m_deviceKind = kind;
    save();
    emit deviceKindChanged();
}
