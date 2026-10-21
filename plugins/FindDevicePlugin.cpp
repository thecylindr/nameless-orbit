#include "FindDevicePlugin.h"
#include "../core/PeerManager.h"
#include "../platform/PlatformBridge.h"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QUrl>
#include <QtMath>
#include <cmath>

namespace {
constexpr int ALERT_LIMIT_MS = 60000;
}

FindDevicePlugin::FindDevicePlugin(PeerManager *manager, QObject *parent)
    : PluginBase(manager, QStringLiteral("find"), parent)
{
    // пока просят найти устройство — вибрация повторяется (на системах без вибрации вызов пустой)
    m_vibrateTimer.setInterval(2000);
    connect(&m_vibrateTimer, &QTimer::timeout, this, []() { PlatformBridge::vibrate(350); });

    m_limitTimer.setSingleShot(true);
    m_limitTimer.setInterval(ALERT_LIMIT_MS);
    connect(&m_limitTimer, &QTimer::timeout, this, [this]() { stopAlert(); emit ringStopped(); });

    m_sound.setLoopCount(QSoundEffect::Infinite);
    m_sound.setVolume(0.6f);
}

// Сигнал генерируется один раз в кэш: спокойный двойной «колокольчик» (две мягкие ноты с затуханием и пауза), без резких тонов.
// Отдельного звукового файла в ресурсах не нужно, одинаково на всех системах
QString FindDevicePlugin::ensureSoundFile()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/orbit-find-v2.wav");
    if (QFile::exists(path)) return path;

    constexpr int rate = 22050;
    constexpr double seconds = 2.6;                  // один круг: две ноты и тишина
    const int total = int(rate * seconds);
    QByteArray pcm(total * 2, 0);
    auto *out = reinterpret_cast<qint16 *>(pcm.data());
    struct Note { double start, freq, length; };
    const Note notes[] = {{0.0, 659.25, 1.1}, {0.45, 523.25, 1.4}};      // ми и до первой октавы: мягкая нисходящая пара
    for (const Note &n : notes) {
        const int first = int(n.start * rate), count = int(n.length * rate);
        for (int k = 0; k < count && first + k < total; ++k) {
            const double t = double(k) / rate;
            const double attack = qMin(1.0, t / 0.02);                     // 20 мс нарастание — без щелчка
            const double env = attack * std::exp(-t * 3.4);                // плавное затухание, как у колокольчика
            const double v = (std::sin(2 * M_PI * n.freq * t) + 0.18 * std::sin(4 * M_PI * n.freq * t)) * env * 0.33;
            const int idx = first + k;
            out[idx] = qint16(qBound(-32000.0, double(out[idx]) + v * 32000.0, 32000.0));
        }
    }

    QByteArray wav;
    auto le32 = [&wav](quint32 x) { for (int k = 0; k < 4; ++k) wav.append(char((x >> (8 * k)) & 0xFF)); };
    auto le16 = [&wav](quint16 x) { for (int k = 0; k < 2; ++k) wav.append(char((x >> (8 * k)) & 0xFF)); };
    wav.append("RIFF"); le32(36 + pcm.size()); wav.append("WAVEfmt "); le32(16); le16(1); le16(1);
    le32(rate); le32(rate * 2); le16(2); le16(16); wav.append("data"); le32(pcm.size()); wav.append(pcm);

    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { f.write(wav); f.close(); }
    return path;
}

void FindDevicePlugin::startAlert(const QString &fromId, const QString &fromName)
{
    m_ringerId = fromId;
    if (!m_ringing) {
        m_ringing = true;
        PlatformBridge::setLoudVolume(true);                 // телефон на минимальной громкости всё равно должен найтись
        m_sound.setSource(QUrl::fromLocalFile(ensureSoundFile()));
        m_sound.play();
        m_vibrateTimer.start();
        PlatformBridge::vibrate(350);
        emit ringingChanged();
    }
    m_limitTimer.start();
    PlatformBridge::postNotification(QStringLiteral("Вас ищет «%1»").arg(fromName), QStringLiteral("Откройте Orbit и нажмите «Нашёл»"));
    emit ringRequested(fromId, fromName);
}

void FindDevicePlugin::stopAlert()
{
    m_limitTimer.stop();
    m_vibrateTimer.stop();
    m_sound.stop();
    PlatformBridge::cancelVibration();
    PlatformBridge::setLoudVolume(false);
    if (m_ringing) { m_ringing = false; emit ringingChanged(); }
}

void FindDevicePlugin::ring(const QString &deviceId)
{
    QJsonObject payload;
    payload[QStringLiteral("action")] = QStringLiteral("ring");
    m_manager->sendPlugin(deviceId, name(), payload);
}

void FindDevicePlugin::stop(const QString &deviceId)
{
    QJsonObject payload;
    payload[QStringLiteral("action")] = QStringLiteral("stop");
    m_manager->sendPlugin(deviceId, name(), payload);
}

void FindDevicePlugin::dismiss()
{
    const QString who = m_ringerId;
    stopAlert();
    if (!who.isEmpty()) {
        QJsonObject payload;
        payload[QStringLiteral("action")] = QStringLiteral("found");
        m_manager->sendPlugin(who, name(), payload);
    }
    m_ringerId.clear();
    emit ringStopped();
}

void FindDevicePlugin::handleMessage(const QString &deviceId, const QJsonObject &payload)
{
    if (!isEnabled()) return;
    const QString action = payload.value(QStringLiteral("action")).toString();
    const QString who = m_manager->trustStore()->device(deviceId).name;
    if (action == QLatin1String("ring")) {
        startAlert(deviceId, who);
    } else if (action == QLatin1String("stop")) {
        stopAlert();
        m_ringerId.clear();
        emit ringStopped();
    } else if (action == QLatin1String("found")) {
        emit deviceFound(deviceId, who);
    }
}
