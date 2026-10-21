#include "SharePlugin.h"
#include "../core/PeerManager.h"
#include "../platform/PlatformBridge.h"
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QUuid>
#include <QUrl>
#include <QVariantMap>
#include <QDebug>

namespace {
constexpr int CHUNK = 32 * 1024;
constexpr int WINDOW = 8;                      // сколько кусков может быть отправлено без подтверждения
constexpr qint64 BACKLOG_LIMIT = 256 * 1024;   // сколько уже лежит в очереди сокета — больше не подкладываем
constexpr qint64 MAX_FILE = qint64(8) * 1024 * 1024 * 1024;
}

SharePlugin::SharePlugin(PeerManager *manager, QObject *parent)
    : PluginBase(manager, QStringLiteral("share"), parent)
{
    m_pumpTimer.setInterval(40);
    connect(&m_pumpTimer, &QTimer::timeout, this, &SharePlugin::pumpAll);
    m_notifyTimer.setSingleShot(true);
    m_notifyTimer.setInterval(120);
    connect(&m_notifyTimer, &QTimer::timeout, this, [this]() { m_dirty = false; emit transfersChanged(); });

    // пропало устройство — передачи с ним обрываются, а не висят вечно
    connect(manager, &PeerManager::deviceDisconnected, this, [this](const QString &deviceId) {
        for (const auto &t : std::as_const(m_transfers))
            if (t->deviceId == deviceId && (t->state == QLatin1String("active") || t->state == QLatin1String("offered")))
                finish(*t, QStringLiteral("failed"), QStringLiteral("связь с устройством потеряна"));
    });
}

QString SharePlugin::cleanName(const QString &name)
{
    // только имя файла: ни путей (../), ни управляющих символов; пустое — «file»
    QString n = QFileInfo(name).fileName();
    n.replace(QLatin1Char('/'), QLatin1Char('_')).replace(QLatin1Char('\\'), QLatin1Char('_'));
    QString out;
    for (const QChar c : n) if (c.unicode() >= 32 && c != QLatin1Char(':')) out += c;
    out = out.trimmed().left(120);
    while (out.startsWith(QLatin1Char('.'))) out.remove(0, 1);
    return out.isEmpty() ? QStringLiteral("file") : out;
}

QString SharePlugin::uniqueName(const QString &dir, const QString &name)
{
    if (!QFile::exists(dir + QLatin1Char('/') + name)) return name;
    const QFileInfo fi(name);
    const QString base = fi.completeBaseName(), ext = fi.suffix();
    for (int i = 1; i < 1000; ++i) {
        const QString cand = ext.isEmpty() ? QStringLiteral("%1 (%2)").arg(base).arg(i) : QStringLiteral("%1 (%2).%3").arg(base).arg(i).arg(ext);
        if (!QFile::exists(dir + QLatin1Char('/') + cand)) return cand;
    }
    return QUuid::createUuid().toString(QUuid::WithoutBraces) + QLatin1Char('_') + name;
}

QVariantList SharePlugin::transfers() const
{
    QVariantList out;
    for (const QString &id : m_order) {
        const auto it = m_transfers.constFind(id);
        if (it == m_transfers.constEnd()) continue;
        const Transfer &t = **it;
        QVariantMap m;
        m[QStringLiteral("id")] = t.id;
        m[QStringLiteral("deviceId")] = t.deviceId;
        m[QStringLiteral("deviceName")] = t.deviceName;
        m[QStringLiteral("name")] = t.name;
        m[QStringLiteral("size")] = t.size;
        m[QStringLiteral("done")] = t.done;
        m[QStringLiteral("outgoing")] = t.outgoing;
        m[QStringLiteral("state")] = t.state;
        m[QStringLiteral("path")] = t.path;
        out.append(m);
    }
    return out;
}

void SharePlugin::changed()
{
    // прогресс идёт часто — интерфейс обновляется не чаще ~8 раз в секунду
    if (!m_dirty) { m_dirty = true; m_notifyTimer.start(); }
}

void SharePlugin::sendTo(const Transfer &t, const QJsonObject &payload)
{
    m_manager->sendPlugin(t.deviceId, name(), payload);
}

// ---------- отправка ----------

void SharePlugin::sendFile(const QString &deviceId, const QString &pathOrUrl)
{
    if (!isEnabled() || !m_manager->isConnected(deviceId)) return;
    QString path = pathOrUrl;
    if (path.startsWith(QLatin1String("file:"))) path = QUrl(path).toLocalFile();   // content:// остаётся как есть — Qt открывает его сам
    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::ReadOnly)) { emit transferFailed(QFileInfo(path).fileName(), QStringLiteral("не удалось открыть файл")); return; }
    const qint64 size = file->size();
    if (size <= 0 || size > MAX_FILE) { emit transferFailed(QFileInfo(path).fileName(), size <= 0 ? QStringLiteral("файл пустой") : QStringLiteral("файл слишком большой")); return; }

    auto t = std::make_shared<Transfer>();
    t->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    t->deviceId = deviceId;
    t->deviceName = m_manager->trustStore()->device(deviceId).name;
    t->name = cleanName(QFileInfo(path).fileName());
    t->size = size;
    t->outgoing = true;
    t->state = QStringLiteral("offered");
    t->file = std::move(file);
    m_transfers.insert(t->id, t);
    m_order.append(t->id);

    QJsonObject o;
    o[QStringLiteral("kind")] = QStringLiteral("offer");
    o[QStringLiteral("id")] = t->id;
    o[QStringLiteral("name")] = t->name;
    o[QStringLiteral("size")] = double(size);
    sendTo(*t, o);
    changed();
}

