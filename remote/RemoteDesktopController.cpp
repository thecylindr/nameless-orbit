#include "RemoteDesktopController.h"
#include <QElapsedTimer>
#include <cstring>
#include <QColor>
#include <QStandardPaths>
#include <QtEndian>
#include <thread>
#include <vector>
#include <algorithm>
#include <QVariantMap>
#include "../core/PeerManager.h"
#include "../core/SettingsManager.h"
#include "../platform/AndroidCapture.h"
#include <QQmlEngine>
#include <QBuffer>
#include <QJsonArray>
#include <cmath>
#include <QGuiApplication>
#include <QPainter>
#include <QThreadPool>
#include <QScreen>
#include <QDateTime>
#include "../platform/PlatformBridge.h"

namespace {
constexpr int INPUT_TIMEOUT_CHECK_MS = 5000;
constexpr int GRACE_MS = 20000;               // сколько показ ждёт вернувшегося зрителя
constexpr int RECONNECT_TIMEOUT_MS = 30000;   // сколько зритель ждёт вернувшегося показывающего

// «Лесенка» качества: чем выше номер, тем легче поток. Размер считается от ширины окна зрителя (scale), поэтому текст остаётся чётким,
// а в первую очередь снижается частота кадров и качество JPEG, и лишь потом — размер
struct Level { double scale; int quality, fps; };
constexpr Level kLevels[] = {{1.00, 78, 45}, {1.00, 74, 36}, {1.00, 68, 30}, {1.00, 62, 24}, {0.85, 58, 20}, {0.70, 54, 15}, {0.55, 50, 12}, {0.45, 46, 8}};
// Лесенка видео: частота меняется мелкими шагами (60 → 48 → 36 → 30 → 24 → 20 → 15 → 12 → 8), размер и качество — лишь с четвёртой ступени.
// crfAdd — на сколько ухудшается сжатие (одна единица — около четырёх процентов качества)
struct VLevel { int fps; double scale; int crfAdd; };
constexpr VLevel kVideo[] = {{60, 1.00, 0}, {48, 1.00, 1}, {36, 1.00, 2}, {30, 1.00, 3}, {24, 0.90, 4}, {20, 0.80, 6}, {15, 0.70, 8}, {12, 0.55, 10}, {8, 0.45, 12}};
constexpr int kVideoLevelCount = int(sizeof(kVideo) / sizeof(kVideo[0]));
constexpr int kMaxDecoders = 3;
constexpr int kLevelCount = int(sizeof(kLevels) / sizeof(kLevels[0]));
inline qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
}

RemoteDesktopController::RemoteDesktopController(PeerManager *manager, SettingsManager *settings, QQmlEngine *qmlEngine, QObject *parent)
    : PluginBase(manager, QStringLiteral("remoteDesktop"), parent), m_settings(settings)
{
    if (qmlEngine) qmlEngine->addImageProvider(QStringLiteral("orbit"), new ImageProvider(this));
#ifdef Q_OS_ANDROID
    AndroidCapture::stop();          // служба захвата, оставшаяся от прошлого запуска, при старте не нужна: показ всегда начинается с запроса
#endif

    connect(&m_capture, &ScreenCaptureSession::frameReady, this, [this](const QVector<FramePart> &parts, int w, int h, bool key, const QRectF &region, const QSize &source, qint64 captureMs) {
        m_outRegion = region; m_outSource = source; m_outCaptureMs = captureMs;
        sendFrame(parts, w, h, key);
    });
    connect(&m_capture, &ScreenCaptureSession::videoReady, this, [this](const QByteArray &au, int w, int h, bool key, const QRectF &region, const QSize &source, qint64 captureMs) {
        m_outRegion = region; m_outSource = source; m_outCaptureMs = captureMs;
        sendVideo(au, w, h, key);
    });
    // монитор второго экрана не должен пережить Orbit: на выходе убирается (а если Orbit упал — при следующем запуске, см. VirtualDisplay::cleanupStale)
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() { releaseSecondScreen(); });
    // звук: пакеты показывающего уходят зрителю; настройки приёма применяются сразу; просмотр закончился — звук тоже
    connect(&m_audioOut, &AudioStream::packet, this, [this](const QByteArray &data, const QString &codec) {
        if (m_sharingTo.isEmpty() || m_manager->backlog(m_sharingTo) > 256 * 1024) return;       // сеть не успевает — кусок пропускается, звук не копится
        QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("audio"); p[QStringLiteral("c")] = codec; p[QStringLiteral("d")] = QString::fromLatin1(data.toBase64());
        sendToPeer(m_sharingTo, p);
    });
    connect(&m_audioOut, &AudioStream::failed, this, [this](const QString &why) {
        ScreenCaptureSession::log(QStringLiteral("Звук (отправка): %1").arg(why));
        if (!m_sharingTo.isEmpty()) { QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("audioState"); p[QStringLiteral("on")] = false; p[QStringLiteral("reason")] = why; sendToPeer(m_sharingTo, p); }
        m_audioOut.stopCapture();
    });
    connect(&m_audioIn, &AudioStream::failed, this, [this](const QString &why) { ScreenCaptureSession::log(QStringLiteral("Звук (приём): %1").arg(why)); stopAudioIn(QStringLiteral("off:") + why); });
    // громкость этого компьютера (и «без звука») — зрителю: у него звук такой же громкий, как здесь
    connect(&m_audioOut, &AudioStream::volumeChanged, this, [this](double level, bool muted) {
        if (m_sharingTo.isEmpty()) return;
        QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("audioVol"); p[QStringLiteral("v")] = level; p[QStringLiteral("m")] = muted;
        sendToPeer(m_sharingTo, p);
    });
    // клавиатура зрителя второго экрана: каждая клавиша — сразу (их мало, задержка важнее пачек)
    connect(&m_kbGrab, &KeyboardGrab::key, this, [this](int code, int value) {
        if (m_viewing.isEmpty()) return;
        QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("input"); p[QStringLiteral("op")] = QStringLiteral("raw");
        p[QStringLiteral("e")] = QJsonArray{QJsonArray{1 /* EV_KEY */, code, value}};
        sendToPeer(m_viewing, p);
    });
    connect(&m_kbGrab, &KeyboardGrab::released, this, [this]() { emit keyboardGrabChanged(); emit keyboardReleasedByUser(); });
    connect(m_settings, &SettingsManager::audioChanged, this, [this]() { applyAudioSettings(); });
    connect(this, &RemoteDesktopController::viewingChanged, this, [this]() { if (m_viewing.isEmpty() && (m_audioWant || !m_audioState.isEmpty())) stopAudioIn(); });
    applyAudioSettings();
    m_decoder = new VideoDecoder(this);
    connect(m_decoder, &VideoDecoder::frameDecoded, this, [this](const QVideoFrame &f, qint64 pts) { onVideoFrame(f, pts); });
    connect(m_decoder, &VideoDecoder::failed, this, [this](const QString &why) { onVideoFailed(why); });
    connect(&m_capture, &ScreenCaptureSession::sourceLost, this, [this]() {
        emit errorOccurred(QStringLiteral("Показываемое окно закрыто — показ остановлен"));
        stopSharing();
    });
    connect(&m_capture, &ScreenCaptureSession::errorChanged, this, &RemoteDesktopController::onCaptureError);
    connect(&m_capture, &ScreenCaptureSession::activeChanged, this, &RemoteDesktopController::sharingChanged);

    m_inputTimeoutTimer.setInterval(INPUT_TIMEOUT_CHECK_MS);
    connect(&m_inputTimeoutTimer, &QTimer::timeout, this, [this]() {
        if (m_inputArmedFor.isEmpty()) return;
        const int limit = m_settings->inputSessionTimeoutMin();
        if (m_ssFps == 0 && limit > 0 && m_inputArmedAt.elapsed() > qint64(limit) * 60000) stopInputSession();      // у второго экрана ввод без срока
    });
    m_inputTimeoutTimer.start();

    // кадр готовится, только если зритель успевает: не больше трёх без подтверждения и пустая очередь сокета
    m_capture.setGate([this]() { return canSendFrame(); });

    m_statsTimer.setInterval(1000);
    connect(&m_statsTimer, &QTimer::timeout, this, [this]() {
        m_viewFps = m_viewFramesInSecond;
        m_viewFramesInSecond = 0;
        m_shownFps = m_shownInSecond; m_shownInSecond = 0;
        if (!m_viewing.isEmpty()) { m_fpsHistory.append(m_viewFps); while (m_fpsHistory.size() > 40) m_fpsHistory.removeFirst(); }
        else m_fpsHistory.clear();
        m_rxKbit = int(m_rxBytes * 8 / 1000); m_rxBytes = 0;
        m_txKbit = int(m_txBytes * 8 / 1000); m_txBytes = 0;
        m_sentFps = m_sentInSecond;
        m_sentInSecond = 0;
        if (!m_sharingTo.isEmpty() && m_manager->isConnected(m_sharingTo)) {
            // ступень меняется по задержке до зрителя и по заполнению очереди сокета — и только если это держится 2 секунды подряд
            // (кратковременный скачок Wi-Fi не должен ронять качество); возврат — после 4 спокойных секунд
            const bool autoMode = m_settings->streamAuto();         // в ручном режиме значения неизменны
            // Пороги — от собственной «спокойной» задержки сети (на Wi-Fi она сама 100–250 мс из-за телефона). Раньше «быстро» было только
            // при задержке меньше 140 мс: на обычной Wi-Fi это не наступало никогда, и после одного сбоя частота так и оставалась низкой (12 к/с)
            if (m_rttMs > 0) { if (m_rttBase <= 0 || m_rttMs < m_rttBase) m_rttBase = m_rttMs; else m_rttBase += 0.5; }
            const double slowAt = qMax(600.0, m_rttBase * 4), fastAt = qMax(320.0, m_rttBase * 2.2);
            const bool slow = autoMode && (m_rttMs > slowAt || m_queueBlocked);
            const bool fast = autoMode && m_rttMs > 0 && m_rttMs < fastAt && !m_queueBlocked;
            // кодер не успевает за заданной частотой (процессор отправителя): видео идёт с постоянной частотой, поэтому недобор — признак нехватки ЦП;
            // снижается размер, а не только частота
            const bool vid = videoLadder();
            // разгон кодера после запуска (первые секунды кадров меньше) нехваткой не считается, видеокарта не ограничена процессором,
            // а решение принимается, только если недобор держится три секунды подряд — раньше одна медленная секунда старта запирала частоту на 15 к/с
            const bool lagNow = autoMode && vid && m_capture.videoMode() && !m_capture.gpuEncoding() && m_capture.encoderAgeMs() > 4000
                                && m_capture.targetFps() >= 20 && m_capture.captureFps() > 0 && m_capture.captureFps() < 0.75 * m_capture.targetFps();
            m_cpuLagSeconds = lagNow ? m_cpuLagSeconds + 1 : 0;
            const bool cpuLag = m_cpuLagSeconds >= 3;
            const qint64 tNow = nowMs();
            const int steps = ladderSize();
            if (slow || cpuLag) {
                m_goodSeconds = 0;
                const bool severe = m_rttMs > 1800;                   // совсем плохо — снижаем сразу, и сразу на две ступени
                // кодер не тянет заданную частоту: запоминаем, сколько он реально выдаёт, и две минуты выше не поднимаемся — иначе частота «пилила»:
                // ПК упирался в процессор, лесенка поднимала частоту, снова упиралась и снова падала (отсюда «то 40, то ещё что-то»)
                if (cpuLag) { m_encCeilFps = qMax(20, m_capture.captureFps()); m_encCeilUntil = tNow + 60000; m_cpuLagSeconds = 0; }
                if ((++m_badSeconds >= 3 || severe) && m_level < steps - 1) {
                    // подъём на эту ступень только что был и сорвался — в следующий раз на неё поднимаемся осторожнее (дольше ждём)
                    if (m_levelSince > 0 && tNow - m_levelSince < 15000) m_climbPenalty[m_level] = qMin(36, m_climbPenalty[m_level] * 2 + 3);
                    m_level = qMin(steps - 1, m_level + (severe ? 2 : 1));
                    m_levelSince = tNow; m_badSeconds = 0; applyLevel();
                }
            } else if (fast) {
                m_badSeconds = 0;
                // подъём — на одну ступень после трёх спокойных секунд плюс «штраф» этой ступени за прошлые срывы
                const int need = (vid ? 6 : 3) + (m_level > 0 ? m_climbPenalty[m_level - 1] : 0);      // у видео подъём реже: каждый — перезапуск кодера
                if (++m_goodSeconds >= need && m_level > 0) { --m_level; m_levelSince = tNow; m_goodSeconds = 0; applyLevel(); }
                // долгая стабильность прощает прошлые срывы: штрафы медленно сходят на нет
                if (tNow - m_levelSince > 30000 && m_goodSeconds % 10 == 9)
                    for (int &pen : m_climbPenalty) pen = qMax(0, pen - 1);
            } else { m_badSeconds = 0; m_goodSeconds = 0; }
            m_queueBlocked = false;
            // экран не меняется — захват кадров не шлёт; последний кадр повторяется, чтобы зритель видел, что связь жива
            if (nowMs() - m_lastSentMs > 1800 && m_lastSentW > 0 && canSendFrame()) {
                m_outRegion = m_lastSentReg; m_outSource = m_lastSentSrc;
                sendFrame({}, m_lastSentW, m_lastSentH, false);               // пустое обновление — «связь жива, экран не менялся»
            }
        }
        if (!m_sharingTo.isEmpty()) applyLevel();           // скорость разжатия у зрителя могла измениться
        emit statsChanged();
    });
    m_statsTimer.start();
    connect(m_settings, &SettingsManager::streamChanged, this, [this]() { if (!m_sharingTo.isEmpty()) { m_capture.setEncoderPreference(m_settings->streamEncoder()); chooseFormat(); applyLevel(); } });
    m_videoTimer.setSingleShot(true);
    connect(&m_videoTimer, &QTimer::timeout, this, &RemoteDesktopController::presentVideo);
    m_presentTimer.setSingleShot(true);
    connect(&m_presentTimer, &QTimer::timeout, this, &RemoteDesktopController::presentTick);

    // Android: кадры делает Java-служба в своём потоке — ей нужны готовые числа, обращаться к Qt-объектам оттуда нельзя
    AndroidCapture::attach(this);
    m_paramsTimer.setInterval(80);
    connect(&m_paramsTimer, &QTimer::timeout, this, [this]() {
        const bool can = !m_sharingTo.isEmpty() && canSendFrame();
        AndroidCapture::setParams(m_capture.jpegQuality(), m_capture.targetFps(), can, m_capture.tilesMode(), m_capture.wantsVideo(), m_extBitrateKbps);
    });
