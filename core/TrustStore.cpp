#include "TrustStore.h"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDateTime>
#include <QVariantMap>

QString TrustStore::storePath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/trusted-devices.json");
}

TrustStore::TrustStore(QObject *parent) : QObject(parent)
{
    load();
}

void TrustStore::load()
{
    bool migrated = false;
    QFile f(storePath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        TrustedDevice d;
        d.deviceId = o.value(QStringLiteral("id")).toString();
        d.publicKey = QByteArray::fromHex(o.value(QStringLiteral("key")).toString().toLatin1());
        d.name = o.value(QStringLiteral("name")).toString();
        d.kind = o.value(QStringLiteral("kind")).toString();
        d.pairedAt = qint64(o.value(QStringLiteral("pairedAt")).toDouble());
        d.lastSeenAt = qint64(o.value(QStringLiteral("lastSeenAt")).toDouble());
        d.model = o.value(QStringLiteral("model")).toString();
        d.lastAddress = o.value(QStringLiteral("addr")).toString();
        d.lastPort = quint16(o.value(QStringLiteral("port")).toInt());
        d.trust = qBound(0, o.value(QStringLiteral("trust")).toInt(0), 2);
        for (const QJsonValue &p : o.value(QStringLiteral("plugins")).toArray())
            d.allowedPlugins.insert(p.toString());
        // записи прежних версий: появились разрешения «буфер», «плеер», «файлы» — включаются по умолчанию, как у новых сопряжений
        const int pv = o.value(QStringLiteral("pv")).toInt(1);
        if (pv < 3) {
            d.allowedPlugins.insert(QStringLiteral("clipboard")); d.allowedPlugins.insert(QStringLiteral("media")); d.allowedPlugins.insert(QStringLiteral("share"));
            d.allowedPlugins.insert(QStringLiteral("runcommand"));
            migrated = true;
        }
        if (!d.deviceId.isEmpty() && d.publicKey.size() == 32)
            m_devices.insert(d.deviceId, d);
    }
    if (migrated) save();
}

void TrustStore::setTrustLevel(const QString &deviceId, int level)
{
    auto it = m_devices.find(deviceId);
    if (it == m_devices.end()) return;
    level = qBound(0, level, 2);
    if (it->trust == level) return;
    it->trust = level;
    save();
    emit changed();
}

void TrustStore::save() const
{
    QJsonArray arr;
    for (const TrustedDevice &d : m_devices) {
        QJsonObject o;
        o[QStringLiteral("id")] = d.deviceId;
        o[QStringLiteral("key")] = QString::fromLatin1(d.publicKey.toHex());
        o[QStringLiteral("name")] = d.name;
        o[QStringLiteral("pv")] = 3;          // версия набора разрешений по умолчанию
        o[QStringLiteral("kind")] = d.kind;
        o[QStringLiteral("model")] = d.model;
        o[QStringLiteral("addr")] = d.lastAddress;
        o[QStringLiteral("port")] = int(d.lastPort);
        o[QStringLiteral("trust")] = d.trust;
        o[QStringLiteral("pairedAt")] = double(d.pairedAt);
        o[QStringLiteral("lastSeenAt")] = double(d.lastSeenAt);
        QJsonArray plugins;
        for (const QString &p : d.allowedPlugins) plugins.append(p);
        o[QStringLiteral("plugins")] = plugins;
        arr.append(o);
    }
    QFile f(storePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    }
}

bool TrustStore::verifyKey(const QString &deviceId, const QByteArray &publicKey) const
{
    const auto it = m_devices.constFind(deviceId);
    if (it == m_devices.constEnd()) return false;
    return it->publicKey == publicKey;
}

bool TrustStore::pluginAllowed(const QString &deviceId, const QString &plugin) const
{
    const auto it = m_devices.constFind(deviceId);
    return it != m_devices.constEnd() && it->allowedPlugins.contains(plugin);
}

void TrustStore::addOrUpdate(const QString &deviceId, const QByteArray &publicKey,
                              const QString &name, const QString &kind)
{
    TrustedDevice &d = m_devices[deviceId];
    const bool isNew = d.deviceId.isEmpty();
    d.deviceId = deviceId;
    d.publicKey = publicKey;
    d.name = name;
    d.kind = kind;
    if (isNew) {
        d.pairedAt = QDateTime::currentSecsSinceEpoch();
        // включается сразу то, что работает по явному действию человека и в пределах своего списка: буфер — кнопкой, файл — с согласием
        // получателя, команды — только из списка владельца (опасные в нём помечаются «подтверждать здесь»); удалённый экран — отдельно
        d.allowedPlugins = {QStringLiteral("battery"), QStringLiteral("find"), QStringLiteral("notifications"),
                            QStringLiteral("clipboard"), QStringLiteral("media"), QStringLiteral("share"), QStringLiteral("runcommand")};
    }
    d.lastSeenAt = QDateTime::currentSecsSinceEpoch();
    save();
    emit changed();
}

void TrustStore::forget(const QString &deviceId)
{
    if (m_devices.remove(deviceId) > 0) {
        save();
        emit changed();
    }
}

void TrustStore::setPluginAllowed(const QString &deviceId, const QString &plugin, bool allowed)
{
    const auto it = m_devices.find(deviceId);
    if (it == m_devices.end()) return;
    if (allowed) it->allowedPlugins.insert(plugin);
    else it->allowedPlugins.remove(plugin);
    save();
    emit changed();
    emit pluginAllowedChanged(deviceId, plugin, allowed);
}

bool TrustStore::anyAllows(const QString &plugin) const
{
    for (const TrustedDevice &d : m_devices) if (d.allowedPlugins.contains(plugin)) return true;
    return false;
}

void TrustStore::setLastContact(const QString &deviceId, const QString &model, const QString &address, quint16 port)
{
    const auto it = m_devices.find(deviceId);
    if (it == m_devices.end()) return;
    const bool dirty = it->model != model || it->lastAddress != address || it->lastPort != port;
    it->model = model; it->lastAddress = address; it->lastPort = port;
    it->lastSeenAt = QDateTime::currentSecsSinceEpoch();
    if (dirty) { save(); emit changed(); }
}

void TrustStore::touchLastSeen(const QString &deviceId)
{
    const auto it = m_devices.find(deviceId);
    if (it == m_devices.end()) return;
    it->lastSeenAt = QDateTime::currentSecsSinceEpoch();
    // здесь не сохраняем на диск каждое подключение (частое событие) — lastSeenAt попадёт в файл при следующем save()
}

QVariantList TrustStore::devicesForQml() const
{
    QVariantList out;
    for (const TrustedDevice &d : m_devices) {
        QVariantMap m;
        m[QStringLiteral("deviceId")] = d.deviceId;
        m[QStringLiteral("name")] = d.name;
        m[QStringLiteral("kind")] = d.kind;
        m[QStringLiteral("model")] = d.model;
        m[QStringLiteral("address")] = d.lastAddress;
        m[QStringLiteral("pairedAt")] = d.pairedAt;
        m[QStringLiteral("lastSeenAt")] = d.lastSeenAt;
        m[QStringLiteral("trust")] = d.trust;
        QVariantList plugins;
        for (const QString &p : d.allowedPlugins) plugins.append(p);
        m[QStringLiteral("plugins")] = plugins;
        out.append(m);
    }
    return out;
}