void SharePlugin::pump(Transfer &t)
{
    if (!t.outgoing || t.state != QLatin1String("active") || t.eofSent) return;
    while (t.sentSeq - t.ackedSeq < WINDOW && m_manager->backlog(t.deviceId) < BACKLOG_LIMIT) {
        const QByteArray data = t.file->read(CHUNK);
        if (data.isEmpty()) {
            t.eofSent = true;
            QJsonObject o;
            o[QStringLiteral("kind")] = QStringLiteral("done");
            o[QStringLiteral("id")] = t.id;
            o[QStringLiteral("chunks")] = t.sentSeq;
            sendTo(t, o);
            return;
        }
        QJsonObject o;
        o[QStringLiteral("kind")] = QStringLiteral("chunk");
        o[QStringLiteral("id")] = t.id;
        o[QStringLiteral("seq")] = t.sentSeq;
        o[QStringLiteral("data")] = QString::fromLatin1(data.toBase64());
        sendTo(t, o);
        ++t.sentSeq;
    }
}

void SharePlugin::pumpAll()
{
    bool any = false;
    for (const auto &t : std::as_const(m_transfers))
        if (t->outgoing && t->state == QLatin1String("active") && !t->eofSent) { any = true; pump(*t); }
    if (!any) m_pumpTimer.stop();
}

// ---------- получение: решение человека ----------

void SharePlugin::accept(const QString &id)
{
    const auto it = m_transfers.find(id);
    if (it == m_transfers.end() || (*it)->outgoing || (*it)->state != QLatin1String("offered")) return;
    Transfer &t = **it;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/incoming");
    QDir().mkpath(dir);
    t.partPath = dir + QLatin1Char('/') + t.id + QStringLiteral(".part");
    t.file = std::make_unique<QFile>(t.partPath);
    if (!t.file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QJsonObject o; o[QStringLiteral("kind")] = QStringLiteral("decline"); o[QStringLiteral("id")] = id; sendTo(t, o);
        finish(t, QStringLiteral("failed"), QStringLiteral("нет места для файла"));
        return;
    }
    t.state = QStringLiteral("active");
    QJsonObject o;
    o[QStringLiteral("kind")] = QStringLiteral("accept");
    o[QStringLiteral("id")] = id;
    sendTo(t, o);
    changed();
}

void SharePlugin::decline(const QString &id)
{
    const auto it = m_transfers.find(id);
    if (it == m_transfers.end() || (*it)->outgoing || (*it)->state != QLatin1String("offered")) return;
    QJsonObject o;
    o[QStringLiteral("kind")] = QStringLiteral("decline");
    o[QStringLiteral("id")] = id;
    sendTo(**it, o);
    finish(**it, QStringLiteral("declined"));
}

void SharePlugin::cancel(const QString &id)
{
    const auto it = m_transfers.find(id);
    if (it == m_transfers.end()) return;
    Transfer &t = **it;
    if (t.state != QLatin1String("active") && t.state != QLatin1String("offered")) return;
    QJsonObject o;
    o[QStringLiteral("kind")] = QStringLiteral("cancel");
    o[QStringLiteral("id")] = id;
    sendTo(t, o);
    finish(t, QStringLiteral("cancelled"));
}

void SharePlugin::clearFinished()
{
    QStringList keep;
    for (const QString &id : std::as_const(m_order)) {
        const auto it = m_transfers.constFind(id);
        if (it == m_transfers.constEnd()) continue;
        const QString s = (*it)->state;
        if (s == QLatin1String("active") || s == QLatin1String("offered")) keep << id;
        else m_transfers.remove(id);
    }
    m_order = keep;
    emit transfersChanged();
}

void SharePlugin::finish(Transfer &t, const QString &state, const QString &reason)
{
    t.state = state;
    if (t.file) { t.file->close(); t.file.reset(); }
    if (!t.outgoing && !t.partPath.isEmpty() && state != QLatin1String("done")) QFile::remove(t.partPath);
    if (state == QLatin1String("failed")) emit transferFailed(t.name, reason);
    changed();
}

// ---------- сообщения от другого устройства ----------