#ifdef Q_OS_ANDROID
    m_paramsTimer.start();
#endif

    // зритель пропал: показ ждёт его до GRACE_MS (захват не останавливается, портал заново не спрашивается), потом заканчивается;
    // сам просмотр при обрыве тоже не сбрасывается — последний кадр остаётся на экране, связь восстанавливается
    m_graceTimer.setSingleShot(true);
    m_graceTimer.setInterval(GRACE_MS);
    connect(&m_graceTimer, &QTimer::timeout, this, [this]() { m_graceFor.clear(); stopSharing(); });
    m_reconnectTimeout.setSingleShot(true);
    m_reconnectTimeout.setInterval(RECONNECT_TIMEOUT_MS);
    connect(&m_reconnectTimeout, &QTimer::timeout, this, [this]() { endViewing(); });      // и второй экран тоже: раньше флаг оставался включённым

    connect(manager, &PeerManager::deviceDisconnected, this, [this](const QString &deviceId) {
        if (m_sharingTo == deviceId) {
            m_graceFor = deviceId;
            // второй экран: пропавшее устройство ждут недолго (своя настройка), потом виртуальный монитор убирается, а окна и курсор возвращаются на основной
            const bool second = m_ssFps > 0;
            m_graceTimer.setInterval(second ? m_settings->secondGraceSec() * 1000 : GRACE_MS);
            m_graceTimer.start();
            if (second) { ScreenCaptureSession::log(QStringLiteral("Второй экран: связь с %1 потеряна, монитор вернём через %2 с, если оно не вернётся").arg(who(deviceId)).arg(m_settings->secondGraceSec())); emit errorOccurred(QStringLiteral("Связь со вторым экраном потеряна: монитор вернётся на основной через %1 с, если устройство не вернётся").arg(m_settings->secondGraceSec())); }
        }
        if (m_inputArmedFor == deviceId) stopInputSession();
        if (m_viewing == deviceId) { m_reconnecting = true; m_reconnectTimeout.start(); emit viewingChanged(); }
        if (m_offeredTo == deviceId) m_offeredTo.clear();
    });
    connect(manager, &PeerManager::deviceConnected, this, [this](const QString &deviceId, const QString &) {
        m_videoFails = 0;      // устройство могли обновить: видео снова пробуется (счётчик сбоев не вечный)
        if (m_graceFor == deviceId) { m_graceFor.clear(); m_graceTimer.stop(); }
        if (m_viewing == deviceId && m_reconnecting) QTimer::singleShot(700, this, [this]() { resumeViewing(); });
    });
}

// умеет ли это устройство разжимать видео H.264 (Android — аппаратным декодером MediaCodec)
// умеет ли это устройство разжимать видео H.264: на телефоне — аппаратный декодер MediaCodec, на ПК — libavcodec (если сборка с ней)
static bool canDecodeH264()
{
#ifdef Q_OS_ANDROID
    return PlatformBridge::hasHardwareVideoDecoder();
#else
    return VideoDecoder::available();
#endif
}

// умеет ли это устройство кодировать свой экран в H.264: ПК — через wf-recorder (видеокарта или x264), телефон — аппаратным MediaCodec
static bool canEncodeH264()
{
#if defined(Q_OS_ANDROID)
    return true;
#elif defined(ORBIT_DESKTOP_LINUX)
    return QGuiApplication::platformName() == QLatin1String("wayland") && !QStandardPaths::findExecutable(QStringLiteral("wf-recorder")).isEmpty();
#else
    return false;
#endif
}

// сколько пикселей в ширину у моего экрана — пригодится показывающему, чтобы не слать лишнего (длинная сторона: экран можно повернуть)
static int myScreenLongSide()
{
    const QScreen *sc = QGuiApplication::primaryScreen();
    if (!sc) return 0;
    const QSize s = sc->size();
    return int(qMax(s.width(), s.height()) * sc->devicePixelRatio());
}

bool RemoteDesktopController::canSendFrame()
{
    if (m_sharingTo.isEmpty() || !m_manager->isConnected(m_sharingTo)) return false;
    const qint64 now = nowMs();
    const bool inflightOk = (m_sendSeq - m_ackedSeq) < 10 || now - m_lastAckMs > 2500;     // после тишины пропускаем один кадр, чтобы не застрять навсегда
    // очередь сокета допускается около четырёх обычных сообщений (но не меньше 300 КБ): раньше предел был 160 КБ, а один целый кадр
    // весит больше, поэтому очередь «переполнялась» всегда, и «лесенка» качества сама сползала на самую низкую ступень
    const bool queueOk = m_manager->backlog(m_sharingTo) < qint64(qMax(300.0 * 1024, 4 * m_avgMsgBytes));
    if (!queueOk) m_queueBlocked = true;          // очередь сокета — признак перегруженной сети; ожидание подтверждений — обычная работа окна
    return inflightOk && queueOk;
}

bool RemoteDesktopController::videoLadder() const { return m_capture.videoMode() || (m_viewerH264 && m_videoFails < 2 && !m_sharingTo.isEmpty()); }
int RemoteDesktopController::ladderSize() const { return videoLadder() ? kVideoLevelCount : kLevelCount; }

// Какой формат пойдёт этому зрителю: видео H.264 (если он его умеет и оно у него не ломалось), «Тайлы Orbit» (универсальный — любому с новой версией),
// JPEG — только по явному выбору или если зритель не знает тайлов. «Авто» — видео, иначе тайлы
// Формат показа МОЕГО экрана выбирает МОЯ настройка («Формат показа моего экрана»); зритель только сообщает, что он умеет разжимать.
// Видео — если я выбрал «Авто»/«H.264», умею кодировать, зритель умеет разжимать и у него видео не ломалось; иначе тайлы (если зритель их знает);
// JPEG — по явному выбору или для старого зрителя. Смена настройки во время показа применяется сразу
void RemoteDesktopController::chooseFormat()
{
    const QString f = m_settings->streamFormat();
    const bool videoOk = m_viewerH264 && m_videoFails < 2 && canEncodeH264();
    const bool video = videoOk && (f == QLatin1String("auto") || f == QLatin1String("h264"));
    const bool tiles = !video && m_viewerTiles && f != QLatin1String("jpeg");
    if (!video) m_extVideo = false;
    m_capture.setVideoMode(video);
    m_capture.setTilesMode(tiles);
    ScreenCaptureSession::log(QStringLiteral("Формат показа: %1 (зритель %2 H.264; видео %3, тайлы %4)").arg(f, m_viewerH264 ? QStringLiteral("умеет") : QStringLiteral("не умеет"), video ? QStringLiteral("включено") : QStringLiteral("выключено"), tiles ? QStringLiteral("включены") : QStringLiteral("выключены")));
    m_videoWhy = video ? QString()
               : (f == QLatin1String("jpeg") ? QStringLiteral("выбран JPEG")
               : (f == QLatin1String("tiles") ? QStringLiteral("выбраны тайлы")
               : (!m_viewerH264 ? QStringLiteral("зритель не умеет разжимать H.264 (на ПК нужна сборка с libavcodec)")
               : (m_videoFails >= 2 ? QStringLiteral("у зрителя дважды не заработал декодер") : QStringLiteral("это устройство не умеет кодировать H.264")))));
}

QVariantMap RemoteDesktopController::viewStats() const
{
    QVariantMap m;
    if (m_viewing.isEmpty()) return m;
    m[QStringLiteral("codec")] = m_rxCodec.isEmpty() ? QStringLiteral("—") : m_rxCodec;
    m[QStringLiteral("w")] = m_frameW; m[QStringLiteral("h")] = m_frameH;
    m[QStringLiteral("fps")] = m_viewFps;                // пришло кадров за секунду
    m[QStringLiteral("shown")] = m_shownFps;             // показано на экране
    m[QStringLiteral("kbit")] = m_rxKbit;
    m[QStringLiteral("bufferMs")] = m_rxCodec.startsWith(QLatin1String("H.264")) ? m_videoPlayMs : int(m_playoutMs);
    m[QStringLiteral("ageMs")] = frameAgeMs();
    m[QStringLiteral("level")] = m_senderLevel; m[QStringLiteral("steps")] = m_senderSteps; m[QStringLiteral("capFps")] = m_senderCapFps;
    QVariantList hist;
    for (int v : m_fpsHistory) hist.append(v);
    m[QStringLiteral("history")] = hist;
    return m;
}

QVariantList RemoteDesktopController::availableFormats() const
{
    const bool enc = canEncodeH264();
    QVariantList out;
    auto add = [&out](const QString &id, const QString &name, bool ok, const QString &note) {
        QVariantMap m; m[QStringLiteral("id")] = id; m[QStringLiteral("name")] = name; m[QStringLiteral("ok")] = ok; m[QStringLiteral("note")] = note;
        out.append(m);
    };
    add(QStringLiteral("auto"), QStringLiteral("Авто"), true,
        enc ? QStringLiteral("видео H.264, если зритель умеет его разжимать, иначе «Тайлы Orbit»") : QStringLiteral("«Тайлы Orbit»: это устройство не кодирует H.264"));
    add(QStringLiteral("h264"), QStringLiteral("Видео H.264"), enc,
        enc ? QStringLiteral("плавнее всего при движении, лёгкий поток; у зрителя без декодера H.264 будут тайлы") : QStringLiteral("недоступно: нет кодера (на ПК нужен wf-recorder под Wayland)"));
    add(QStringLiteral("tiles"), QStringLiteral("Тайлы Orbit"), true,
        QStringLiteral("универсальный: любой зритель; текст и интерфейс без потерь, в покое идеально чётко, обновляются только изменившиеся участки"));
    add(QStringLiteral("jpeg"), QStringLiteral("JPEG"), true, QStringLiteral("простой запасной: кадры целиком, поток тяжелее"));
    return out;
}

QVariantList RemoteDesktopController::technologies() const
{
    QVariantList out;
    auto add = [&out](const QString &name, bool on, const QString &note) {
        QVariantMap m; m[QStringLiteral("name")] = name; m[QStringLiteral("on")] = on; m[QStringLiteral("note")] = note;
        out.append(m);
    };
    const bool sharing = !m_sharingTo.isEmpty(), viewing = !m_viewing.isEmpty();
    const bool h264 = (sharing && m_txCodec.startsWith(QLatin1String("H.264"))) || (viewing && m_rxCodec == QLatin1String("H.264"));
    add(QStringLiteral("Видео H.264"), h264,
        h264 ? QStringLiteral("между кадрами идёт только разница: поток в 10–20 раз легче JPEG")
             : (sharing || viewing ? QStringLiteral("сейчас JPEG: ") + (m_videoWhy.isEmpty() ? QStringLiteral("видео для этой пары не включилось") : m_videoWhy)
                                   : QStringLiteral("включается само, когда экран ПК смотрит телефон")));
#ifdef Q_OS_ANDROID
    const bool tilesNow = (sharing && m_capture.tilesMode()) || (viewing && m_rxCodec == QLatin1String("Тайлы Orbit"));
    add(QStringLiteral("Тайлы Orbit (универсальный формат)"), tilesNow,
        tilesNow ? QStringLiteral("участки 64×64: интерфейс и текст без потерь, фото и видео — JPEG, в покое картинка доводится до идеальной чёткости")
                 : QStringLiteral("включается сам, когда у зрителя нет видео, или по выбору в настройках зрителя"));
    add(QStringLiteral("Кодирование на видеокарте (VAAPI)"), sharing && m_capture.gpuEncoding(),
        sharing && m_capture.gpuEncoding() ? QStringLiteral("полное разрешение экрана при нагрузке на процессор в единицы процентов")
                                           : QStringLiteral("Intel/AMD; если не заработает — процессор (x264) сам; выбор в «Кодер»"));
    add(QStringLiteral("Декодер видео (MediaCodec на телефоне, libavcodec на ПК)"), viewing && h264, QStringLiteral("видеочип разжимает кадр за единицы миллисекунд, процессор свободен"));
#else
    add(QStringLiteral("Декодер видео (MediaCodec на телефоне, libavcodec на ПК)"), viewing && h264, VideoDecoder::available() ? QStringLiteral("libavcodec разжимает видео в режиме малой задержки, цвета переводит видеокарта") : QStringLiteral("в этой сборке нет libavcodec — видео на ПК недоступно"));
#endif
    add(QStringLiteral("Лесенка частоты и качества"), m_settings->streamAuto(),
        m_settings->streamAuto() ? QStringLiteral("ступень %1 из %2, сейчас просит %3 к/с — шаги 60 · 48 · 36 · 30 · 24 · 20 · 15 · 12 · 8").arg(m_level + 1).arg(ladderSize()).arg(m_capture.targetFps())
                                 : QStringLiteral("выключена: значения неизменны"));
    add(QStringLiteral("Осторожный подъём"), m_settings->streamAuto(), QStringLiteral("после срыва на ступени подъём на неё требует больше спокойных секунд — частота не «пилит»"));
    add(QStringLiteral("Выравнивающий буфер"), viewing, QStringLiteral("кадры выводятся по времени съёмки, короткая задержка по разбросу прихода, лишние пропускаются"));
    add(QStringLiteral("Параллельное разжатие"), viewing, QStringLiteral("целые кадры разжимаются в нескольких потоках, остальное — по порядку"));
    add(QStringLiteral("Сеть без задержки (TCP_NODELAY)"), true, QStringLiteral("сообщения не ждут накопления сегмента: до 40 мс меньше на кадр"));
    add(QStringLiteral("Шифрование канала"), true, QStringLiteral("X25519, подпись каждого блока с заголовком, защита от повтора и подмены"));
    return out;
}