void SharePlugin::handleMessage(const QString &deviceId, const QJsonObject &p)
{
    if (!isEnabled()) return;
    const QString kind = p.value(QStringLiteral("kind")).toString();
    const QString id = p.value(QStringLiteral("id")).toString();

    if (kind == QLatin1String("offer")) {
        const qint64 size = qint64(p.value(QStringLiteral("size")).toDouble());
        if (id.isEmpty() || m_transfers.contains(id) || size <= 0 || size > MAX_FILE) return;
        auto t = std::make_shared<Transfer>();
        t->id = id;
        t->deviceId = deviceId;
        t->deviceName = m_manager->trustStore()->device(deviceId).name;
        t->name = cleanName(p.value(QStringLiteral("name")).toString());
        t->size = size;
        t->outgoing = false;
        t->state = QStringLiteral("offered");
        m_transfers.insert(id, t);
        m_order.append(id);
        changed();
        PlatformBridge::postNotification(QStringLiteral("%1 отправляет файл").arg(t->deviceName), t->name);
        // «доверенное» устройство (уровень 1 и выше): файл принимается без вопроса; размер и имя всё равно проверены выше
        if (m_manager->trustStore()->trustLevel(t->deviceId) >= 1 && size <= (qint64(4) << 30)) { accept(id); return; }
        emit offerReceived(id, t->deviceName, t->name, size);
        return;
    }

    const auto it = m_transfers.find(id);
    if (it == m_transfers.end() || (*it)->deviceId != deviceId) return;     // чужие и неизвестные id игнорируются
    Transfer &t = **it;

    if (t.outgoing) {
        if (kind == QLatin1String("accept") && t.state == QLatin1String("offered")) {
            t.state = QStringLiteral("active");
            m_pumpTimer.start();
            pump(t);
            changed();
        } else if (kind == QLatin1String("decline")) {
            finish(t, QStringLiteral("declined"));
        } else if (kind == QLatin1String("ack") && t.state == QLatin1String("active")) {
            t.ackedSeq = qMax(t.ackedSeq, p.value(QStringLiteral("seq")).toInt() + 1);
            t.done = qMin(t.size, qint64(t.ackedSeq) * CHUNK);
            pump(t);
            changed();
        } else if (kind == QLatin1String("saved")) {
            t.done = t.size;
            const QString n = t.name, dev = t.deviceName;
            finish(t, QStringLiteral("done"));
            emit fileSent(dev, n);
        } else if (kind == QLatin1String("cancel")) {
            finish(t, QStringLiteral("cancelled"));
        } else if (kind == QLatin1String("failed")) {
            finish(t, QStringLiteral("failed"), p.value(QStringLiteral("reason")).toString());
        }
        return;
    }

    // получатель
    if (kind == QLatin1String("chunk") && t.state == QLatin1String("active") && t.file) {
        const int seq = p.value(QStringLiteral("seq")).toInt(-1);
        if (seq != t.expectSeq) { finish(t, QStringLiteral("failed"), QStringLiteral("нарушен порядок кусков")); return; }
        const QByteArray data = QByteArray::fromBase64(p.value(QStringLiteral("data")).toString().toLatin1());
        if (data.isEmpty() || t.done + data.size() > t.size) { finish(t, QStringLiteral("failed"), QStringLiteral("получено больше заявленного")); return; }
        if (t.file->write(data) != data.size()) { finish(t, QStringLiteral("failed"), QStringLiteral("не хватает места")); return; }
        t.done += data.size();
        ++t.expectSeq;
        QJsonObject o;
        o[QStringLiteral("kind")] = QStringLiteral("ack");
        o[QStringLiteral("id")] = id;
        o[QStringLiteral("seq")] = seq;
        sendTo(t, o);
        changed();
    } else if (kind == QLatin1String("done") && t.state == QLatin1String("active")) {
        t.file->close();
        if (t.done != t.size) {
            QJsonObject o; o[QStringLiteral("kind")] = QStringLiteral("failed"); o[QStringLiteral("id")] = id; o[QStringLiteral("reason")] = QStringLiteral("размер не совпал"); sendTo(t, o);
            finish(t, QStringLiteral("failed"), QStringLiteral("размер не совпал"));
            return;
        }
        const QString saved = PlatformBridge::publishDownload(t.partPath, t.name);
        if (saved.isEmpty()) {
            QJsonObject o; o[QStringLiteral("kind")] = QStringLiteral("failed"); o[QStringLiteral("id")] = id; o[QStringLiteral("reason")] = QStringLiteral("не удалось сохранить"); sendTo(t, o);
            finish(t, QStringLiteral("failed"), QStringLiteral("не удалось сохранить"));
            return;
        }
        t.path = saved;
        QJsonObject o;
        o[QStringLiteral("kind")] = QStringLiteral("saved");
        o[QStringLiteral("id")] = id;
        sendTo(t, o);
        const QString n = t.name, dev = t.deviceName;
        finish(t, QStringLiteral("done"));
        PlatformBridge::postNotification(QStringLiteral("Файл получен"), QStringLiteral("%1 — %2").arg(n, saved));
        emit fileReceived(dev, n, saved);
    } else if (kind == QLatin1String("cancel")) {
        finish(t, QStringLiteral("cancelled"));
    }
}