void RemoteDesktopController::applyLevel()
{
    // потолки задаёт человек (Настройки → «Показ экрана»): частота, качество JPEG, ширина кадра; ступень «лесенки» опускает от них
    static const double fpsFactor[] = {1.0, 0.8, 0.65, 0.5, 0.4, 0.3, 0.22, 0.16};
    static const int qualityDrop[] = {0, 4, 10, 16, 20, 24, 28, 32};
    // у «второго экрана» частота и ширина кадра — те, что попросил зритель (полное разрешение монитора), а не общие настройки показа
    const int fpsCap = m_ssFps > 0 ? m_ssFps : m_settings->streamFps(), qualityCap = m_settings->streamQuality(), widthCap = m_ssWidth > 0 ? m_ssWidth : m_settings->streamWidth();
    // ширина: задана явно — она; иначе по окну зрителя (нет данных — 1600), но не больше 1920 и не меньше 800
    const int base = widthCap > 0 ? widthCap : qBound(800, m_viewerWidth > 0 ? m_viewerWidth : 1600, 1920);

    // зритель разжимает один кадр за m_peerDecodeMs: слать чаще — только забивать ему очередь (задержка и «моргание»)
    const int decodeFpsCap = m_peerDecodeMs > 0 ? qMax(5, int(1000.0 / (m_peerDecodeMs * 1.25))) : 60;
    if (!m_settings->streamAuto()) {                       // ручной режим: значения неизменны
        m_capture.setMaxWidth(base);
        m_capture.setJpegQuality(qualityCap);
        m_capture.setTargetFps(qMin(fpsCap, decodeFpsCap));
        return;
    }
    if (videoLadder()) {
        const VLevel &vl = kVideo[qBound(0, m_level, kVideoLevelCount - 1)];
        // второй экран — это монитор: текст должен оставаться чётким, поэтому при плохой сети снижается частота, а размер не трогается,
        // и качество проседает вдвое мягче
        const bool monitor = m_ssFps > 0;
        m_capture.setMaxWidth(qMax(640, int(base * (monitor ? 1.0 : vl.scale))));
        m_capture.setJpegQuality(qMax(30, qualityCap - vl.crfAdd * (monitor ? 2 : 4)));
        static const int kExtKbps[] = {9000, 7500, 6000, 5000, 4000, 3200, 2500, 1800, 1200};       // скорость кодера телефона по ступеням лесенки
        m_extBitrateKbps = kExtKbps[qBound(0, m_level, kVideoLevelCount - 1)] * qBound(30, qualityCap, 95) / 75;
        int vfps = qMin(fpsCap, vl.fps);
        if (m_encCeilUntil > nowMs()) vfps = qMin(vfps, m_encCeilFps);
        m_capture.setTargetFps(qBound(8, vfps, 60));
        return;
    }
    const int lv = qBound(0, m_level, kLevelCount - 1);
    m_capture.setMaxWidth(qMax(640, int(base * kLevels[lv].scale)));
    m_capture.setJpegQuality(qMax(30, qualityCap - qualityDrop[lv]));
    m_capture.setTargetFps(qMin(decodeFpsCap, qMax(5, int(fpsCap * fpsFactor[lv] + 0.5))));
}

void RemoteDesktopController::updateBackgroundHold()
{
    // Android: пока идёт просмотр или показ, процесс не должен засыпать (процессор и Wi-Fi), а просмотр — уметь жить в маленьком окне
    const bool busy = !m_viewing.isEmpty() || !m_sharingTo.isEmpty();
    PlatformBridge::keepAwake(busy);
    PlatformBridge::setPictureInPicture(!m_viewing.isEmpty(), m_frameW, m_frameH);
}

int RemoteDesktopController::frameAgeMs() const
{
    return m_lastFrameRxMs > 0 && !m_viewing.isEmpty() ? int(qMin<qint64>(nowMs() - m_lastFrameRxMs, 600000)) : -1;
}

void RemoteDesktopController::setViewerWidth(int pixels)
{
    if (m_viewing.isEmpty() || pixels < 200) return;
    if (m_viewerWidthSent > 0 && qAbs(pixels - m_viewerWidthSent) * 10 < m_viewerWidthSent) return;       // изменение меньше 10 % не стоит сообщения
    m_viewerWidthSent = pixels;
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("viewerSize"); p[QStringLiteral("vw")] = pixels;
    sendToPeer(m_viewing, p);
}

void RemoteDesktopController::enterPip() { PlatformBridge::enterPictureInPicture(); }
void RemoteDesktopController::setFullscreenView(bool on, int orientation) { PlatformBridge::setViewerFullscreen(on, orientation); }

void RemoteDesktopController::resumeViewing()
{
    if (m_viewing.isEmpty() || !m_reconnecting || !m_manager->isConnected(m_viewing)) return;
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("shareRequest"); p[QStringLiteral("vw")] = myScreenLongSide(); p[QStringLiteral("h264")] = canDecodeH264(); p[QStringLiteral("tiles")] = true;
    sendToPeer(m_viewing, p);
}

bool RemoteDesktopController::captureAvailable() const
{
#if defined(Q_OS_IOS)
    return false;      // iOS не даёт приложению захватывать экран; Android показывает экран через свою службу (OrbitCaptureService)
#else
    return true;
#endif
}

QQuickImageProvider *RemoteDesktopController::createImageProvider() { return new ImageProvider(this); }

bool RemoteDesktopController::featureEnabled() const
{
    return m_settings->remoteDesktopFeatureEnabled();
}

int RemoteDesktopController::inputRemainingSeconds() const
{
    if (m_inputArmedFor.isEmpty()) return 0;
    const int limit = m_settings->inputSessionTimeoutMin();
    if (limit <= 0) return -1;   // без ограничения
    return qMax(0, int(limit) * 60 - int(m_inputArmedAt.elapsed() / 1000));
}

void RemoteDesktopController::sendToPeer(const QString &deviceId, const QJsonObject &payload)
{
    m_manager->sendPlugin(deviceId, name(), payload);
}

void RemoteDesktopController::sendStatus(const QString &deviceId, const QString &text)
{
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("shareStatus"); p[QStringLiteral("text")] = text;
    sendToPeer(deviceId, p);
}

// просмотр закончился по решению другой стороны (отказ, ошибка, остановка): без ответного сообщения ей
void RemoteDesktopController::endViewing()
{
    if (m_viewing.isEmpty()) return;
    if (m_kbGrab.active()) { m_kbGrab.release(); emit keyboardGrabChanged(); }
    m_viewing.clear(); m_viewNote.clear();
    m_reconnecting = false; m_reconnectTimeout.stop();
    m_frameSeq = 0;
    if (m_audioWant) { m_audioWant = false; m_audioIn.stopPlayback(); m_audioState.clear(); emit audioStateChanged(); }
    if (m_secondViewing) { m_secondViewing = false; emit secondScreenChanged(); }
    clearCanvas();
    emit viewingChanged(); emit frameUpdated();
    updateBackgroundHold();
}

// ---------- просмотр чужого экрана ----------

void RemoteDesktopController::requestShare(const QString &deviceId)
{
    m_audioOverride = QStringLiteral("auto"); m_audioCodecPref.clear();      // выбор звука из окна второго экрана относится только к нему
    // смотреть чужой экран можно всегда (в том числе с телефона, где главный выключатель скрыт): решает тот, чей это экран.
    // Уже смотрим этот же экран — ничего не делаем; другой экран или второй экран — переключаемся, а не молчим
    if (m_viewing == deviceId && !m_secondViewing) return;
    if (!m_viewing.isEmpty()) stopViewing();
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("shareRequest"); p[QStringLiteral("vw")] = myScreenLongSide(); p[QStringLiteral("h264")] = canDecodeH264(); p[QStringLiteral("tiles")] = true;
    sendToPeer(deviceId, p);
    beginViewing(deviceId);
}

void RemoteDesktopController::beginViewing(const QString &deviceId)
{
    m_viewing = deviceId;    // UI сразу показывает «ожидание ответа…»; кадры придут, когда другая сторона согласится
    m_viewStage = 0;
    m_hostVolume = 1.0; m_hostMuted = false;
    m_viewNote.clear();
    ScreenCaptureSession::log(QStringLiteral("Смотрим экран устройства %1: запрос отправлен").arg(who(deviceId)));
    m_reconnecting = false;
    m_controlState = 0;
    m_minTransit = 1e12; m_jitter = 10; m_playoutMs = 60;
    m_viewerWidthSent = 0; m_lastFrameRxMs = 0; m_lastRegionSent = QRectF(0, 0, 1, 1); m_inRegion = QRectF(0, 0, 1, 1);
    emit viewingChanged();
    updateBackgroundHold();
    // другая сторона так и не начала показ (запрос потерялся, окно разрешения закрыли, на той стороне старая версия): страница не висит вечно с «Ждём…»
    const int gen = ++m_viewGen;      // таймер относится к этой попытке: после остановки и нового запроса старый не должен сбросить новую
    QTimer::singleShot(45000, this, [this, deviceId, gen]() {
        if (gen == m_viewGen && m_viewing == deviceId && m_frameSeq == 0 && !m_reconnecting) {
            ScreenCaptureSession::log(QStringLiteral("От %1 за 45 секунд не пришло ни одного кадра — просмотр закрыт").arg(who(deviceId)));
            stopViewing();
            emit errorOccurred(QStringLiteral("Устройство не начало показ за 45 секунд: проверьте, что на нём разрешён показ экрана и запущена та же версия Orbit"));
        }
    });
}

// ---------- второй экран ----------

void RemoteDesktopController::requestSecondScreen(const QString &deviceId, const QVariantMap &spec)
{
    if (m_viewing == deviceId && m_secondViewing) return;
    if (!m_viewing.isEmpty()) stopViewing();          // идёт обычный просмотр — второй экран его заменяет
    // размер экрана этого устройства (физические пиксели) → размер второго экрана: ориентация, разрешение, предел 2560×1600 с сохранением пропорций
    const QScreen *sc = QGuiApplication::primaryScreen();
    QSize px = sc ? sc->size() * sc->devicePixelRatio() : QSize(1920, 1080);
    const int longS = qMax(px.width(), px.height()), shortS = qMax(1, qMin(px.width(), px.height()));
    const QString orient = spec.value(QStringLiteral("orientation"), QStringLiteral("auto")).toString();
    const bool landscape = orient == QLatin1String("landscape") || (orient != QLatin1String("portrait") && px.width() >= px.height());
    double aspect = double(longS) / shortS;
    int s = shortS, l = longS;
    const QString res = spec.value(QStringLiteral("res"), QStringLiteral("native")).toString();
    if (res == QLatin1String("1080") && s > 1080) { s = 1080; l = int(s * aspect); }
    if (res == QLatin1String("720") && s > 720) { s = 720; l = int(s * aspect); }
    if (l > 2560) { l = 2560; s = int(l / aspect); }
    if (s > 1600) { s = 1600; l = int(s * aspect); }
    l -= l % 12; s -= s % 12;                                  // кратно 12: на такой размер делятся нацело масштабы 1,5 и 2 (иначе Hyprland подставит свой)
    SecondSpec ss;
    ss.w = landscape ? l : s; ss.h = landscape ? s : l;
    ss.fps = qBound(24, spec.value(QStringLiteral("fps"), 60).toInt(), 120);
    ss.side = spec.value(QStringLiteral("side"), QStringLiteral("right")).toString();
    // масштаб монитора по размеру экрана этого устройства: на телефоне 1078 px по короткой стороне — это интерфейс размером с мышиную точку,
    // поэтому короткая сторона приводится примерно к 720 логическим пикселям (шаг 0,25, от 1 до 2)
    ss.scale = qBound(1.0, qRound(qMin(ss.w, ss.h) / 720.0 * 4) / 4.0, 2.0);
    const double chosen = spec.value(QStringLiteral("scale")).toString().toDouble();          // выбран в окне запроса; «auto» — 0
    if (chosen >= 1.0) ss.scale = qBound(1.0, chosen, 3.0);
    ss.scaleAuto = chosen < 1.0;
    ss.mode = spec.value(QStringLiteral("mode"), QStringLiteral("extend")).toString() == QLatin1String("mirror") ? QStringLiteral("mirror") : QStringLiteral("extend");
    ss.workspace = spec.value(QStringLiteral("workspace")).toString();
    m_audioOverride = spec.value(QStringLiteral("audio"), QStringLiteral("auto")).toString();
    m_audioCodecPref = spec.value(QStringLiteral("acodec")).toString();

    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("secondScreenRequest");
    p[QStringLiteral("w")] = ss.w; p[QStringLiteral("h")] = ss.h; p[QStringLiteral("fps")] = ss.fps; p[QStringLiteral("side")] = ss.side; p[QStringLiteral("scale")] = ss.scale; p[QStringLiteral("scaleAuto")] = ss.scaleAuto; p[QStringLiteral("mode")] = ss.mode; p[QStringLiteral("workspace")] = ss.workspace;
    p[QStringLiteral("vw")] = qMax(ss.w, ss.h); p[QStringLiteral("h264")] = canDecodeH264(); p[QStringLiteral("tiles")] = true;
    sendToPeer(deviceId, p);
    m_secondViewing = true;
    m_secondOrient = landscape ? 1 : 2;
    beginViewing(deviceId);
    emit secondScreenChanged();
}

void RemoteDesktopController::offerSecondScreen(const QString &deviceId, const QVariantMap &spec, const QString &windowChoice)
{
    if (!featureEnabled() || !captureAvailable() || VirtualDisplay::backend() == VirtualDisplay::Backend::None) {
        emit errorOccurred(QStringLiteral("На этом компьютере второй экран недоступен: нужен Hyprland (или driftwm, бета) и включённый показ экрана"));
        return;
    }
    if (VirtualDisplay::backend() == VirtualDisplay::Backend::Driftwm && windowChoice.toInt() <= 0) {
        emit errorOccurred(QStringLiteral("Выберите окно приложения: у driftwm (бета) вторым экраном становится окно"));
        return;
    }
    m_ssOffered.insert(deviceId, windowChoice);
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("secondScreenOffer");
    for (const char *k : {"orientation", "res", "side", "scale", "mode", "workspace", "audio", "acodec"}) p[QString::fromLatin1(k)] = spec.value(QString::fromLatin1(k)).toString();
    p[QStringLiteral("fps")] = spec.value(QStringLiteral("fps"), 60).toInt();
    p[QStringLiteral("beta")] = VirtualDisplay::backend() == VirtualDisplay::Backend::Driftwm;
    sendToPeer(deviceId, p);
}

void RemoteDesktopController::acceptSecondScreenOffer(const QString &deviceId)
{
    if (!m_ssOfferSpec.contains(deviceId)) return;
    requestSecondScreen(deviceId, m_ssOfferSpec.take(deviceId));
}

void RemoteDesktopController::declineSecondScreenOffer(const QString &deviceId)
{
    m_ssOfferSpec.remove(deviceId);
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("secondScreenDenied"); p[QStringLiteral("reason")] = QStringLiteral("устройство отказалось быть вторым экраном");
    sendToPeer(deviceId, p);
}

void RemoteDesktopController::declineSecondScreen(const QString &deviceId, const QString &reason)
{
    ScreenCaptureSession::log(QStringLiteral("Второй экран для %1 не включён: %2").arg(who(deviceId), reason));
    m_ssPending.remove(deviceId);
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("secondScreenDenied"); p[QStringLiteral("reason")] = reason;
    sendToPeer(deviceId, p);
}

void RemoteDesktopController::acceptSecondScreen(const QString &deviceId, const QString &windowChoice)
{
    const auto it = m_ssPending.constFind(deviceId);
    if (it == m_ssPending.constEnd()) return;
    const SecondSpec spec = it.value();
    m_ssPending.remove(deviceId);
    if (!m_sharingTo.isEmpty() && m_sharingTo != deviceId) stopSharing();          // иначе остановка потом снесла бы новый монитор
    QString source, err;
    m_ssMirror = false;
    if (VirtualDisplay::backend() == VirtualDisplay::Backend::Hyprland && spec.mode == QLatin1String("mirror")) {
        // повтор существующего монитора: выбранный в запросе (или в настройках), иначе первый; новый монитор не создаётся, касания идут на него же
        QString mon = windowChoice.isEmpty() ? m_settings->secondAnchor() : windowChoice;
        const QStringList mons = VirtualDisplay::monitorNames();
        if (!mons.contains(mon)) mon = mons.value(0);
        if (mon.isEmpty()) { declineSecondScreen(deviceId, QStringLiteral("не найден монитор для повтора")); return; }
        m_ssMirror = true;
        m_ssOutput.clear();
        source = mon;
        m_capture.setVirtualSize(QSize());
        m_input.setTarget(mon);
        m_shareSource = mon;
    } else if (VirtualDisplay::backend() == VirtualDisplay::Backend::Hyprland) {
        VirtualDisplay::Spec vs; vs.width = spec.w; vs.height = spec.h; vs.fps = spec.fps; vs.side = spec.side; vs.scale = spec.scale;
        vs.workspace = spec.workspace.isEmpty() ? m_settings->secondWorkspace() : (spec.workspace == QLatin1String("own") ? QString() : spec.workspace);      // выбран на устройстве: «own» — отдельный стол, число — стол 1–10
        vs.anchor = windowChoice.isEmpty() ? m_settings->secondAnchor() : windowChoice;   // для Hyprland выбор в окне запроса — имя монитора
        if (!VirtualDisplay::create(vs, &m_ssOutput, &err)) { m_ssOutput.clear(); declineSecondScreen(deviceId, err); return; }
        source = m_ssOutput;
        m_ssWorkspace = vs.workspace;
        if (!m_ssKeep.isSingleShot() && m_ssKeep.interval() != 1000) {
            m_ssKeep.setInterval(1000);
            connect(&m_ssKeep, &QTimer::timeout, this, [this] { VirtualDisplay::keepWorkspace(m_ssOutput, m_ssWorkspace); });
        }
        m_ssKeep.start();
        m_capture.setVirtualSize(QSize(spec.w, spec.h));
        m_input.setTarget(m_ssOutput);                                             // касания зрителя попадают на этот монитор
    } else if (VirtualDisplay::backend() == VirtualDisplay::Backend::Driftwm) {
        // БЕТА: вторым экраном становится окно приложения, подогнанное под размер экрана зрителя
        const int wid = windowChoice.toInt();
        QRect rect; QString outName;
        if (wid <= 0 || !VirtualDisplay::driftwmPrepare(wid, spec.w, spec.h, &rect, &outName, &err)) { declineSecondScreen(deviceId, err.isEmpty() ? QStringLiteral("окно не выбрано") : err); return; }
        QRect orect;
        for (const QScreen *s : QGuiApplication::screens()) if (s->name() == outName) orect = s->geometry();
        if (orect.isEmpty()) { declineSecondScreen(deviceId, QStringLiteral("не найден экран окна")); return; }
        source = QStringLiteral("dwm:") + QString::number(wid);
        m_input.setTarget(QString(), QRectF(double(rect.x() - orect.x()) / orect.width(), double(rect.y() - orect.y()) / orect.height(),
                                           double(rect.width()) / orect.width(), double(rect.height()) / orect.height()));
    } else { declineSecondScreen(deviceId, QStringLiteral("второй экран поддерживают Hyprland и driftwm (бета)")); return; }
    m_ssFps = spec.fps; m_ssWidth = spec.w;
    ScreenCaptureSession::log(QStringLiteral("Второй экран для %1: %2×%3, %4 к/с, источник — %5").arg(who(deviceId)).arg(spec.w).arg(spec.h).arg(spec.fps).arg(source));
    acceptShareRequest(deviceId, source);
    armInput(deviceId, true);                                                      // касания и клавиатура включены: для второго экрана это и нужно
}

void RemoteDesktopController::releaseSecondScreen()
{
    m_ssKeep.stop();
    if (!m_ssOutput.isEmpty()) { VirtualDisplay::destroy(m_ssOutput); m_ssOutput.clear(); }
    if (m_ssFps > 0) { m_capture.setScreenName(QString()); m_capture.setVirtualSize(QSize()); m_input.setTarget(QString()); }
    m_ssFps = 0; m_ssWidth = 0; m_ssMirror = false;
}

void RemoteDesktopController::offerShare(const QString &deviceId, const QString &source)
{
    if (!featureEnabled() || !captureAvailable() || !m_sharingTo.isEmpty()) return;
    // что показывать, спрашивается ДО предложения: иначе принятый показ молча снимал бы первый экран
    if (source.isEmpty() && !availableScreens().isEmpty()) {
        emit sourceChoiceNeeded(deviceId, m_manager->trustStore()->device(deviceId).name);
        return;
    }
    m_offeredSource = source;
    m_offeredTo = deviceId;
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("shareOffer");
    sendToPeer(deviceId, p);
}

void RemoteDesktopController::stopViewing()
{
    if (m_viewing.isEmpty()) return;
    setKeyboardGrab(false);                 // зажатые клавиши отпускаются у той стороны, пока она ещё на связи
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("shareStop"); p[QStringLiteral("by")] = QStringLiteral("viewer");
    sendToPeer(m_viewing, p);
    m_viewing.clear();
    m_viewNote.clear();
    m_reconnecting = false;
    m_reconnectTimeout.stop();
    m_frameSeq = 0;
    if (m_secondViewing) { m_secondViewing = false; emit secondScreenChanged(); }
    clearCanvas();
    emit viewingChanged();
    emit frameUpdated();
    updateBackgroundHold();
}

// ---------- демонстрация своего экрана ----------

QVariantList RemoteDesktopController::availableScreens() const
{
    QVariantList out;
#ifndef Q_OS_ANDROID
    const QScreen *primary = QGuiApplication::primaryScreen();
    for (const QScreen *s : QGuiApplication::screens()) {
        QVariantMap m;
        m[QStringLiteral("name")] = s->name();
        const QSize px = s->size() * s->devicePixelRatio();
        m[QStringLiteral("label")] = QStringLiteral("Экран %1 · %2×%3%4").arg(s->name()).arg(px.width()).arg(px.height()).arg(s == primary ? QStringLiteral(" · основной") : QString());
        m[QStringLiteral("primary")] = (s == primary);
        out.append(m);
    }
    for (const QVariant &w : ScreenCaptureSession::visibleWindows()) {
        const QVariantMap wm = w.toMap();
        QVariantMap m;
        m[QStringLiteral("name")] = QStringLiteral("window:") + wm.value(QStringLiteral("address")).toString();
        m[QStringLiteral("label")] = QStringLiteral("Окно: ") + wm.value(QStringLiteral("label")).toString();
        m[QStringLiteral("primary")] = false;
        out.append(m);
    }
    QVariantMap portal;
    portal[QStringLiteral("name")] = QStringLiteral("portal");
    portal[QStringLiteral("label")] = QStringLiteral("Выбрать в системном окне (окно или экран)");
    portal[QStringLiteral("primary")] = false;
    out.append(portal);
#endif
    return out;
}

QVariantMap RemoteDesktopController::shareInfo() const
{
    QVariantMap m;
    const bool on = !m_sharingTo.isEmpty();
    m[QStringLiteral("active")] = on;
    if (!on) return m;
    const QString peer = m_manager->trustStore()->device(m_sharingTo).name;
    m[QStringLiteral("peer")] = peer.isEmpty() ? QStringLiteral("устройству") : peer;
    m[QStringLiteral("second")] = m_ssFps > 0;
    m[QStringLiteral("waiting")] = !(m_capture.isActive() || m_externalActive);          // захват ещё не начался: системное окно разрешения
    QString what;
    if (m_ssFps > 0) what = m_ssMirror ? QStringLiteral("повтор монитора %1").arg(m_shareSource) : QStringLiteral("второй экран (монитор %1)").arg(m_ssOutput.isEmpty() ? QStringLiteral("окна") : m_ssOutput);
#ifdef Q_OS_ANDROID
    else what = QStringLiteral("экран этого телефона");
#else
    else if (m_shareSource.startsWith(QLatin1String("window:")) || m_shareSource.startsWith(QLatin1String("dwm:"))) what = QStringLiteral("окно приложения");
    else if (m_shareSource.isEmpty()) what = QStringLiteral("основной экран");
    else what = QStringLiteral("экран %1").arg(m_shareSource);
#endif
    m[QStringLiteral("what")] = what;
    return m;
}

void RemoteDesktopController::acceptShareRequest(const QString &deviceId, const QString &screenName)
{
    m_shareSource = screenName;
    m_capture.setScreenName(screenName);
    if (!captureAvailable()) { declineShareRequest(deviceId); return; }
    if (!m_sharingTo.isEmpty() && m_sharingTo != deviceId) stopSharing();   // одна демонстрация единовременно
    m_sharingTo = deviceId;
    m_offeredTo.clear();
    // новый показ начинается со средней ступени, дальше она подстраивается под сеть и экран зрителя
    m_capture.setRegion(QRectF(0, 0, 1, 1)); m_outRegion = QRectF(0, 0, 1, 1); m_capture.requestKeyFrame();
    // начинаем с потолков, заданных человеком
    m_level = 0; m_levelSince = 0; m_encCeilFps = 0; m_encCeilUntil = 0; for (int &pen : m_climbPenalty) pen = 0; m_rttBase = 0; m_goodSeconds = 0; m_badSeconds = 0; m_dropsInPeriod = 0; m_calmSeconds = 0; m_queueBlocked = false;
    m_sendSeq = 0; m_ackedSeq = 0; m_lastAckMs = nowMs(); m_sentAt.clear(); m_rttMs = 0; m_peerDecodeMs = 0;
    m_capture.setEncoderPreference(m_settings->streamEncoder());
    chooseFormat();       // видео, «Тайлы Orbit» или JPEG — по моей настройке и возможностям зрителя
    ScreenCaptureSession::log(QStringLiteral("Начинаю показ для %1: источник — %2, видео H.264 %3, сбоев видео ранее: %4").arg(who(deviceId), screenName.isEmpty() ? QStringLiteral("основной экран") : screenName, (m_viewerH264 && m_videoFails < 2) ? QStringLiteral("будет использоваться") : QStringLiteral("не используется")).arg(m_videoFails));
    applyLevel();
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("shareAccepted");
    sendToPeer(deviceId, p);
    // системное окно выбора появляется только у портала и на телефоне; монитор, окно Hyprland и второй экран снимаются сразу
    const bool portal = screenName == QLatin1String("portal");
#ifdef Q_OS_ANDROID
    sendStatus(deviceId, QStringLiteral("На телефоне появится системное окно «Начать трансляцию» — подтвердите его"));
    AndroidCapture::request();      // системное окно «Начать демонстрацию экрана» — второе согласие
    const int firstFrameSec = 120;
#else
    sendStatus(deviceId, portal ? QStringLiteral("На компьютере открыто системное окно выбора экрана — выберите, что показать")
                                : (m_ssFps > 0 ? QStringLiteral("Монитор создан, запускаю захват…") : QStringLiteral("Запускаю захват экрана…")));
    m_capture.start();   // здесь показывается системный выбор экрана (xdg-desktop-portal) — второе согласие
    const int firstFrameSec = portal ? 120 : 15;
#endif
    // сторож первого кадра: захват молча не дал ни одного кадра (wf-recorder не запустился на мониторе, видеокарта, портал закрыт) —
    // зритель получает причину сразу, а не висит с пустым экраном, а виртуальный монитор убирается
    const int gen = ++m_shareGen;
    QTimer::singleShot(firstFrameSec * 1000, this, [this, deviceId, gen]() {
        if (gen != m_shareGen || m_sharingTo != deviceId || m_sendSeq > 0) return;
        const QString why = m_capture.errorString().isEmpty() ? QStringLiteral("захват экрана не дал ни одного кадра — проверьте wf-recorder и видеодрайвер (журнал orbit-video.log)")
                                                              : m_capture.errorString();
        ScreenCaptureSession::log(QStringLiteral("Показ для %1: первого кадра нет — %2").arg(who(deviceId), why));
        QJsonObject e; e[QStringLiteral("kind")] = QStringLiteral("shareError"); e[QStringLiteral("message")] = why;
        sendToPeer(deviceId, e);
        stopSharing();
        emit errorOccurred(QStringLiteral("Показ не начался: ") + why);
    });
    emit sharingChanged();
    updateBackgroundHold();
}

// как называть устройство в журнале: «Имя» (первые символы идентификатора)
QString RemoteDesktopController::who(const QString &deviceId) const
{
    const QString n = m_manager->trustStore()->device(deviceId).name;
    return n.isEmpty() ? deviceId.left(8) : QStringLiteral("«%1» (%2)").arg(n, deviceId.left(8));
}

void RemoteDesktopController::applyAudioSettings()
{
    // громкость повторяет выход показывающего: его доля по шкале системы (кубической, как у PulseAudio) → линейная, умноженная на свою
    // громкость приёма из настроек. «Без звука» там — тишина здесь
    const double host = m_hostMuted ? 0.0 : qMin(1.0, std::pow(qBound(0.0, m_hostVolume, 1.5), 3.0));
    m_audioIn.setVolume(host * m_settings->audioVolume() / 100.0);
    m_audioIn.setLatencyMs(m_settings->audioLatencyMs());
}

void RemoteDesktopController::stopAudioIn(const QString &state)
{
    if (m_audioWant && !m_viewing.isEmpty()) { QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("audioStop"); sendToPeer(m_viewing, p); }
    m_audioWant = false;
    m_audioIn.stopPlayback();
    m_audioCodecIn.clear();
    m_audioState = state;
    emit audioStateChanged();
}

void RemoteDesktopController::setKeyboardGrab(bool on)
{
    // захватывать есть смысл, только пока идёт второй экран и тот компьютер разрешил управление
    on = on && !m_viewing.isEmpty() && m_secondViewing && m_controlState == 2 && KeyboardGrab::available();
    if (on == m_kbGrab.active()) return;
    if (on && !m_kbGrab.grab()) {
        emit errorOccurred(QStringLiteral("Клавиатуру захватить не удалось: нужен доступ к /dev/input (группа input)"));
        return;
    }
    if (!on) m_kbGrab.release();
    emit keyboardGrabChanged();
}

void RemoteDesktopController::setAudio(bool on, const QString &codecPref)
{
    if (!on) { stopAudioIn(); return; }
    if (m_viewing.isEmpty()) return;
    m_audioWant = true;
    m_audioState = QStringLiteral("waiting");
    applyAudioSettings();
    QJsonArray codecs; codecs.append(QStringLiteral("pcm")); if (AudioStream::opusAvailable()) codecs.append(QStringLiteral("opus"));
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("audioRequest"); p[QStringLiteral("codecs")] = codecs; p[QStringLiteral("pref")] = codecPref;
    sendToPeer(m_viewing, p);
    emit audioStateChanged();
}

void RemoteDesktopController::declineShareRequest(const QString &deviceId)
{
    ScreenCaptureSession::log(QStringLiteral("Показ экрана для %1 отклонён").arg(who(deviceId)));
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("shareDeclined");
    sendToPeer(deviceId, p);
}

void RemoteDesktopController::stopSharing()
{
    ScreenCaptureSession::log(QStringLiteral("Останавливаю показ (получатель: %1, захват телефона: %2, захват ПК: %3)").arg(m_sharingTo.isEmpty() ? QStringLiteral("нет") : who(m_sharingTo), m_externalActive ? QStringLiteral("идёт") : QStringLiteral("нет"), m_capture.isActive() ? QStringLiteral("идёт") : QStringLiteral("нет")));
    if (m_sharingTo.isEmpty() && !m_externalActive && !m_capture.isActive()) return;      // показа нет ни кому, и захват не висит
    m_capture.stop();
    m_audioOut.stopCapture();
#ifdef Q_OS_ANDROID
    AndroidCapture::stop();
    m_externalActive = false;
    emit sharingChanged();
#endif
    if (m_sharingTo.isEmpty()) return;
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("shareStop"); p[QStringLiteral("by")] = QStringLiteral("sharer");
    sendToPeer(m_sharingTo, p);
    if (m_inputArmedFor == m_sharingTo) stopInputSession();
    m_sharingTo.clear();
    m_graceFor.clear();
    m_graceTimer.stop();
    releaseSecondScreen();
    emit sharingChanged();
    updateBackgroundHold();
}

void RemoteDesktopController::onFrameReady(const QByteArray &jpeg, int width, int height)
{
    // цельный кадр без областей (захват экрана телефона): ключевой кадр из одного прямоугольника
    sendFrame({{0, 0, width, height, jpeg}}, width, height, true);
}

void RemoteDesktopController::sendFrame(const QVector<FramePart> &parts, int width, int height, bool key)
{
    if (m_sharingTo.isEmpty() || !m_manager->isConnected(m_sharingTo)) return;
    const quint32 seq = ++m_sendSeq;
    m_lastSentW = width; m_lastSentH = height; m_lastSentMs = nowMs();
    m_lastSentReg = m_outRegion; m_lastSentSrc = m_outSource;
    m_sentAt.insert(seq, nowMs());
    if (seq > 64) m_sentAt.remove(seq - 64);      // подтверждения давно потерянных кадров не копим

    QJsonObject p;
    p[QStringLiteral("kind")] = QStringLiteral("frame");
    p[QStringLiteral("seq")] = double(seq);
    p[QStringLiteral("w")] = width;
    p[QStringLiteral("h")] = height;
    p[QStringLiteral("key")] = key;
    p[QStringLiteral("t")] = double(m_outCaptureMs > 0 ? m_outCaptureMs : nowMs());       // когда снят кадр: по этому времени зритель выдерживает ровный темп
    m_outCaptureMs = 0;
    if (!parts.isEmpty()) ++m_sentInSecond;
    QJsonArray arr;
    qint64 bytes = 0;
    for (const FramePart &fp : parts) {
        QJsonObject o;
        o[QStringLiteral("x")] = fp.x; o[QStringLiteral("y")] = fp.y; o[QStringLiteral("w")] = fp.w; o[QStringLiteral("h")] = fp.h;
        if (fp.fmt != 'c') o[QStringLiteral("jpeg")] = QString::fromLatin1(fp.jpeg.toBase64());
        if (fp.fmt == 'z') o[QStringLiteral("f")] = QStringLiteral("z");          // участок без потерь (zlib), а не JPEG
        if (fp.fmt == 's') o[QStringLiteral("f")] = QStringLiteral("s");          // однотонный участок: в данных один цвет (4 байта)
        if (fp.fmt == 'c') { o[QStringLiteral("f")] = QStringLiteral("c"); o[QStringLiteral("sx")] = fp.sx; o[QStringLiteral("sy")] = fp.sy; }      // прокрутка: данных нет
        arr.append(o);
        bytes += fp.jpeg.size();
    }
    p[QStringLiteral("parts")] = arr;
    p[QStringLiteral("lv")] = m_level + 1; p[QStringLiteral("ls")] = ladderSize(); p[QStringLiteral("cf")] = m_capture.captureFps();     // для статистики у зрителя
    if (m_capture.tilesMode()) p[QStringLiteral("mode")] = QStringLiteral("tiles");
    // что в кадре: область и размер всего экрана (у телефона-источника кадр всегда целый)
    const QRectF reg = m_outRegion.isValid() ? m_outRegion : QRectF(0, 0, 1, 1);
    p[QStringLiteral("rx")] = reg.x(); p[QStringLiteral("ry")] = reg.y(); p[QStringLiteral("rw")] = reg.width(); p[QStringLiteral("rh")] = reg.height();
    const QSize src = m_outSource.isValid() ? m_outSource : QSize(width, height);
    p[QStringLiteral("sw")] = src.width(); p[QStringLiteral("sh")] = src.height();
    if (!parts.isEmpty()) { m_avgMsgBytes = 0.9 * m_avgMsgBytes + 0.1 * (bytes * 1.37 + 200); m_txCodec = m_capture.tilesMode() ? QStringLiteral("Тайлы Orbit") : QStringLiteral("JPEG"); m_txBytes += bytes; }       // base64 раздувает на треть
    sendToPeer(m_sharingTo, p);
}

void RemoteDesktopController::onExternalVideo(const QByteArray &au, int w, int h, bool key)
{
    if (m_sharingTo.isEmpty() || !m_capture.wantsVideo()) return;
    // как у wf-recorder: пропущенный кадр ломает все следующие до ключевого, поэтому после пропуска ждём ключевой (и просим его у кодера)
    if (!key && m_extDropUntilKey) return;
    if (!canSendFrame()) { m_extDropUntilKey = true; AndroidCapture::requestKeyFrame(); return; }
    if (key) m_extDropUntilKey = false;
    m_extVideo = true;
    m_outRegion = QRectF(0, 0, 1, 1);                    // кодер снимает весь экран; приближение у зрителя — обычным масштабом
    m_outSource = QSize(w, h);
    m_outCaptureMs = nowMs();
    sendVideo(au, w, h, key);
}

void RemoteDesktopController::onExternalVideoFailed(const QString &reason)
{
    ScreenCaptureSession::log(QStringLiteral("Кодер видео на телефоне не заработал: ") + reason);
    m_extVideo = false;
    m_videoWhy = reason;
    m_viewerH264 = false;                                // для этого показа — без видео; формат выбирается заново (тайлы)
    chooseFormat();
    emit errorOccurred(QStringLiteral("Видео с телефона не заработало (") + reason + QStringLiteral("), показ идёт тайлами"));
}

void RemoteDesktopController::sendVideo(const QByteArray &au, int width, int height, bool key)
{
    if (m_sharingTo.isEmpty() || !m_manager->isConnected(m_sharingTo)) return;
    const quint32 seq = ++m_sendSeq;
    m_lastSentW = width; m_lastSentH = height; m_lastSentMs = nowMs();
    m_lastSentReg = m_outRegion; m_lastSentSrc = m_outSource;
    m_sentAt.insert(seq, nowMs());
    if (seq > 64) m_sentAt.remove(seq - 64);
    ++m_sentInSecond;

    QJsonObject p;
    p[QStringLiteral("kind")] = QStringLiteral("frame");
    p[QStringLiteral("codec")] = QStringLiteral("h264");
    p[QStringLiteral("seq")] = double(seq);
    p[QStringLiteral("w")] = width;
    p[QStringLiteral("h")] = height;
    p[QStringLiteral("key")] = key;
    p[QStringLiteral("t")] = double(m_outCaptureMs > 0 ? m_outCaptureMs : nowMs());
    m_outCaptureMs = 0;
    p[QStringLiteral("au")] = QString::fromLatin1(au.toBase64());
    p[QStringLiteral("lv")] = m_level + 1; p[QStringLiteral("ls")] = ladderSize(); p[QStringLiteral("cf")] = m_capture.captureFps();
    const QRectF reg = m_outRegion.isValid() ? m_outRegion : QRectF(0, 0, 1, 1);
    p[QStringLiteral("rx")] = reg.x(); p[QStringLiteral("ry")] = reg.y(); p[QStringLiteral("rw")] = reg.width(); p[QStringLiteral("rh")] = reg.height();
    const QSize src = m_outSource.isValid() ? m_outSource : QSize(width, height);
    p[QStringLiteral("sw")] = src.width(); p[QStringLiteral("sh")] = src.height();
    m_avgMsgBytes = 0.9 * m_avgMsgBytes + 0.1 * (au.size() * 1.37 + 200);
    m_txCodec = m_extVideo ? QStringLiteral("H.264 · кодер телефона") : (m_capture.gpuEncoding() ? QStringLiteral("H.264 · видеокарта") : QStringLiteral("H.264 · x264")); m_txBytes += au.size();
    sendToPeer(m_sharingTo, p);
}

void RemoteDesktopController::setViewRegion(double x, double y, double w, double h)
{
    if (m_viewing.isEmpty()) return;
    QRectF r(x, y, w, h);
    r = r.intersected(QRectF(0, 0, 1, 1));
    if (!r.isValid() || r.width() < 0.04 || r.height() < 0.04) return;
    if (r.width() > 0.97 && r.height() > 0.97) r = QRectF(0, 0, 1, 1);
    // мелкие сдвиги не стоят сообщения
    if (qAbs(r.x() - m_lastRegionSent.x()) + qAbs(r.y() - m_lastRegionSent.y()) + qAbs(r.width() - m_lastRegionSent.width()) + qAbs(r.height() - m_lastRegionSent.height()) < 0.012) return;
    m_lastRegionSent = r;
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("viewRegion");
    p[QStringLiteral("x")] = r.x(); p[QStringLiteral("y")] = r.y(); p[QStringLiteral("w")] = r.width(); p[QStringLiteral("h")] = r.height();
    sendToPeer(m_viewing, p);
}

void RemoteDesktopController::onExternalState(int state)
{
    ScreenCaptureSession::log(QStringLiteral("Захват экрана телефона: %1 (получатель: %2)").arg(state == 1 ? QStringLiteral("начался") : (state == 2 ? QStringLiteral("остановлен") : (state == 3 ? QStringLiteral("не разрешён")  : QStringLiteral("состояние %1").arg(state))), m_sharingTo.isEmpty() ? QStringLiteral("нет") : who(m_sharingTo)));
    if (state == 1) {
        // захват экрана запущен, а показывать уже некому (запрос отозван, зритель отключился, пока шло системное окно): без этого полоса «Остановить показ»
        // висела бы вечно при том, что никому ничего не показывается
        if (m_sharingTo.isEmpty()) {
#ifdef Q_OS_ANDROID
            AndroidCapture::stop();
#endif
            m_externalActive = false;
            emit sharingChanged();
            return;
        }
        m_externalActive = true; emit sharingChanged(); return;
    }
    m_externalActive = false;
    if (state == 3) {
        if (!m_sharingTo.isEmpty()) {
            QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("shareError"); p[QStringLiteral("message")] = QStringLiteral("показ экрана не разрешён на устройстве");
            sendToPeer(m_sharingTo, p);
        }
        emit errorOccurred(QStringLiteral("показ экрана не разрешён"));
    }
    if (!m_sharingTo.isEmpty()) stopSharing();
    emit sharingChanged();
}

void RemoteDesktopController::onCaptureError()
{
    emit errorOccurred(m_capture.errorString());
    if (!m_sharingTo.isEmpty()) {          // тот, кто смотрит, должен узнать причину, а не ждать кадров
        QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("shareError"); p[QStringLiteral("message")] = m_capture.errorString();
        sendToPeer(m_sharingTo, p);
    }
    if (!m_capture.isActive() && !m_sharingTo.isEmpty()) stopSharing();
}

// ---------- управление ----------

void RemoteDesktopController::armInput(const QString &deviceId, bool allow)
{
    if (allow) {
        if (!InputInjector::isAvailable()) { emit errorOccurred(QStringLiteral("удалённый ввод недоступен на этой системе")); return; }
        if (m_inputArmedFor != deviceId) stopInputSession();
        if (!m_input.isOpen() && !m_input.openDevice()) { emit errorOccurred(m_input.lastError()); return; }
        m_inputArmedFor = deviceId;
        m_inputArmedAt.start();
        QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("inputPermission"); p[QStringLiteral("allowed")] = true;
        sendToPeer(deviceId, p);
        emit inputArmedChanged();
    } else {
        stopInputSession();
    }
}

void RemoteDesktopController::stopInputSession()
{
    if (m_inputArmedFor.isEmpty()) return;
    const QString was = m_inputArmedFor;
    m_inputArmedFor.clear();
    m_input.closeDevice();
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("inputPermission"); p[QStringLiteral("allowed")] = false;
    sendToPeer(was, p);
    emit inputArmedChanged();
}

void RemoteDesktopController::requestControl()
{
    if (m_viewing.isEmpty()) return;
    m_controlState = 1;
    emit inputArmedChanged();
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("controlRequest");
    sendToPeer(m_viewing, p);
}

void RemoteDesktopController::extendInputSession()
{
    if (!m_inputArmedFor.isEmpty()) m_inputArmedAt.start();
}

// ---------- отправка ввода тому, чей экран я смотрю ----------

void RemoteDesktopController::sendMove(double dx, double dy)
{
    if (m_viewing.isEmpty()) return;
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("input"); p[QStringLiteral("op")] = QStringLiteral("move");
    p[QStringLiteral("dx")] = dx; p[QStringLiteral("dy")] = dy;
    sendToPeer(m_viewing, p);
}
void RemoteDesktopController::sendMoveAbsolute(double nx, double ny)
{
    if (m_viewing.isEmpty()) return;
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("input"); p[QStringLiteral("op")] = QStringLiteral("moveAbs");
    p[QStringLiteral("nx")] = nx; p[QStringLiteral("ny")] = ny;
    sendToPeer(m_viewing, p);
}
void RemoteDesktopController::sendButton(const QString &button, bool down)
{
    if (m_viewing.isEmpty()) return;
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("input"); p[QStringLiteral("op")] = QStringLiteral("button");
    p[QStringLiteral("button")] = button; p[QStringLiteral("down")] = down;
    sendToPeer(m_viewing, p);
}
void RemoteDesktopController::sendClick(const QString &button)
{
    if (m_viewing.isEmpty()) return;
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("input"); p[QStringLiteral("op")] = QStringLiteral("click");
    p[QStringLiteral("button")] = button;
    sendToPeer(m_viewing, p);
}
void RemoteDesktopController::sendScroll(int dy, int dx)
{
    if (m_viewing.isEmpty()) return;
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("input"); p[QStringLiteral("op")] = QStringLiteral("scroll");
    p[QStringLiteral("dy")] = dy; p[QStringLiteral("dx")] = dx;
    sendToPeer(m_viewing, p);
}
void RemoteDesktopController::sendKey(const QString &key, const QStringList &modifiers)
{
    if (m_viewing.isEmpty()) return;
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("input"); p[QStringLiteral("op")] = QStringLiteral("key");
    p[QStringLiteral("key")] = key;
    QJsonArray mods; for (const QString &m : modifiers) mods.append(m);
    p[QStringLiteral("mods")] = mods;
    sendToPeer(m_viewing, p);
}
void RemoteDesktopController::sendType(const QString &text)
{
    if (m_viewing.isEmpty()) return;
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("input"); p[QStringLiteral("op")] = QStringLiteral("type");
    p[QStringLiteral("text")] = text;
    sendToPeer(m_viewing, p);
}

// ---------- входящие сообщения ----------

void RemoteDesktopController::handleMessage(const QString &deviceId, const QJsonObject &payload)
{
    const QString kind = payload.value(QStringLiteral("kind")).toString();

    if (kind == QLatin1String("shareOffer")) {
        if (!m_viewing.isEmpty()) { declineShareRequest(deviceId); return; }
        // доверенное устройство показывает экран — смотрим сразу (страница просмотра откроется сама)
        if (m_manager->trustStore()->trustLevel(deviceId) >= 1) { requestShare(deviceId); return; }
        emit shareOffered(deviceId, m_manager->trustStore()->device(deviceId).name);     // человек решает, смотреть ли
        return;
    }
    if (kind == QLatin1String("shareError")) {
        if (m_viewing != deviceId) return;
        const QString msg = payload.value(QStringLiteral("message")).toString();
        ScreenCaptureSession::log(QStringLiteral("%1: показ не начался — %2").arg(who(deviceId), msg));
        endViewing();
        emit errorOccurred(QStringLiteral("Показ не начался: ") + msg);
        return;
    }
    // ход запуска показа у хозяина («создаю монитор», «ждём выбора экрана»): зритель видит, что происходит, а не пустое окно
    if (kind == QLatin1String("shareStatus")) {
        if (m_viewing == deviceId) { m_viewNote = payload.value(QStringLiteral("text")).toString().left(160); emit viewingChanged(); }
        return;
    }
    if (kind == QLatin1String("secondScreenRequest")) {
        ScreenCaptureSession::log(QStringLiteral("%1 просит стать вторым экраном (%2, уровень доверия %3)").arg(who(deviceId), m_ssOffered.contains(deviceId) ? QStringLiteral("мы сами это предлагали") : QStringLiteral("запрос от него")).arg(m_manager->trustStore()->trustLevel(deviceId)));
        SecondSpec s;
        s.w = qBound(640, payload.value(QStringLiteral("w")).toInt(1920), 3840); s.h = qBound(360, payload.value(QStringLiteral("h")).toInt(1080), 3840);
        s.fps = qBound(24, payload.value(QStringLiteral("fps")).toInt(60), 120);
        s.side = payload.value(QStringLiteral("side")).toString(QStringLiteral("right"));
        s.scale = qBound(1.0, payload.value(QStringLiteral("scale")).toDouble(1.0), 3.0);
        s.mode = payload.value(QStringLiteral("mode")).toString() == QLatin1String("mirror") ? QStringLiteral("mirror") : QStringLiteral("extend");
        s.workspace = payload.value(QStringLiteral("workspace")).toString();
        // «авто» у устройства — решает настройка этого компьютера, если в ней задан масштаб
        const double mine = m_settings->secondScale().toDouble();
        if (payload.value(QStringLiteral("scaleAuto")).toBool(false) && mine >= 1.0) s.scale = qBound(1.0, mine, 3.0);
        m_viewerWidth = payload.value(QStringLiteral("vw")).toInt();
        m_viewerH264 = payload.value(QStringLiteral("h264")).toBool(false);
        m_viewerTiles = payload.value(QStringLiteral("tiles")).toBool(false);
        if (!featureEnabled() || !captureAvailable()) { declineSecondScreen(deviceId, QStringLiteral("показ экрана на этом компьютере выключен")); return; }
        if (!m_sharingTo.isEmpty() && m_sharingTo != deviceId) { declineSecondScreen(deviceId, QStringLiteral("уже идёт показ другому устройству")); return; }
        if (VirtualDisplay::backend() == VirtualDisplay::Backend::None) { declineSecondScreen(deviceId, QStringLiteral("второй экран поддерживают Hyprland и driftwm (бета)")); return; }
        m_ssPending.insert(deviceId, s);
        if (m_ssOffered.contains(deviceId)) { acceptSecondScreen(deviceId, m_ssOffered.take(deviceId)); return; }      // компьютер сам предлагал — отдельный вопрос не нужен
        static const QHash<QString, QString> sides{{QStringLiteral("left"), QStringLiteral("слева")}, {QStringLiteral("right"), QStringLiteral("справа")},
                                                   {QStringLiteral("top"), QStringLiteral("сверху")}, {QStringLiteral("bottom"), QStringLiteral("снизу")}};
        const bool dwm = VirtualDisplay::backend() == VirtualDisplay::Backend::Driftwm;
        const QString desc = dwm ? QStringLiteral("%1×%2 · %3 к/с — вторым экраном станет окно приложения (бета)").arg(s.w).arg(s.h).arg(s.fps)
                                 : QStringLiteral("%1×%2 · %3 к/с, монитор будет %4 от основного").arg(s.w).arg(s.h).arg(s.fps).arg(sides.value(s.side, QStringLiteral("справа")));
        // полное доверие — без вопроса (Hyprland: окно выбирать не нужно)
        if (!dwm && m_manager->trustStore()->trustLevel(deviceId) >= 2) { acceptSecondScreen(deviceId); return; }
        emit secondScreenRequested(deviceId, m_manager->trustStore()->device(deviceId).name, desc, dwm ? VirtualDisplay::driftwmWindows() : QVariantList());
        return;
    }
    if (kind == QLatin1String("secondScreenOffer")) {
        ScreenCaptureSession::log(QStringLiteral("%1 предлагает использовать это устройство вторым экраном (уровень доверия %2)").arg(who(deviceId)).arg(m_manager->trustStore()->trustLevel(deviceId)));
        QVariantMap spec;
        for (const char *k : {"orientation", "res", "side", "scale", "mode", "workspace", "audio", "acodec"}) spec[QString::fromLatin1(k)] = payload.value(QString::fromLatin1(k)).toString();
        spec[QStringLiteral("fps")] = payload.value(QStringLiteral("fps")).toInt(60);
        if (!m_viewing.isEmpty() && m_viewing != deviceId) { declineSecondScreenOffer(deviceId); return; }      // уже смотрим другое устройство; это же — переключаемся на второй экран
        m_ssOfferSpec.insert(deviceId, spec);
        static const QHash<QString, QString> sides{{QStringLiteral("left"), QStringLiteral("слева")}, {QStringLiteral("right"), QStringLiteral("справа")},
                                                   {QStringLiteral("top"), QStringLiteral("сверху")}, {QStringLiteral("bottom"), QStringLiteral("снизу")}};
        const QString desc = QStringLiteral("%1 к/с, экран окажется %2 от основного монитора").arg(spec.value(QStringLiteral("fps")).toInt()).arg(sides.value(spec.value(QStringLiteral("side")).toString(), QStringLiteral("справа")));
        // экран телефона целиком занимается чужой картинкой — без вопроса только при полном доверии (как и у запроса второго экрана)
        if (m_manager->trustStore()->trustLevel(deviceId) >= 2) { acceptSecondScreenOffer(deviceId); return; }
        emit secondScreenOffered(deviceId, m_manager->trustStore()->device(deviceId).name, desc);
        return;
    }
    if (kind == QLatin1String("secondScreenDenied")) {
        if (m_ssOffered.remove(deviceId)) {                        // отказ от предложенного нами: зритель не согласился
            emit errorOccurred(QStringLiteral("Устройство не стало вторым экраном: ") + payload.value(QStringLiteral("reason")).toString());
            return;
        }
        if (m_viewing != deviceId) return;
        endViewing();
        emit errorOccurred(QStringLiteral("Второй экран не включён: ") + payload.value(QStringLiteral("reason")).toString());
        return;
    }
    if (kind == QLatin1String("shareRequest")) {
        ScreenCaptureSession::log(QStringLiteral("%1 хочет посмотреть экран (%2, уровень доверия %3, показ экрана %4)").arg(who(deviceId), m_offeredTo == deviceId ? QStringLiteral("мы сами предлагали") : QStringLiteral("по его запросу")).arg(m_manager->trustStore()->trustLevel(deviceId)).arg(featureEnabled() ? QStringLiteral("включён") : QStringLiteral("выключен")));
        m_viewerWidth = payload.value(QStringLiteral("vw")).toInt();
        m_viewerH264 = payload.value(QStringLiteral("h264")).toBool(false);
        m_viewerTiles = payload.value(QStringLiteral("tiles")).toBool(false);
        if (m_sharingTo == deviceId) chooseFormat();
        // тот же зритель вернулся, пока показ ждал его: поток продолжается без нового вопроса и без нового запуска захвата
        if (m_sharingTo == deviceId) {
            m_graceFor.clear(); m_graceTimer.stop();
            m_sendSeq = 0; m_ackedSeq = 0; m_lastAckMs = nowMs(); m_sentAt.clear();
            m_capture.requestKeyFrame();
            applyLevel();
            QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("shareAccepted");
            sendToPeer(deviceId, p);
            return;
        }
        if (!featureEnabled()) { declineShareRequest(deviceId); return; }
        if (m_offeredTo == deviceId) { acceptShareRequest(deviceId, m_offeredSource); return; }          // сам предложил, он согласился — без повторного вопроса
        // доверенное устройство: показ начинается без вопроса (экран — основной, как «по умолчанию»; выбрать другой можно в настройках показа)
        if (m_manager->trustStore()->trustLevel(deviceId) >= 1) { acceptShareRequest(deviceId, QString()); return; }
        emit shareRequested(deviceId, m_manager->trustStore()->device(deviceId).name);   // ждём локального Accept/Decline
        return;
    }
    if (kind == QLatin1String("audioRequest")) {
        auto reply = [&](bool on, const QString &reason, const QString &codec) {
            QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("audioState"); p[QStringLiteral("on")] = on; p[QStringLiteral("reason")] = reason; p[QStringLiteral("c")] = codec;
            sendToPeer(deviceId, p);
        };
        if (m_sharingTo != deviceId) { reply(false, QStringLiteral("показ экрана не идёт"), QString()); return; }
        if (!AudioStream::captureAvailable()) { reply(false, QStringLiteral("это устройство пока не передаёт звук (нужен компьютер с Linux и parec)"), QString()); return; }
        if (!m_manager->trustStore()->pluginAllowed(deviceId, QStringLiteral("audio"))) { reply(false, QStringLiteral("звук не разрешён: включите «Звук» для этого устройства в его настройках"), QString()); return; }
        const QStringList theirs = payload.value(QStringLiteral("codecs")).toVariant().toStringList();
        QString pref = payload.value(QStringLiteral("pref")).toString();
        if (pref.isEmpty() || pref == QLatin1String("auto")) pref = m_settings->audioCodec();
        QString codec = QStringLiteral("pcm");
        if ((pref == QLatin1String("opus") || pref == QLatin1String("auto")) && theirs.contains(QStringLiteral("opus")) && AudioStream::opusAvailable()) codec = QStringLiteral("opus");
        if (!m_audioOut.startCapture(m_settings->audioSource(), codec, m_settings->audioOpusKbps())) { reply(false, QStringLiteral("не удалось начать захват звука"), QString()); return; }
        ScreenCaptureSession::log(QStringLiteral("Звук для %1: передаю (%2)").arg(who(deviceId), m_audioOut.codec() == QLatin1String("opus") ? QStringLiteral("сжатие Opus") : QStringLiteral("без сжатия")));
        reply(true, QString(), m_audioOut.codec());
        return;
    }
    if (kind == QLatin1String("audioStop")) { if (m_sharingTo == deviceId) m_audioOut.stopCapture(); return; }
    if (kind == QLatin1String("audioState")) {
        if (m_viewing != deviceId) return;
        if (payload.value(QStringLiteral("on")).toBool()) { m_audioState = QStringLiteral("on"); m_audioCodecIn = payload.value(QStringLiteral("c")).toString(); emit audioStateChanged(); }
        else { stopAudioIn(QStringLiteral("off:") + payload.value(QStringLiteral("reason")).toString()); emit errorOccurred(QStringLiteral("Звук не включён: ") + payload.value(QStringLiteral("reason")).toString()); }
        return;
    }
    if (kind == QLatin1String("audioVol")) {
        if (m_viewing != deviceId) return;
        m_hostVolume = payload.value(QStringLiteral("v")).toDouble(1.0);
        m_hostMuted = payload.value(QStringLiteral("m")).toBool(false);
        applyAudioSettings();
        return;
    }
    if (kind == QLatin1String("audio")) {
        if (m_viewing == deviceId && m_audioWant) m_audioIn.play(QByteArray::fromBase64(payload.value(QStringLiteral("d")).toString().toLatin1()), payload.value(QStringLiteral("c")).toString());
        return;
    }
    if (kind == QLatin1String("permissionDenied")) {
        // другая сторона не разрешила этому устройству показ экрана: в её настройках «Что разрешено этому устройству» выключена «Демонстрация экрана»
        ScreenCaptureSession::log(QStringLiteral("%1 не разрешил запрос «%2» — нужно включить разрешение на его странице устройства").arg(who(deviceId), payload.value(QStringLiteral("of")).toString()));
        if (m_viewing == deviceId) endViewing();
        m_ssOffered.remove(deviceId);
        emit errorOccurred(QStringLiteral("Устройство не разрешило показ экрана. Откройте на нём это устройство и включите «Демонстрация экрана» в блоке «Что разрешено этому устройству у вас»"));
        return;
    }
    if (kind == QLatin1String("shareAccepted")) {
        ScreenCaptureSession::log(QStringLiteral("%1 согласился показать экран, ждём первые кадры").arg(who(deviceId)));
        if (m_viewing == deviceId && !m_audioWant) {
            const bool auto_ = m_secondViewing ? m_settings->audioAutoSecond() : m_settings->audioAutoShare();
            if (m_audioOverride == QLatin1String("on") || (m_audioOverride == QLatin1String("auto") && auto_)) QTimer::singleShot(300, this, [this, deviceId]() { if (m_viewing == deviceId) setAudio(true, m_audioCodecPref); });
        }
        if (m_viewing == deviceId) { m_viewStage = 1; m_reconnecting = false; m_reconnectTimeout.stop(); emit viewingChanged(); }
        return;
    }
    if (kind == QLatin1String("shareDeclined")) {
        if (m_viewing == deviceId) { endViewing(); emit shareDeclinedByPeer(deviceId); }
        return;
    }
    if (kind == QLatin1String("shareStop")) {
        // by: viewer — он перестал смотреть мой экран, sharer — он перестал показывать свой; без поля (старая версия) — оба направления
        const QString by = payload.value(QStringLiteral("by")).toString();
        if (m_viewing == deviceId && by != QLatin1String("viewer")) endViewing();
        if (m_sharingTo == deviceId && by != QLatin1String("sharer")) stopSharing();   // смотрящий отключился — демонстрация теряет смысл
        return;
    }
    if (kind == QLatin1String("frame")) {
        if (m_viewing != deviceId) return;   // кадры принимаются только от того, чей экран сейчас действительно смотрим
        // подтверждение уходит ПОСЛЕ разжатия (или когда кадр отброшен как устаревший): отправитель видит, с какой скоростью зритель
        // реально успевает, и сам снижает частоту — раньше слабый телефон заваливался кадрами, очередь сбрасывалась и картинка «моргала»
        const double frameSeq = payload.value(QStringLiteral("seq")).toDouble();
        const qint64 prevRx = m_lastFrameRxMs;
        m_lastFrameRxMs = nowMs();
        if (m_reconnecting) { m_reconnecting = false; m_reconnectTimeout.stop(); emit viewingChanged(); }
        ++m_viewFramesInSecond;
        if (payload.contains(QStringLiteral("lv"))) {
            m_senderLevel = payload.value(QStringLiteral("lv")).toInt(); m_senderSteps = payload.value(QStringLiteral("ls")).toInt(); m_senderCapFps = payload.value(QStringLiteral("cf")).toInt();
        }

        if (payload.value(QStringLiteral("codec")).toString() == QLatin1String("h264")) {
            // видеокадр: уходит аппаратному декодеру, готовый кадр вернётся через onVideoFrame
            sendAck(frameSeq, 0);
            VideoFrame meta;
            meta.region = QRectF(payload.value(QStringLiteral("rx")).toDouble(0), payload.value(QStringLiteral("ry")).toDouble(0),
                                 payload.value(QStringLiteral("rw")).toDouble(1), payload.value(QStringLiteral("rh")).toDouble(1));
            meta.source = QSize(payload.value(QStringLiteral("sw")).toInt(0), payload.value(QStringLiteral("sh")).toInt(0));
            {
                // ровный темп: тот же расчёт, что и у JPEG, но задержка короче (8–70 мс) — для живого экрана важнее отклик
                const qint64 tCap = qint64(payload.value(QStringLiteral("t")).toDouble(0));
                const qint64 arrival = nowMs();
                meta.due = arrival;
                if (tCap > 0) {
                    const double transit = double(arrival - tCap);
                    if (transit < m_minTransit || (prevRx > 0 && arrival - prevRx > 1500)) m_minTransit = transit;
                    else m_minTransit += 0.05;
                    m_jitter = 0.95 * m_jitter + 0.05 * qAbs(transit - m_minTransit);
                    const qint64 play = qBound(qint64(8), qint64(2.0 * m_jitter + 6), qint64(70));
                    m_videoPlayMs = int(play);
                    meta.due = tCap + qint64(m_minTransit) + play;
                    if (meta.due < arrival - 150) { m_minTransit = transit; meta.due = arrival + play; }      // сильно опоздали — пересинхронизация
                }
            }
            m_videoMeta.insert(qint64(frameSeq), meta);
            if (m_videoMeta.size() > 120) { const QList<qint64> ks = m_videoMeta.keys(); m_videoMeta.remove(*std::min_element(ks.begin(), ks.end())); }
            const QByteArray au = QByteArray::fromBase64(payload.value(QStringLiteral("au")).toString().toLatin1());
            const bool key = payload.value(QStringLiteral("key")).toBool();
            m_rxCodec = QStringLiteral("H.264"); m_rxBytes += au.size();
            const int vw = payload.value(QStringLiteral("w")).toInt(), vh = payload.value(QStringLiteral("h")).toInt();
            if (au.size() > 4 && vw >= 16 && vh >= 16 && vw <= 8192 && vh <= 8192) {
                if (!feedDecoder(au, vw, vh, key, qint64(frameSeq) * 1000)) {
                    // декодер не принял кадр (занят или ждёт ключевой): просим ключевой, пока он не придёт — остальное пропускается декодером
                    QJsonObject nk; nk[QStringLiteral("kind")] = QStringLiteral("needKey");
                    if (key == false && m_lastNeedKeyMs + 700 < nowMs()) { m_lastNeedKeyMs = nowMs(); sendToPeer(deviceId, nk); }
                }
            }
            return;
        }
        Update u;
        u.w = payload.value(QStringLiteral("w")).toInt();
        u.h = payload.value(QStringLiteral("h")).toInt();
        u.key = payload.value(QStringLiteral("key")).toBool(true);       // прежние версии слали целый кадр без признака
        u.region = QRectF(payload.value(QStringLiteral("rx")).toDouble(0), payload.value(QStringLiteral("ry")).toDouble(0),
                          payload.value(QStringLiteral("rw")).toDouble(1), payload.value(QStringLiteral("rh")).toDouble(1));
        u.source = QSize(payload.value(QStringLiteral("sw")).toInt(0), payload.value(QStringLiteral("sh")).toInt(0));
        u.t = qint64(payload.value(QStringLiteral("t")).toDouble(0));
        if (u.t > 0) {
            // ровный темп: кадр показывается через (его время снимка + наименьшая «дорога» + небольшая задержка). Задержка растёт с разбросом
            // прихода (jitter) — так сетевые рывки не превращаются в рывки картинки
            const qint64 arrival = nowMs();
            const double transit = double(arrival - u.t);
            if (transit < m_minTransit || (prevRx > 0 && arrival - prevRx > 1500)) m_minTransit = transit;       // новая точка отсчёта (после паузы часы «уплывают»)
            else m_minTransit += 0.05;                                                                  // медленно подтягиваемся к дрейфу часов
            m_jitter = 0.95 * m_jitter + 0.05 * qAbs(transit - m_minTransit);
            m_playoutMs = qBound<qint64>(qint64(35), qint64(2.0 * m_jitter + 25), qint64(160));
            u.due = u.t + qint64(m_minTransit) + m_playoutMs;
            if (u.due < arrival - 250) { m_minTransit = transit; u.due = arrival + m_playoutMs; }         // сильно опоздали — пересинхронизация
        }
        if (u.w < 16 || u.h < 16 || u.w > 8192 || u.h > 8192) { sendAck(frameSeq, 0); return; }
        for (const QJsonValue &v : payload.value(QStringLiteral("parts")).toArray()) {
            const QJsonObject o = v.toObject();
            FramePart fp;
            fp.x = o.value(QStringLiteral("x")).toInt(); fp.y = o.value(QStringLiteral("y")).toInt();
            fp.w = o.value(QStringLiteral("w")).toInt(); fp.h = o.value(QStringLiteral("h")).toInt();
            fp.jpeg = QByteArray::fromBase64(o.value(QStringLiteral("jpeg")).toString().toLatin1());
            if (o.value(QStringLiteral("f")).toString() == QLatin1String("c")) {
                fp.fmt = 'c';
                fp.sx = o.value(QStringLiteral("sx")).toInt(); fp.sy = o.value(QStringLiteral("sy")).toInt();
                if (fp.w < 1 || fp.h < 1 || fp.w > 8192 || fp.h > 8192 || fp.x < 0 || fp.y < 0 || fp.sx < 0 || fp.sy < 0) continue;
            } else if (o.value(QStringLiteral("f")).toString() == QLatin1String("s")) {
                fp.fmt = 's';
                if (fp.jpeg.size() != 4 || fp.w < 1 || fp.h < 1 || fp.w > 4096 || fp.h > 4096) continue;
            } else if (o.value(QStringLiteral("f")).toString() == QLatin1String("z")) {
                fp.fmt = 'z';
                if (fp.jpeg.size() < 8 || fp.jpeg.size() > 8 * 1024 * 1024 || fp.w < 1 || fp.h < 1 || fp.w > 4096 || fp.h > 4096) continue;
            } else if (fp.jpeg.size() < 100 || quint8(fp.jpeg[0]) != 0xFF || quint8(fp.jpeg[1]) != 0xD8) continue;    // не JPEG — отбрасываем
            u.parts.append(fp);
            m_rxBytes += fp.jpeg.size();
        }
        if (u.parts.isEmpty()) { sendAck(frameSeq, 0); emit statsChanged(); return; }          // пустое обновление — только «связь жива», формат не меняет
        m_rxCodec = payload.value(QStringLiteral("mode")).toString() == QLatin1String("tiles") ? QStringLiteral("Тайлы Orbit") : QStringLiteral("JPEG");
        if (m_videoOnSink) {
            // отправитель переключился с видео на тайлы/JPEG прямо во время показа: картинку снова рисует FrameView, видео-слой гасится
            m_videoOnSink = false;
            if (m_videoSink) m_videoSink->setVideoFrame(QVideoFrame());
            if (m_decoder) m_decoder->stop();
            PlatformBridge::stopVideo();
            emit frameUpdated();
        }
        u.seq = frameSeq;
        enqueueUpdate(u);
        return;
    }
    if (kind == QLatin1String("videoFail")) {
        // у зрителя не заработал декодер видео: возвращаемся к JPEG для этого показа
        if (m_sharingTo != deviceId) return;
        ScreenCaptureSession::log(QStringLiteral("Зритель сообщил, что не смог показать видео: ") + payload.value(QStringLiteral("reason")).toString());
        ++m_videoFails;
        m_viewerH264 = false;
        chooseFormat();
        m_videoWhy = QStringLiteral("зритель: ") + payload.value(QStringLiteral("reason")).toString().left(120);
        return;
    }
    if (kind == QLatin1String("needKey")) {
        if (m_sharingTo == deviceId) { m_capture.requestKeyFrame(); AndroidCapture::requestKeyFrame(); }         // зритель потерял картинку — следующее обновление целиком
        return;
    }
    if (kind == QLatin1String("viewRegion")) {
        // зритель увеличил картинку: с этого момента снимается только этот участок (телефон-источник слать область не умеет — кадр остаётся целым)
        if (m_sharingTo == deviceId) {
            m_capture.setRegion(QRectF(payload.value(QStringLiteral("x")).toDouble(), payload.value(QStringLiteral("y")).toDouble(),
                                       payload.value(QStringLiteral("w")).toDouble(1), payload.value(QStringLiteral("h")).toDouble(1)));
        }
        return;
    }
    if (kind == QLatin1String("viewerSize")) {
        // окно зрителя изменилось (поворот телефона, «картинка в картинке»): размер кадра подгоняется под него
        if (m_sharingTo == deviceId) { m_viewerWidth = payload.value(QStringLiteral("vw")).toInt(); applyLevel(); }
        return;
    }
    if (kind == QLatin1String("frameAck")) {
        if (m_sharingTo != deviceId) return;
        const quint32 seq = quint32(payload.value(QStringLiteral("seq")).toDouble());
        const auto it = m_sentAt.constFind(seq);
        if (it != m_sentAt.constEnd()) {
            const double sample = double(nowMs() - it.value());
            m_rttMs = m_rttMs <= 0 ? sample : 0.7 * m_rttMs + 0.3 * sample;
            m_sentAt.remove(seq);
        }
        const int dec = payload.value(QStringLiteral("dec")).toInt(0);
        if (dec > 0) m_peerDecodeMs = m_peerDecodeMs <= 0 ? dec : 0.8 * m_peerDecodeMs + 0.2 * dec;
        m_ackedSeq = qMax(m_ackedSeq, seq);
        m_lastAckMs = nowMs();
        return;
    }
    if (kind == QLatin1String("inputPermission")) {
        // это сообщение присылает тот, чей экран я смотрю — сообщает, разрешил ли он мне управлять
        if (m_viewing != deviceId) return;
        const bool allowed = payload.value(QStringLiteral("allowed")).toBool();
        m_controlState = allowed ? 2 : 3;
        if (!allowed) emit errorOccurred(QStringLiteral("управление недоступно: на показывающем устройстве нет доступа к /dev/uinput или вы не получили разрешение"));
        emit inputArmedChanged();   // сам факт смены состояния на удалённой стороне отражается через UI DeviceDetailPage
        return;
    }
    if (kind == QLatin1String("controlRequest")) {
        if (m_sharingTo != deviceId) return;
        if (!InputInjector::isAvailable()) {      // телефон и системы без доступа к вводу: сразу отказ, без вопроса человеку
            QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("inputPermission"); p[QStringLiteral("allowed")] = false;
            sendToPeer(deviceId, p);
            return;
        }
        // полное доверие (уровень 2): управление включается без вопроса; красная полоса сверху всё равно показывает, что оно включено
        if (m_manager->trustStore()->trustLevel(deviceId) >= 2) { armInput(deviceId, true); return; }
        emit controlRequested(deviceId, m_manager->trustStore()->device(deviceId).name);
        return;
    }
    if (kind == QLatin1String("input")) {
        // входящий ввод принимается, только если ИМЕННО этому устройству сейчас разрешено мной управлять
        if (m_inputArmedFor != deviceId || !m_input.isOpen()) return;
        extendInputSession();
        const QString op = payload.value(QStringLiteral("op")).toString();
        if (op == QLatin1String("move")) m_input.moveRelative(payload.value(QStringLiteral("dx")).toInt(), payload.value(QStringLiteral("dy")).toInt());
        else if (op == QLatin1String("moveAbs")) m_input.moveAbsolute(payload.value(QStringLiteral("nx")).toDouble(), payload.value(QStringLiteral("ny")).toDouble());
        else if (op == QLatin1String("button")) m_input.mouseButton(payload.value(QStringLiteral("button")).toString(), payload.value(QStringLiteral("down")).toBool());
        else if (op == QLatin1String("click")) m_input.click(payload.value(QStringLiteral("button")).toString());
        else if (op == QLatin1String("scroll")) m_input.scroll(payload.value(QStringLiteral("dy")).toInt(), payload.value(QStringLiteral("dx")).toInt());
        else if (op == QLatin1String("key")) {
            QStringList mods;
            for (const QJsonValue &v : payload.value(QStringLiteral("mods")).toArray()) mods << v.toString();
            m_input.keyTap(payload.value(QStringLiteral("key")).toString(), mods);
        } else if (op == QLatin1String("type")) {
            m_input.typeText(payload.value(QStringLiteral("text")).toString());
        } else if (op == QLatin1String("raw")) {
            // клавиатура зрителя целиком (второй экран): коды evdev как есть — раскладка и сочетания работают, как у своей клавиатуры
            for (const QJsonValue &v : payload.value(QStringLiteral("e")).toArray()) {
                const QJsonArray a = v.toArray();
                if (a.size() == 3) m_input.rawEvent(quint16(a.at(0).toInt()), quint16(a.at(1).toInt()), a.at(2).toInt());
            }
            m_input.rawSync();
        }
        return;
    }
}

// ---------- сборка картинки у зрителя ----------

void RemoteDesktopController::enqueueUpdate(const Update &u)
{
    QVector<double> dropped;
    {
    QMutexLocker lock(&m_decodeMutex);
    if (u.seq >= 0) {
        if (u.seq + 100 < m_lastEnqSeq) { m_lastKeySeq = -1; m_ready.clear(); }       // нумерация начата заново (повторное подключение)
        m_lastEnqSeq = u.seq;
    }
    if (u.key) {
        // ключевой кадр целиком заменяет всё, что было до него: ждущие разжатия старые обновления не нужны — зритель не отстаёт и не копит задержку
        for (const Update &o : std::as_const(m_decodeQueue)) dropped.append(o.seq);
        m_decodeQueue.clear();
    } else if (m_decodeQueue.size() + m_ready.size() >= 24) {
        // не успеваем (слабый телефон): очередь сбрасывается, у показывающего просим целый кадр — иначе картинка «поплывёт»
        for (const Update &o : std::as_const(m_decodeQueue)) dropped.append(o.seq);
        m_decodeQueue.clear(); m_ready.clear();
        dropped.append(u.seq);
        QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("needKey"); if (!m_viewing.isEmpty()) sendToPeer(m_viewing, p);
        lock.unlock();
        for (double s : std::as_const(dropped)) sendAck(s, 0);
        return;
    }
    m_decodeQueue.append(u);
    // целые кадры разжимаются параллельно (у телефона несколько ядер), каждый в своём потоке: один поток на 1920×1080 давал около 13 к/с
    if (m_decoders < kMaxDecoders) { ++m_decoders; QThreadPool::globalInstance()->start([this]() { decodeLoop(); }); }
    }
    for (double s : std::as_const(dropped)) sendAck(s, 0);
}

QString RemoteDesktopController::formatLine(const QString &codec, int w, int h, int fps, int kbit)
{
    QStringList parts{codec};
    if (w > 0 && h > 0) parts << QStringLiteral("%1×%2").arg(w).arg(h);
    parts << QStringLiteral("%1 к/с").arg(fps);
    parts << (kbit >= 1000 ? QStringLiteral("%1 Мбит/с").arg(kbit / 1000.0, 0, 'f', 1) : QStringLiteral("%1 кбит/с").arg(kbit));
    return parts.join(QStringLiteral(" · "));
}

bool RemoteDesktopController::feedDecoder(const QByteArray &au, int w, int h, bool key, qint64 ptsUs)
{
#ifdef Q_OS_ANDROID
    return PlatformBridge::feedVideo(au, w, h, key, ptsUs);
#else
    Q_UNUSED(w) Q_UNUSED(h)
    return m_decoder && m_decoder->feed(au, key, ptsUs);
#endif
}

void RemoteDesktopController::onVideoFrame(const QVideoFrame &frame, qint64 ptsUs)
{
    if (m_viewing.isEmpty() || !frame.isValid()) return;
    VideoFrame f = m_videoMeta.take(ptsUs / 1000);
    f.frame = frame;
    if (f.due == 0) f.due = nowMs();
    // очередь короткая: если картинки копятся быстрее показа, самые старые выбрасываются — показ догоняет, а не растягивает задержку
    m_videoQueue.append(f);
    while (m_videoQueue.size() > 3) m_videoQueue.removeFirst();
    presentVideo();
}

void RemoteDesktopController::presentVideo()
{
    const qint64 now = nowMs();
    bool shown = false;
    while (!m_videoQueue.isEmpty() && m_videoQueue.first().due <= now) {
        // если подошло время сразу нескольких кадров, показывается последний: промежуточные пропускаются целиком
        const VideoFrame f = m_videoQueue.takeFirst();
        if (!m_videoQueue.isEmpty() && m_videoQueue.first().due <= now) continue;
        showVideoFrame(f);
        shown = true;
    }
    Q_UNUSED(shown)
    if (!m_videoQueue.isEmpty()) m_videoTimer.start(int(qMax<qint64>(1, m_videoQueue.first().due - now)));
}

void RemoteDesktopController::showVideoFrame(const VideoFrame &f)
{
    const QSize sz = f.frame.size();
    if (m_videoSink) {
        m_videoSink->setVideoFrame(f.frame);            // YUV → RGB делает видеочип при отрисовке
        m_videoOnSink = true;
        QMutexLocker lock(&m_frameMutex);
        m_canvasRegion = f.region;
    } else {
        // приёмника нет (страница ещё не создана): перевод на процессоре, картинка идёт через FrameView
        const QImage img = f.frame.toImage();
        m_videoOnSink = false;
        QMutexLocker lock(&m_frameMutex);
        m_canvas = img;
        m_canvasRegion = f.region;
        ++m_canvasSerial;
    }
    m_inRegion = f.region; m_inSource = f.source;
    ++m_shownInSecond;
    const bool sizeChanged = (sz.width() != m_frameW || sz.height() != m_frameH);
    m_frameW = sz.width(); m_frameH = sz.height();
    if (sizeChanged) updateBackgroundHold();
    ++m_frameSeq;
    emit frameUpdated();
}

void RemoteDesktopController::onVideoFailed(const QString &reason)
{
    if (m_viewing.isEmpty()) return;
    emit errorOccurred(QStringLiteral("Видео не заработало: ") + reason + QStringLiteral(". Показ идёт через JPEG."));
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("videoFail"); p[QStringLiteral("reason")] = reason;
    sendToPeer(m_viewing, p);
}

void RemoteDesktopController::sendAck(double seq, int decodeMs)
{
    if (seq < 0 || m_viewing.isEmpty()) return;
    QJsonObject ack;
    ack[QStringLiteral("kind")] = QStringLiteral("frameAck");
    ack[QStringLiteral("seq")] = seq;
    if (decodeMs > 0) ack[QStringLiteral("dec")] = decodeMs;
    sendToPeer(m_viewing, ack);
}

// Плитка без потерь: обратное к ScreenCaptureSession::losslessOf — zlib, затем прибавление левого соседа в каждой строке
static QImage decodeLossless(const FramePart &fp)
{
    const int row = fp.w * 4;
    if (fp.jpeg.size() < 4 || qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(fp.jpeg.constData())) != quint32(row) * quint32(fp.h)) return QImage();
    const QByteArray raw = qUncompress(fp.jpeg);
    if (raw.size() != row * fp.h) return QImage();
    QImage img(fp.w, fp.h, QImage::Format_RGB32);
    for (int y = 0; y < fp.h; ++y) {
        const uchar *s = reinterpret_cast<const uchar *>(raw.constData()) + y * row;
        uchar *d = img.scanLine(y);
        for (int i = 0; i < 4 && i < row; ++i) d[i] = s[i];
        for (int i = 4; i < row; ++i) d[i] = uchar(s[i] + d[i - 4]);
    }
    return img;
}

// Рабочий поток: только разжимает JPEG-участки. Картинка при этом не трогается — обновление ждёт своего часа в m_ready.
// Потоков до kMaxDecoders: целые кадры независимы, а обновления-«дельты» зависят от порядка и разжимаются только когда рабочий поток один
void RemoteDesktopController::decodeLoop()
{
    for (;;) {
        Update u;
        int parallel = 1;
        {
            QMutexLocker lock(&m_decodeMutex);
            if (m_decodeQueue.isEmpty() || (!m_decodeQueue.first().key && m_decoders > 1)) { --m_decoders; return; }
            u = m_decodeQueue.takeFirst();
            parallel = m_decoders;
        }
        QElapsedTimer decT; decT.start();
        // участки разжимаются параллельно (целый кадр режется на полосы, у обновления бывает несколько участков): до трёх потоков;
        // порядок наложения сохраняется — результат кладётся в ячейку по номеру участка
        QVector<QImage> decoded(u.parts.size());
        auto decodePart = [&](int i) {
            const FramePart &fp = u.parts[i];
            if (fp.fmt == 's') {         // однотонный участок: заливка цветом
                quint32 col = 0;
                std::memcpy(&col, fp.jpeg.constData(), 4);
                QImage fill(fp.w, fp.h, QImage::Format_RGB32);
                fill.fill(QColor::fromRgb(col));
                decoded[i] = fill;
            } else if (fp.fmt == 'z') decoded[i] = decodeLossless(fp);
            else if (fp.fmt != 'c') decoded[i] = QImage::fromData(fp.jpeg, "JPEG");
        };
        const int np = int(u.parts.size());
        if (np >= 3) {
            const int nt = qMin(3, np);
            std::vector<std::thread> workers;
            for (int t = 1; t < nt; ++t) workers.emplace_back([&, t]() { for (int i = t; i < np; i += nt) decodePart(i); });
            for (int i = 0; i < np; i += nt) decodePart(i);
            for (std::thread &th : workers) th.join();
        } else {
            for (int i = 0; i < np; ++i) decodePart(i);
        }
        for (int i = 0; i < np; ++i) {
            const FramePart &fp = u.parts[i];
            if (fp.fmt == 'c') u.copies.append({QRect(fp.x, fp.y, fp.w, fp.h), QPoint(fp.sx, fp.sy)});
            else if (!decoded[i].isNull()) u.patches.append({QPoint(fp.x, fp.y), decoded[i]});
        }
        u.parts.clear();
        // «цена кадра» для отправителя — с учётом параллельности: именно столько зритель тратит на кадр при потоке кадров
        int decMs = qMax(1, int(decT.elapsed()) / qMax(1, parallel));
        {
            QMutexLocker lock(&m_decodeMutex);
            if (u.seq >= 0 && u.seq < m_lastKeySeq) {
                decMs = 0;                                  // опоздал: уже показан более новый целый кадр
            } else {
                if (u.key) {
                    if (u.seq >= 0) m_lastKeySeq = u.seq;
                    m_ready.removeIf([&u](const Update &o) { return o.seq < u.seq; });      // готовые, но не показанные обновления устарели
                }
                int pos = m_ready.size();
                while (pos > 0 && m_ready.at(pos - 1).seq > u.seq) --pos;                   // порядок показа — по номеру кадра
                m_ready.insert(pos, u);
            }
        }
        const double seq = u.seq;
        QMetaObject::invokeMethod(this, [this, seq, decMs]() {
            sendAck(seq, decMs);
            if (!m_presentTimer.isActive()) m_presentTimer.start(0);
        }, Qt::QueuedConnection);
    }
}

// Поток интерфейса: накладывает на картинку те обновления, чей час пришёл, и сообщает отрисовке один раз за тик
void RemoteDesktopController::presentTick()
{
    const qint64 now = nowMs();
    bool changed = false;
    qint64 wait = -1;
    for (;;) {
        Update u;
        {
            QMutexLocker lock(&m_decodeMutex);
            if (m_ready.isEmpty()) break;
            const qint64 due = m_ready.first().due;
            if (due > now) { wait = due - now; break; }
            u = m_ready.takeFirst();
        }
        applyUpdate(u);
        changed = true;
    }
    if (changed) {
        ++m_shownInSecond;
        ++m_frameSeq;
        emit frameUpdated();
    }
    if (wait >= 0) m_presentTimer.start(int(qMax<qint64>(1, wait)));
}

void RemoteDesktopController::applyUpdate(const Update &u)
{
    bool needKey = false;
    {
        QMutexLocker lock(&m_frameMutex);
        if (m_canvas.size() != QSize(u.w, u.h)) {
            if (!u.key) needKey = true;                         // обновление без ключевого кадра нам ничем не поможет
            else m_canvas = QImage(u.w, u.h, QImage::Format_RGB32);
        }
        if (!needKey) {
            QPainter painter(&m_canvas);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            // сначала прокрутка (сдвиг уже имеющейся картинки), потом свежие участки поверх
            for (const auto &cp : u.copies) {
                const QRect src(cp.second, cp.first.size());
                if (!m_canvas.rect().contains(src) || !m_canvas.rect().contains(cp.first)) continue;
                painter.drawImage(cp.first.topLeft(), m_canvas.copy(src));
            }
            for (const auto &p : u.patches) painter.drawImage(p.first, p.second);
            painter.end();
            m_canvasRegion = u.region;
            ++m_canvasSerial;
        }
    }
    if (needKey) {
        QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("needKey");
        if (!m_viewing.isEmpty()) sendToPeer(m_viewing, p);
        return;
    }
    m_inRegion = u.region; m_inSource = u.source;
    const bool sizeChanged = (u.w != m_frameW || u.h != m_frameH);
    m_frameW = u.w; m_frameH = u.h;
    if (sizeChanged) updateBackgroundHold();                    // размер окна «картинка в картинке» подстраивается под экран показывающего
}

bool RemoteDesktopController::canvasSnapshot(QImage *image, QRectF *region, quint64 *serial)
{
    QMutexLocker lock(&m_frameMutex);
    if (m_canvas.isNull()) return false;
    *image = m_canvas;            // копия QImage — счётчик ссылок; полная копия делается только когда накладывается следующее обновление
    *region = m_canvasRegion;
    *serial = m_canvasSerial;
    return true;
}

void RemoteDesktopController::clearCanvas()
{
    PlatformBridge::stopVideo();
    if (m_decoder) m_decoder->stop();
    m_videoQueue.clear(); m_videoMeta.clear(); m_videoTimer.stop();
    m_videoOnSink = false;
    if (m_videoSink) m_videoSink->setVideoFrame(QVideoFrame());
    { QMutexLocker lock(&m_decodeMutex); m_decodeQueue.clear(); m_ready.clear(); }
    QMutexLocker lock(&m_frameMutex);
    m_canvas = QImage();
    ++m_canvasSerial;
    { QMutexLocker dl(&m_decodeMutex); m_lastKeySeq = -1; m_lastEnqSeq = -1; }
}

QImage RemoteDesktopController::ImageProvider::requestImage(const QString &, QSize *size, const QSize &)
{
    // вызывается из потока загрузки картинок (Image с asynchronous: true); копия QImage — только счётчик ссылок, пока никто не рисует
    QImage img;
    { QMutexLocker lock(&m_owner->m_frameMutex); img = m_owner->m_canvas; }
    if (size) *size = img.size();
    return img;
}
