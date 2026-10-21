#include "ScreenCaptureSession.h"
#include "VirtualDisplay.h"
#include <QVideoFrame>
#include <QImage>
#include <QBuffer>
#include <QThreadPool>
#include <QDateTime>
#include <cstring>
#include <thread>
#include <vector>
#include <QHash>
#include <QMetaObject>
#include <QDebug>
#include <QGuiApplication>
#include <QScreen>
#include <QStandardPaths>
#include <QCoreApplication>
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <csignal>
#endif
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

ScreenCaptureSession::ScreenCaptureSession(QObject *parent) : QObject(parent)
{
    m_pool.setMaxThreadCount(1);       // сжатие идёт строго по очереди
    m_session.setScreenCapture(&m_capture);
    m_session.setVideoSink(&m_sink);

    connect(&m_capture, &QScreenCapture::activeChanged, this, &ScreenCaptureSession::activeChanged);
    connect(&m_capture, &QScreenCapture::errorOccurred, this, [this](QScreenCapture::Error, const QString &msg) {
        m_errorString = msg;
        emit errorChanged();
        qWarning() << "ScreenCaptureSession:" << msg;
    });
    m_vidFlush.setSingleShot(true);
    m_vidFlush.setInterval(4);
    // кодер отдаёт кадр одной записью; если за 4 мс после чтения новых данных нет — кадр закончился: уходит сразу, не ожидая начала следующего
    // (иначе каждый кадр задерживался бы ещё на один интервал кадров)
    connect(&m_vidFlush, &QTimer::timeout, this, [this]() {
        if (!m_video || m_wfBuf.size() < 8) return;
        if (m_wf && m_wf->bytesAvailable() > 0) { onVideoData(); return; }      // продолжение кадра уже пришло — сначала оно (кадр режется по концу данных, не по таймеру)
        const QByteArray au = m_wfBuf;
        m_wfBuf.clear();
        emitVideoUnit(au);
    });
    m_refineTimer.setSingleShot(true);
    m_refineTimer.setInterval(450);
    connect(&m_refineTimer, &QTimer::timeout, this, &ScreenCaptureSession::refineTick);
    m_winTimer.setInterval(900);
    connect(&m_winTimer, &QTimer::timeout, this, &ScreenCaptureSession::checkWindow);
    m_wfRestart.setSingleShot(true);
    m_wfRestart.setInterval(1200);       // перезапуск кодера — это пауза и новый ключевой кадр: изменения копятся и применяются разом
    connect(&m_wfRestart, &QTimer::timeout, this, [this]() { if (m_wfActive) { stopWf(true); startWf(); } });
    connect(&m_sink, &QVideoSink::videoFrameChanged, this, &ScreenCaptureSession::onVideoFrameChanged);
}

bool ScreenCaptureSession::isActive() const { return m_wfActive || m_capture.isActive(); }

void ScreenCaptureSession::setTargetFps(int fps) { fps = qBound(1, fps, 60); if (fps != m_targetFps) { m_targetFps = fps; emit targetFpsChanged(); if (m_wfActive && (m_video ? qAbs(fps - m_wfFps) >= 12 : fps > m_wfFps)) scheduleWfRestart(); } }
void ScreenCaptureSession::setJpegQuality(int q) { q = qBound(10, q, 95); if (q != m_jpegQuality) { m_jpegQuality = q; emit jpegQualityChanged(); if (m_wfActive && !m_raw && qAbs(q - m_wfQuality) >= (m_video ? 10 : 5)) scheduleWfRestart(); } }
void ScreenCaptureSession::setMaxWidth(int w) { w = qBound(320, w, 3840); if (w != m_maxWidth) { m_maxWidth = w; emit maxWidthChanged(); if (m_wfActive && qAbs(w - m_wfWidth) * 100 > m_wfWidth * 15) scheduleWfRestart(); } }

void ScreenCaptureSession::setRegion(const QRectF &r)
{
    const QRectF n = r.isValid() ? r.intersected(QRectF(0, 0, 1, 1)) : QRectF(0, 0, 1, 1);
    const bool changed = !qFuzzyCompare(n.x() + 1, m_region.x() + 1) || !qFuzzyCompare(n.y() + 1, m_region.y() + 1)
        || !qFuzzyCompare(n.width(), m_region.width()) || !qFuzzyCompare(n.height(), m_region.height());
    m_region = n;
    m_forceKey = true;
    if (changed && m_wfActive) scheduleWfRestart(350);        // увеличение у зрителя — отклик важнее
}

void ScreenCaptureSession::log(const QString &line)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    QFile f(dir + QStringLiteral("/orbit-video.log"));
    if (f.size() > 256 * 1024) f.remove();                      // журнал не растёт бесконечно
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(QStringLiteral("[HH:mm:ss] ")) + line + QLatin1Char('\n')).toUtf8());
}

QString ScreenCaptureSession::videoUnavailableReason() const
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    if (qEnvironmentVariable("ORBIT_VIDEO") == QLatin1String("0") || qEnvironmentVariable("ORBIT_CAPTURE") == QLatin1String("qt")) return QStringLiteral("видео выключено переменной окружения");
    if (QGuiApplication::platformName() != QLatin1String("wayland")) return QStringLiteral("Orbit запущен не под Wayland");
    if (QStandardPaths::findExecutable(QStringLiteral("wf-recorder")).isEmpty()) return QStringLiteral("не найден wf-recorder");
    if (m_screenName == QLatin1String("portal")) return QStringLiteral("выбран системный диалог — видео только для экранов и окон");
    return QString();
#else
    return QStringLiteral("видео поддерживает только Linux с Wayland");
#endif
}

void ScreenCaptureSession::setEncoderPreference(const QString &pref)
{
    if (pref == m_encPref) return;
    m_encPref = pref;
    m_gpuFailed = false;
    if (m_wfActive && m_video) scheduleWfRestart();
}

void ScreenCaptureSession::setVideoMode(bool on)
{
    if (m_wantVideo == on) return;
    m_wantVideo = on;
    if (m_wfActive && m_video != on) scheduleWfRestart(400);      // смена формата — сразу
}

void ScreenCaptureSession::requestKeyFrame()
{
    m_forceKey = true;
    // у видео ключевой кадр идёт раз в секунду сам; если запрос пришёл сразу после него — ждём, иначе перезапуск кодера
    if (m_video && m_wfActive && QDateTime::currentMSecsSinceEpoch() - m_lastKeyUnitMs > 1500) scheduleWfRestart(300);
}

void ScreenCaptureSession::start()
{
    if (isActive()) return;
    m_errorString.clear();
    m_frameTimer.invalidate();
    m_forceKey = true;
    if (wfAvailable()) { startWf(); return; }
    startQtCapture();
}

void ScreenCaptureSession::startQtCapture()
{
    log(QStringLiteral("Снимаю экран средствами Qt (JPEG): wf-recorder недоступен или не сработал (видео H.264 было %1)").arg(m_wantVideo ? QStringLiteral("запрошено") : QStringLiteral("не запрошено")));
    // без заданного экрана берётся основной; на Wayland запрос и покажет системный выбор, какой именно делиться
    m_capture.start();
}

void ScreenCaptureSession::stop()
{
    if (m_wfActive) stopWf();
    m_capture.stop();
}

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
bool ScreenCaptureSession::wfAvailable() const
{
    if (qEnvironmentVariable("ORBIT_CAPTURE") == QLatin1String("qt") || m_screenName == QLatin1String("portal")) return false;
    if (QGuiApplication::platformName() != QLatin1String("wayland")) return false;
    return !QStandardPaths::findExecutable(QStringLiteral("wf-recorder")).isEmpty();
}

namespace {
// ответ hyprctl -j <what> одним разбором
QJsonArray hyprctlJson(const char *what)
{
    QProcess p;
    p.setStandardInputFile(QProcess::nullDevice());
    p.start(QStringLiteral("hyprctl"), {QStringLiteral("-j"), QString::fromLatin1(what)});
    if (!p.waitForFinished(800)) { p.kill(); return {}; }
    return QJsonDocument::fromJson(p.readAllStandardOutput()).array();
}

bool windowRectOf(const QString &address, QRect *out)
{
    for (const QJsonValue &v : hyprctlJson("clients")) {
        const QJsonObject c = v.toObject();
        if (c.value(QStringLiteral("address")).toString() != address) continue;
        if (!c.value(QStringLiteral("mapped")).toBool(true)) return false;
        const QJsonArray at = c.value(QStringLiteral("at")).toArray(), sz = c.value(QStringLiteral("size")).toArray();
        if (at.size() < 2 || sz.size() < 2 || sz[0].toInt() < 16 || sz[1].toInt() < 16) return false;
        *out = QRect(at[0].toInt(), at[1].toInt(), sz[0].toInt(), sz[1].toInt());
        return true;
    }
    return false;
}
}

namespace {
// видеокарта для VAAPI: первая renderD* от Intel (драйвер iHD) или AMD (radeonsi). NVIDIA пропускается — для неё VAAPI нужен отдельный драйвер,
// а будить дискретную карту ноутбука ради показа экрана незачем
bool findVaapiDevice(QString *device, QString *driver)
{
    for (int n = 128; n < 136; ++n) {
        QFile vf(QStringLiteral("/sys/class/drm/renderD%1/device/vendor").arg(n));
        if (!vf.open(QIODevice::ReadOnly)) continue;
        const QByteArray vendor = vf.readAll().trimmed();
        if (vendor == "0x8086") { *device = QStringLiteral("/dev/dri/renderD%1").arg(n); *driver = QStringLiteral("iHD"); return true; }
        if (vendor == "0x1002") { *device = QStringLiteral("/dev/dri/renderD%1").arg(n); *driver = QStringLiteral("radeonsi"); return true; }
    }
    return false;
}
}

bool ScreenCaptureSession::fallbackFromGpu()
{
    if (!m_video || !m_gpu) return false;
    m_gpuFailed = true;
    log(QStringLiteral("Видеокарта (VAAPI) не выдала кадров — дальше видео кодирует процессор (x264)"));
    stopWf(true);
    startWf();
    return true;
}

QVariantList ScreenCaptureSession::visibleWindows()
{
    QVariantList out;
    // окно видно, если его рабочий стол сейчас показан на каком-то мониторе: остальные wlr-screencopy не отдаёт
    QHash<int, int> activeOnMonitor;
    for (const QJsonValue &v : hyprctlJson("monitors")) {
        const QJsonObject m = v.toObject();
        activeOnMonitor.insert(m.value(QStringLiteral("id")).toInt(), m.value(QStringLiteral("activeWorkspace")).toObject().value(QStringLiteral("id")).toInt());
    }
    for (const QJsonValue &v : hyprctlJson("clients")) {
        const QJsonObject c = v.toObject();
        if (!c.value(QStringLiteral("mapped")).toBool(true) || c.value(QStringLiteral("hidden")).toBool(false)) continue;
        if (activeOnMonitor.value(c.value(QStringLiteral("monitor")).toInt(), -1) != c.value(QStringLiteral("workspace")).toObject().value(QStringLiteral("id")).toInt()) continue;
        const QString cls = c.value(QStringLiteral("class")).toString(), title = c.value(QStringLiteral("title")).toString();
        if (cls == QLatin1String("nameless-orbit")) continue;               // самого себя показывать незачем
        QString label = cls.isEmpty() ? title : (title.isEmpty() || title == cls ? cls : cls + QStringLiteral(" — ") + title);
        if (label.size() > 52) label = label.left(51) + QChar(0x2026);
        QVariantMap m;
        m[QStringLiteral("address")] = c.value(QStringLiteral("address")).toString();
        m[QStringLiteral("label")] = label;
        out.append(m);
    }
    return out;
}

bool ScreenCaptureSession::currentWindowRect(QRect *r) const
{
    if (m_screenName.startsWith(QLatin1String("dwm:"))) return VirtualDisplay::driftwmWindowRect(m_screenName.mid(4).toInt(), r);
    return windowRectOf(m_screenName.mid(7), r);
}

void ScreenCaptureSession::checkWindow()
{
    if (!m_wfActive || !windowSource()) return;
    QRect now;
    if (!currentWindowRect(&now)) { m_winTimer.stop(); stopWf(); emit sourceLost(); return; }       // окно закрыто
    if (qAbs(now.x() - m_winRect.x()) + qAbs(now.y() - m_winRect.y()) + qAbs(now.width() - m_winRect.width()) + qAbs(now.height() - m_winRect.height()) > 6)
        scheduleWfRestart();                                                                                    // окно сдвинули или изменили размер — захват следует за ним
}

void ScreenCaptureSession::scheduleWfRestart(int delayMs) { m_wfRestart.start(delayMs); }

QScreen *ScreenCaptureSession::pickScreen() const
{
    if (!m_screenName.isEmpty())
        for (QScreen *s : QGuiApplication::screens()) if (s->name() == m_screenName) return s;
    return QGuiApplication::primaryScreen();
}

void ScreenCaptureSession::startWf()
{
    const bool virt = virtualSource() && m_virtualSize.isValid();
    QScreen *scr = virt ? nullptr : pickScreen();
    if (!scr && !virt) { startQtCapture(); return; }
    const double screenDpr = scr ? scr->devicePixelRatio() : 1.0;

    m_wfStartMs = QDateTime::currentMSecsSinceEpoch();
    m_wfFps = qBound(5, m_targetFps, 60);
    m_wfWidth = m_maxWidth;
    m_wfQuality = m_jpegQuality;
    m_wfRegion = m_region;
    m_wfBuf.clear();
    m_wfScan = 0;
    m_wfGotFrame = false;
    m_lastJpeg.clear();

    // качество 10..95 → qscale mjpeg 15..2 (меньше — лучше)
    const int qv = qBound(2, int(2 + (95 - m_wfQuality) * 13.0 / 85.0 + 0.5), 20);
    QStringList args;
    QRect base = virt ? QRect(QPoint(0, 0), m_virtualSize) : scr->geometry();
    const bool win = windowSource();
    if (win) {
        // окно: снимается его прямоугольник на экране (то, что видно на этом месте), а не отдельный буфер окна — поэтому перекрывающее окно попадёт в кадр
        if (!currentWindowRect(&base)) { emit sourceLost(); return; }
        m_winRect = base;
        m_winTimer.start();
    } else {
        // у виртуального монитора кадров по повреждениям нет, пока на нём ничего не меняется, — снимаем непрерывно (-D)
        if (virt) args << QStringLiteral("-D");
        args << QStringLiteral("-o") << (virt ? m_screenName : scr->name());
        m_winTimer.stop();
    }
    QRect capRect = base;
    if (win || m_wfRegion.width() < 0.999 || m_wfRegion.height() < 0.999) {
        const QRect r(base.x() + int(m_wfRegion.x() * base.width()), base.y() + int(m_wfRegion.y() * base.height()),
                      qMax(16, int(m_wfRegion.width() * base.width())), qMax(16, int(m_wfRegion.height() * base.height())));
        args << QStringLiteral("-g") << QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
        capRect = r;
    }
    stopRaw();
    m_video = m_wantVideo && qEnvironmentVariable("ORBIT_VIDEO") != QLatin1String("0");
    m_vidW = m_vidH = 0;
    m_dropUntilKey = false;
    QString vaDevice, vaDriver;
    m_gpu = m_video && m_encPref != QLatin1String("cpu") && !m_gpuFailed && findVaapiDevice(&vaDevice, &vaDriver);
    if (m_gpu) {
        // H.264 на видеокарте (VAAPI): захват, масштаб и сжатие не трогают процессор (замер: 37 к/с при 5 % ЦП). Размер кадра — по окну зрителя
        // и ступени лесенки (scale_vaapi с явным nv12: без формата и с h=-2 фильтр не запускался). Без B-кадров — без лишней задержки;
        // ключевой кадр раз в секунду, AUD в начале каждого кадра. Скорость: качество 30..95 → 3..16 Мбит/с, всплески до полутора раз больше
        const double dpr = screenDpr;
        const int srcW = qMax(64, int(capRect.width() * dpr));
        const int vw = qMax(320, qMin(m_wfWidth, srcW) & ~1);
        const int vh = qMax(180, int(qRound(double(vw) * capRect.height() / qMax(1, capRect.width()))) & ~1);
        const int mbit = qBound(3, int(3 + (m_wfQuality - 30) * 0.2 + 0.5), 16);
        const double areaK = double(vw) * vh / (1920.0 * 1080.0);
        const int gop = qMax(15, m_wfFps);
        args << QStringLiteral("-c") << QStringLiteral("h264_vaapi") << QStringLiteral("-d") << vaDevice << QStringLiteral("-m") << QStringLiteral("h264")
             << QStringLiteral("-r") << QString::number(m_wfFps)
             << QStringLiteral("-b") << QStringLiteral("0");
        if (vw < srcW) args << QStringLiteral("-F") << QStringLiteral("scale_vaapi=w=%1:h=%2:format=nv12").arg(vw).arg(vh);
        // low_power (VDEnc) + async_depth=1 + constrained baseline: по замеру на этом ноутбуке при том же 40 к/с поток 331 КБ/с вместо 598,
        // первый кадр на 35 мс раньше и меньше нагрузка на видеокарту; без B-кадров и CABAC декодирование у зрителя тоже быстрее
        args << QStringLiteral("-p") << QStringLiteral("g=%1").arg(gop)
             << QStringLiteral("-p") << QStringLiteral("aud=1")
             << QStringLiteral("-p") << QStringLiteral("low_power=1")
             << QStringLiteral("-p") << QStringLiteral("async_depth=1")
             << QStringLiteral("-p") << QStringLiteral("profile=constrained_baseline")
             << QStringLiteral("-p") << QStringLiteral("rc_mode=VBR")
             // скорость — по площади кадра, а не по ширине: у высокого экрана (портретный второй экран 1068×2388) по ширине выходило
             // ~3 Мбит/с на 2,5 млн точек — текст «мылился»
             << QStringLiteral("-p") << QStringLiteral("b=%1M").arg(qBound(2, int(mbit * areaK + 0.5), 40))
             << QStringLiteral("-p") << QStringLiteral("maxrate=%1M").arg(qBound(3, int(mbit * areaK * 1.5 + 0.5), 60))
             << QStringLiteral("-f") << QStringLiteral("/dev/stdout");
    } else if (m_video) {
        // H.264 x264 без задержки: ключевой кадр раз в секунду (с него зритель начинает разжатие), AUD в начале каждого кадра — по ним поток делится на кадры,
        // SPS/PPS при каждом ключевом; цветовая матрица BT.601 помечена явно (так переводит swscale), иначе телефон считает её BT.709 и цвета чуть плывут.
        // Качество 30..95 → crf 36..20; потолок скорости ограничивает всплески на движущихся сценах
        const int crf = qBound(18, int(20 + (95 - m_wfQuality) * 16.0 / 65.0 + 0.5), 38);
        // потолок скорости — по площади кадра (кбит/с): ~14 Мбит/с на 1080p, у высокого экрана — больше, у маленького окна — меньше
        const double outW = qMin(m_wfWidth, int(capRect.width() * screenDpr)), outH = outW * capRect.height() / qMax(1, capRect.width());
        const int maxRate = qBound(3000, int(14000.0 * outW * outH / (1920.0 * 1080.0)), 40000);
        const int gop = qMax(15, m_wfFps);
        args << QStringLiteral("-c") << QStringLiteral("libx264") << QStringLiteral("-m") << QStringLiteral("h264")
             << QStringLiteral("-r") << QString::number(m_wfFps)
             << QStringLiteral("-x") << QStringLiteral("yuv420p")
             << QStringLiteral("-F") << QStringLiteral("scale=w='min(%1,iw)':h=-2").arg(m_wfWidth)
             << QStringLiteral("-p") << QStringLiteral("preset=ultrafast")
             << QStringLiteral("-p") << QStringLiteral("tune=zerolatency")
             << QStringLiteral("-p") << QStringLiteral("crf=%1").arg(crf)
             << QStringLiteral("-p") << QStringLiteral("x264-params=aud=1:repeat-headers=1:keyint=%1:min-keyint=%1:scenecut=0:vbv-maxrate=%2:vbv-bufsize=%3:colormatrix=smpte170m:colorprim=smpte170m:transfer=bt709:range=tv")
                                          .arg(gop).arg(maxRate).arg(maxRate / 4)
             << QStringLiteral("-f") << QStringLiteral("/dev/stdout");
    } else if (m_tiles.load()) {
        // «Тайлы Orbit»: сырые кадры нужного размера (масштаб — wf-recorder, быстрый) идут через именованный канал с буфером 1 МБ; плитки считает Orbit.
        // Размер задан явно (scale=W:H), поэтому длина кадра известна заранее и поток режется без маркеров
        const double dpr = screenDpr;
        const int srcW = qMax(64, int(capRect.width() * dpr));
        const int W = qMax(64, qMin(m_wfWidth, srcW) & ~1);
        const int H = qMax(64, int(qRound(double(W) * capRect.height() / qMax(1, capRect.width()))) & ~1);
        m_rawW = W; m_rawH = H;
        m_rawSource = QSize(srcW, qMax(64, int(capRect.height() * dpr)));
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDir().mkpath(dir);
        for (const QString &old : QDir(dir).entryList({QStringLiteral("orbit-raw-*.fifo")}, QDir::System | QDir::Files))
            QFile::remove(dir + QLatin1Char('/') + old);          // каналы от прошлых (упавших) запусков
        m_rawPath = dir + QStringLiteral("/orbit-raw-%1.fifo").arg(QCoreApplication::applicationPid());
        ::unlink(m_rawPath.toLocal8Bit().constData());
        if (::mkfifo(m_rawPath.toLocal8Bit().constData(), 0600) == 0) m_rawFd = ::open(m_rawPath.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
        if (m_rawFd < 0) { log(QStringLiteral("Не удалось создать канал для сырых кадров — перехожу на захват средствами Qt")); startQtCapture(); return; }
        ::fcntl(m_rawFd, F_SETPIPE_SZ, 1 << 20);
        m_raw = true;
        m_rawFill = 0;
        m_rawFrame = QByteArray(W * H * 4, Qt::Uninitialized);
        m_rawNotifier = new QSocketNotifier(m_rawFd, QSocketNotifier::Read, this);
        connect(m_rawNotifier, &QSocketNotifier::activated, this, &ScreenCaptureSession::onRawReadable);
        args << QStringLiteral("-c") << QStringLiteral("rawvideo") << QStringLiteral("-m") << QStringLiteral("rawvideo")
             << QStringLiteral("-r") << QString::number(m_wfFps)
             << QStringLiteral("-x") << QStringLiteral("bgr0")
             << QStringLiteral("-F") << QStringLiteral("scale=%1:%2:flags=fast_bilinear").arg(W).arg(H)
             << QStringLiteral("-f") << m_rawPath;
    } else {
        args << QStringLiteral("-c") << QStringLiteral("mjpeg") << QStringLiteral("-m") << QStringLiteral("mjpeg")
             << QStringLiteral("-r") << QString::number(m_wfFps)
             << QStringLiteral("-x") << QStringLiteral("yuvj420p")          // 4:2:0: кадр почти вдвое меньше и быстрее разжимается на телефоне
             << QStringLiteral("-F") << QStringLiteral("scale=w='min(%1,iw)':h=-2").arg(m_wfWidth)
             << QStringLiteral("-p") << QStringLiteral("q:v=%1").arg(qv)
             << QStringLiteral("-f") << QStringLiteral("/dev/stdout");
    }

    log(QStringLiteral("Запускаю wf-recorder: экран — %1, видео H.264 %2, кодирует %3. Команда: wf-recorder %4").arg(m_screenName.isEmpty() ? QStringLiteral("основной") : m_screenName, m_video ? QStringLiteral("включено") : QStringLiteral("выключено"), m_gpu ? QStringLiteral("видеокарта (%1)").arg(vaDriver) : QStringLiteral("процессор"), args.join(QLatin1Char(' '))));
    m_wf = new QProcess(this);
    // если Orbit закроется нештатно (падение, kill), wf-recorder завершится вместе с ним: раньше такие «сироты» продолжали снимать экран
    // в канал, который никто не читает, и отнимали захват у композитора и у нового показа — отсюда просадки
    m_wf->setChildProcessModifier([]() { ::prctl(PR_SET_PDEATHSIG, SIGKILL); });
    m_wf->setStandardErrorFile(QProcess::nullDevice());
    m_wf->setStandardInputFile(QProcess::nullDevice());      // иначе wf-recorder ждёт чего-то на открытом пустом stdin и не отдаёт ни одного кадра
    if (m_gpu) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("LIBVA_DRIVER_NAME"), vaDriver);       // без явного драйвера libva на гибридной графике не находит нужный и падает
        m_wf->setProcessEnvironment(env);
    }
    if (!m_raw) connect(m_wf, &QProcess::readyReadStandardOutput, this, &ScreenCaptureSession::onWfData);
    QProcess *proc = m_wf;
    connect(m_wf, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e) {
        if (proc != m_wf || e != QProcess::FailedToStart) return;
        stopWf();
        startQtCapture();
    });
    connect(m_wf, &QProcess::finished, this, [this, proc]() {
        if (proc != m_wf) return;           // перезапуск: старый процесс убран намеренно
        const bool hadFrames = m_wfGotFrame;
        if (!hadFrames && fallbackFromGpu()) return;     // видеокарта не завелась — тот же показ на процессоре
        stopWf();
        if (!hadFrames) startQtCapture();   // wf-recorder не смог (нет протокола, чужой композитор) — прежний захват
        else { m_errorString = tr("Захват экрана остановлен"); emit errorChanged(); }
    });
    m_wf->start(QStringLiteral("wf-recorder"), args, QIODevice::ReadOnly);
    // страховка: за 5 секунд не пришло ни одного кадра — быстрый путь не работает на этой системе, включается прежний захват
    QTimer::singleShot(5000, this, [this, proc]() {
        if (proc != m_wf || m_wfGotFrame) return;
        if (fallbackFromGpu()) return;
        qWarning() << "ScreenCaptureSession: wf-recorder не отдал кадров, переход на QScreenCapture";
        log(QStringLiteral("wf-recorder за 5 секунд не прислал кадров — перехожу на захват средствами Qt (JPEG)"));
        stopWf();
        startQtCapture();
    });
    if (!m_wfActive) { m_wfActive = true; emit activeChanged(); }
}

void ScreenCaptureSession::stopWf(bool keepActive)
{
    if (m_wf) {
        QProcess *p = m_wf;
        m_wf = nullptr;
        p->disconnect(this);
        p->kill();
        p->deleteLater();
    }
    stopRaw();
    m_wfBuf.clear();
    if (m_wfActive && !keepActive) { m_wfActive = false; m_winTimer.stop(); emit activeChanged(); }
}

void ScreenCaptureSession::stopRaw()
{
    if (m_rawNotifier) { m_rawNotifier->setEnabled(false); m_rawNotifier->deleteLater(); m_rawNotifier = nullptr; }
    if (m_rawFd >= 0) { ::close(m_rawFd); m_rawFd = -1; }
    if (!m_rawPath.isEmpty()) { ::unlink(m_rawPath.toLocal8Bit().constData()); m_rawPath.clear(); }
    m_raw = false;
    m_rawFrame.clear();
    m_rawFill = 0;
}

// Читаем кадр целиком в заранее выделенный буфер; готовый кадр, если сейчас его можно брать, уходит в тот же движок плиток, что и у QScreenCapture
void ScreenCaptureSession::onRawReadable()
{
    if (m_rawFd < 0 || m_rawFrame.isEmpty()) return;
    for (int guard = 0; guard < 64; ++guard) {
        const int need = m_rawFrame.size() - m_rawFill;
        const ssize_t n = ::read(m_rawFd, m_rawFrame.data() + m_rawFill, size_t(need));
        if (n <= 0) return;                        // нет данных (EAGAIN) или писатель закрылся — завершение поймает QProcess::finished
        m_rawFill += int(n);
        if (m_rawFill < m_rawFrame.size()) continue;
        m_rawFill = 0;
        m_wfGotFrame = true;
        if (!m_capClock.isValid()) m_capClock.start();
        ++m_capCount;
        if (m_capClock.elapsed() >= 1000) { m_capFps = m_capCount; m_capCount = 0; m_capClock.restart(); }
        if (!acceptNow()) continue;                // кадр пропущен — буфер остаётся тем же, выделять заново не нужно
        // буфер уходит в картинку без копирования; новый выделяется под следующий кадр
        QByteArray *holder = new QByteArray(std::move(m_rawFrame));
        m_rawFrame = QByteArray(m_rawW * m_rawH * 4, Qt::Uninitialized);
        const QImage img(reinterpret_cast<const uchar *>(holder->constData()), m_rawW, m_rawH, m_rawW * 4, QImage::Format_RGB32,
                         [](void *p) { delete static_cast<QByteArray *>(p); }, holder);
        submitImage(img, m_rawSource);
    }
}

void ScreenCaptureSession::onWfData()
{
    if (!m_wf) return;
    if (m_video) { onVideoData(); return; }
    m_wfBuf += m_wf->readAllStandardOutput();
    // кадры идут подряд: SOI (FFD8) … EOI (FFD9); у mjpeg-кадров ffmpeg внутри нет миниатюр, поэтому первая пара FFD9 — конец кадра
    for (;;) {
        const int soi = m_wfBuf.indexOf("\xff\xd8", 0);
        if (soi < 0) { m_wfBuf.clear(); m_wfScan = 0; return; }
        if (soi > 0) { m_wfBuf.remove(0, soi); m_wfScan = 0; }
        const int eoi = m_wfBuf.indexOf("\xff\xd9", qMax(2, m_wfScan));
        if (eoi < 0) { m_wfScan = qMax(0, int(m_wfBuf.size()) - 1); break; }
        const QByteArray jpeg = m_wfBuf.left(eoi + 2);
        m_wfBuf.remove(0, eoi + 2);
        m_wfScan = 0;
        onWfFrame(jpeg);
    }
    if (m_wfBuf.size() > 32 * 1024 * 1024) { m_wfBuf.clear(); m_wfScan = 0; }      // защита от мусора
}


namespace {
// чтение битов из SPS (с удалением байтов предотвращения эмуляции 00 00 03)
struct BitReader {
    QByteArray d; int pos = 0;
    explicit BitReader(const QByteArray &nal) {
        for (int i = 0; i < nal.size(); ++i) {
            if (i >= 2 && nal[i] == 3 && nal[i - 1] == 0 && nal[i - 2] == 0) continue;
            d.append(nal[i]);
        }
    }
    int bit() { const int byte = pos >> 3; if (byte >= d.size()) { ++pos; return 0; } const int v = (uchar(d[byte]) >> (7 - (pos & 7))) & 1; ++pos; return v; }
    quint32 u(int n) { quint32 v = 0; while (n--) v = (v << 1) | quint32(bit()); return v; }
    quint32 ue() { int z = 0; while (bit() == 0 && z < 32) ++z; return z ? ((1u << z) - 1 + u(z)) : 0; }
};

// ширина и высота кадра из SPS (NAL без стартового кода, начиная с байта заголовка)
bool spsSize(const QByteArray &nal, int *w, int *h)
{
    if (nal.size() < 8) return false;
    BitReader r(nal.mid(1));
    const quint32 profile = r.u(8);
    r.u(8); r.u(8);
    r.ue();
    quint32 chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 || profile == 118 || profile == 128) {
        chroma = r.ue();
        if (chroma == 3) r.u(1);
        r.ue(); r.ue(); r.u(1);
        if (r.u(1)) {                              // матрицы масштабирования: на размер не влияют, но их нужно пропустить
            const int lists = chroma == 3 ? 12 : 8;
            for (int i = 0; i < lists; ++i) {
                if (!r.u(1)) continue;
                const int size = i < 6 ? 16 : 64;
                int last = 8, next = 8;
                for (int j = 0; j < size; ++j) {
                    if (next != 0) {
                        const quint32 code = r.ue();
                        const int delta = (code & 1) ? int((code + 1) / 2) : -int(code / 2);       // знаковое se(v)
                        next = (last + delta + 256) % 256;
                    }
                    last = next == 0 ? last : next;
                }
            }
        }
    }
    r.ue();
    const quint32 pocType = r.ue();
    if (pocType == 0) r.ue(); else if (pocType == 1) return false;
    r.ue(); r.u(1);
    const quint32 wMbs = r.ue() + 1, hMap = r.ue() + 1;
    const quint32 frameMbsOnly = r.u(1);
    if (!frameMbsOnly) r.u(1);
    r.u(1);
    quint32 cl = 0, cr = 0, ct = 0, cb = 0;
    if (r.u(1)) { cl = r.ue(); cr = r.ue(); ct = r.ue(); cb = r.ue(); }
    const quint32 cux = chroma == 0 || chroma == 3 ? 1 : 2, cuy = (chroma == 1 ? 2 : 1) * (2 - frameMbsOnly);
    const int ww = int(wMbs * 16 - (cl + cr) * cux), hh = int((2 - frameMbsOnly) * hMap * 16 - (ct + cb) * cuy);
    if (ww < 16 || hh < 16 || ww > 8192 || hh > 8192) return false;
    *w = ww; *h = hh;
    return true;
}

// позиция начала AUD (00 00 01 09; 4-байтный код — на байт раньше) начиная с from; -1 — нет
int findAud(const QByteArray &b, int from)
{
    for (;;) {
        const int p = b.indexOf(QByteArrayLiteral("\x00\x00\x01"), from);
        if (p < 0 || p + 3 >= b.size()) return -1;
        if ((uchar(b[p + 3]) & 0x1F) == 9) return (p > 0 && b[p - 1] == 0) ? p - 1 : p;
        from = p + 3;
    }
}
}

void ScreenCaptureSession::onVideoData()
{
    m_wfBuf += m_wf->readAllStandardOutput();
    m_vidFlush.start();
    int first = findAud(m_wfBuf, 0);
    if (first < 0) { if (m_wfBuf.size() > 4 * 1024 * 1024) m_wfBuf.clear(); return; }
    if (first > 0) { m_wfBuf.remove(0, first); }
    for (;;) {
        const int next = findAud(m_wfBuf, 5);
        if (next < 0) break;
        const QByteArray au = m_wfBuf.left(next);
        m_wfBuf.remove(0, next);
        emitVideoUnit(au);
    }
    if (m_wfBuf.size() > 8 * 1024 * 1024) m_wfBuf.clear();
}

void ScreenCaptureSession::emitVideoUnit(const QByteArray &au)
{
    m_wfGotFrame = true;
    if (!m_capClock.isValid()) m_capClock.start();
    ++m_capCount;
    if (m_capClock.elapsed() >= 1000) { m_capFps = m_capCount; m_capCount = 0; m_capClock.restart(); }

    // тип NAL: ключевой кадр — IDR (5); SPS (7) даёт размер
    bool key = false;
    for (int p = au.indexOf(QByteArrayLiteral("\x00\x00\x01")); p >= 0 && p + 3 < au.size(); p = au.indexOf(QByteArrayLiteral("\x00\x00\x01"), p + 3)) {
        const int type = uchar(au[p + 3]) & 0x1F;
        if (type == 5) key = true;
        else if (type == 7) {
            int e = au.indexOf(QByteArrayLiteral("\x00\x00\x01"), p + 4);
            const QByteArray sps = au.mid(p + 3, e < 0 ? -1 : e - (p + 3));
            int w = 0, h = 0;
            if (spsSize(sps, &w, &h)) { m_vidW = w; m_vidH = h; }
        }
    }
    if (m_vidW <= 0) return;                               // размер ещё неизвестен — до первого SPS слать нечего
    if (key) m_lastKeyUnitMs = QDateTime::currentMSecsSinceEpoch();

    // отправлять нельзя — пропущенный кадр ломает все следующие до ключевого, поэтому после пропуска ждём ключевой
    if (!key && m_dropUntilKey) return;
    if (m_gate && !m_gate()) { m_dropUntilKey = true; return; }
    if (key) m_dropUntilKey = false;

    QScreen *scr = pickScreen();
    const QSize source = virtualSource() && m_virtualSize.isValid() ? m_virtualSize : (windowSource() ? (m_winRect.size() * (scr ? scr->devicePixelRatio() : 1.0)) : (scr ? (scr->size() * scr->devicePixelRatio()) : QSize(m_vidW, m_vidH)));
    emit videoReady(au, m_vidW, m_vidH, key, m_wfRegion, source, QDateTime::currentMSecsSinceEpoch());
}

void ScreenCaptureSession::onWfFrame(const QByteArray &jpeg)
{
    m_wfGotFrame = true;
    if (!m_capClock.isValid()) m_capClock.start();
    ++m_capCount;
    if (m_capClock.elapsed() >= 1000) { m_capFps = m_capCount; m_capCount = 0; m_capClock.restart(); }

    // экран не менялся: кодер повторяет кадр по заданной частоте, и такие копии (байт в байт) не отправляются — сеть и телефон ничего не тратят,
    // а картинка та же; «связь жива» уходит отдельно, раз в две секунды
    if (jpeg == m_lastJpeg) return;

    const qint64 minIntervalMs = 1000 / qMax(1, m_targetFps);
    if (m_frameTimer.isValid() && m_frameTimer.elapsed() < minIntervalMs - 3) return;
    if (m_gate && !m_gate()) return;                 // зритель не успевает — кадр отбрасывается без пересылки

    // размер кадра — из заголовка SOF (C0/C1/C2): сжимать и разбирать картинку целиком не нужно
    int W = 0, H = 0;
    const uchar *d = reinterpret_cast<const uchar *>(jpeg.constData());
    const int n = jpeg.size();
    for (int i = 2; i + 9 < n;) {
        if (d[i] != 0xFF) { ++i; continue; }
        const uchar m = d[i + 1];
        if (m == 0xFF) { ++i; continue; }
        if (m >= 0xC0 && m <= 0xC2) { H = (d[i + 5] << 8) | d[i + 6]; W = (d[i + 7] << 8) | d[i + 8]; break; }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { i += 2; continue; }
        i += 2 + ((d[i + 2] << 8) | d[i + 3]);
    }
    if (W <= 0 || H <= 0) return;

    m_lastJpeg = jpeg;                  // запоминается только отправленный кадр: пропущенный по частоте или сети должен дойти следующей копией
    m_frameTimer.restart();
    m_forceKey = false;
    QScreen *scr = pickScreen();
    const QSize source = virtualSource() && m_virtualSize.isValid() ? m_virtualSize : (windowSource() ? (m_winRect.size() * (scr ? scr->devicePixelRatio() : 1.0)) : (scr ? (scr->size() * scr->devicePixelRatio()) : QSize(W, H)));
    emit frameReady({FramePart{0, 0, W, H, jpeg}}, W, H, true, m_wfRegion, source, QDateTime::currentMSecsSinceEpoch());
}
#else
bool ScreenCaptureSession::wfAvailable() const { return false; }
void ScreenCaptureSession::scheduleWfRestart(int) {}
void ScreenCaptureSession::startWf() { startQtCapture(); }
void ScreenCaptureSession::stopWf(bool) {}
void ScreenCaptureSession::onWfData() {}
void ScreenCaptureSession::onRawReadable() {}
void ScreenCaptureSession::stopRaw() {}
void ScreenCaptureSession::onWfFrame(const QByteArray &) {}
void ScreenCaptureSession::onVideoData() {}
bool ScreenCaptureSession::currentWindowRect(QRect *) const { return false; }
void ScreenCaptureSession::checkWindow() {}
bool ScreenCaptureSession::fallbackFromGpu() { return false; }
QVariantList ScreenCaptureSession::visibleWindows() { return {}; }
void ScreenCaptureSession::emitVideoUnit(const QByteArray &) {}
#endif

namespace {
constexpr int TILE = 64;

QByteArray jpegOf(const QImage &img, int quality)
{
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", quality);
    buf.close();
    return out;
}

// Плитка без потерь: в каждой строке пиксель заменяется разностью с левым соседом (фильтр Sub из PNG), дальше zlib первого уровня —
// быстро и для интерфейса, текста и однотонных областей сжимает в 3–10 раз. Обратное действие — в RemoteDesktopController::decodeLoop
QByteArray losslessOf(const QImage &tile)
{
    const int w = tile.width(), h = tile.height(), row = w * 4;
    QByteArray raw(row * h, Qt::Uninitialized);
    uchar *o = reinterpret_cast<uchar *>(raw.data());
    for (int y = 0; y < h; ++y) {
        const uchar *s = tile.constScanLine(y);
        uchar *d = o + y * row;
        for (int i = 0; i < 4 && i < row; ++i) d[i] = s[i];
        for (int i = 4; i < row; ++i) d[i] = uchar(s[i] - s[i - 4]);
    }
    return qCompress(raw, 1);
}

// Один участок: без потерь, если он хорошо сжимается (интерфейс, текст), иначе JPEG (фото, видео); force — без потерь в любом случае
FramePart encodePart(const QRect &px, const QImage &tile, int quality, bool allowLossless, bool force)
{
    FramePart fp;
    fp.x = px.x(); fp.y = px.y(); fp.w = px.width(); fp.h = px.height();
    if (allowLossless) {
        // однотонный участок (фон окна, пустое поле): достаточно одного цвета — 4 байта вместо сотен; у интерфейса таких большинство
        const quint32 first = *reinterpret_cast<const quint32 *>(tile.constScanLine(0));
        bool solid = true;
        for (int y = 0; y < tile.height() && solid; ++y) {
            const quint32 *row = reinterpret_cast<const quint32 *>(tile.constScanLine(y));
            for (int x = 0; x < tile.width(); ++x) if (row[x] != first) { solid = false; break; }
        }
        if (solid) { fp.jpeg = QByteArray(reinterpret_cast<const char *>(&first), 4); fp.fmt = 's'; return fp; }
        const QByteArray z = losslessOf(tile);
        if (force || qint64(z.size()) * 100 < qint64(tile.width()) * tile.height() * 4 * 30) { fp.jpeg = z; fp.fmt = 'z'; return fp; }
    }
    fp.jpeg = jpegOf(tile, quality);
    fp.fmt = 'j';
    return fp;
}

struct TileRect { int x0, x1, y0, y1; };     // в плитках, правая и нижняя границы не включены
}

bool ScreenCaptureSession::acceptNow()
{
    // лишние кадры между тактами и пока предыдущий ещё сжимается просто пропускаются — очередь и задержка не копятся
    const qint64 minIntervalMs = 1000 / qMax(1, m_targetFps);
    if (m_frameTimer.isValid() && m_frameTimer.elapsed() < minIntervalMs - 3) return false;      // допуск 3 мс: кадры приходят с шагом экрана и иначе «округлялись» вниз до каждого второго-третьего
    if (m_inFlight.load() >= 2) return false;
    if (m_gate && !m_gate()) return false;             // зритель не успевает — кадр даже не снимаем
    return true;
}

void ScreenCaptureSession::setTilesMode(bool on)
{
    if (m_tiles.load() == on) return;
    m_tiles = on;
    m_forceKey = true;
    if (!on) m_refineTimer.stop();
    if (m_wfActive && !m_video) scheduleWfRestart(400);    // источник другой: JPEG-кадры целиком ↔ сырой поток для плиток
}

void ScreenCaptureSession::submitExternal(const QImage &image)
{
    if (image.isNull()) return;
    if (!m_capClock.isValid()) m_capClock.start();
    ++m_capCount;
    if (m_capClock.elapsed() >= 1000) { m_capFps = m_capCount; m_capCount = 0; m_capClock.restart(); }
    if (!acceptNow()) return;
    submitImage(image, image.size());
}

void ScreenCaptureSession::onVideoFrameChanged(const QVideoFrame &frame)
{
    if (!frame.isValid()) return;
    if (!m_capClock.isValid()) m_capClock.start();
    ++m_capCount;
    if (m_capClock.elapsed() >= 1000) { m_capFps = m_capCount; m_capCount = 0; m_capClock.restart(); }
    if (!acceptNow()) return;
    const QImage image = frame.toImage();               // единственное, что делается в потоке интерфейса: забрать пиксели кадра
    if (image.isNull()) return;
    submitImage(image, image.size());
}

namespace {
// Много независимых участков кодируются параллельно (по до трёх потоков): участки не зависят друг от друга, а кодирование — главная трата времени
QVector<FramePart> encodeAll(const QVector<QRect> &pxs, const QImage &cur, int quality, bool allowLossless)
{
    QVector<FramePart> out(pxs.size());
    auto work = [&](int a, int b) { for (int i = a; i < b; ++i) out[i] = encodePart(pxs[i], cur.copy(pxs[i]), quality, allowLossless, false); };
    const int n = int(pxs.size()), threads = qMin(3, n);
    if (threads <= 1) { work(0, n); return out; }
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) pool.emplace_back(work, n * t / threads, n * (t + 1) / threads);
    work(0, n / threads);
    for (std::thread &th : pool) th.join();
    return out;
}

// Хэши строк в полосе колонок [x0, x1) — для поиска прокрутки
QVector<size_t> rowHashes(const QImage &im, int x0, int x1)
{
    QVector<size_t> hs(im.height());
    for (int y = 0; y < im.height(); ++y)
        hs[y] = qHash(QByteArrayView(reinterpret_cast<const char *>(im.constScanLine(y)) + x0 * 4, (x1 - x0) * 4), 0);
    return hs;
}

struct Scroll { bool ok = false; int dy = 0, ya = 0, yb = 0, x0 = 0, x1 = 0; };

// Прокрутка по вертикали: содержимое средней полосы сдвинулось на dy строк. Строка cur[y] совпала со строкой prev[y - dy] — голосуем за сдвиг по строкам,
// затем берём самый длинный подряд совпавший участок. Колонки по бокам (панели, полосы прокрутки) в сдвиге не участвуют
Scroll findScroll(const QImage &prev, const QImage &cur)
{
    Scroll s;
    const int W = cur.width(), H = cur.height();
    s.x0 = (W / 5 / TILE) * TILE;
    s.x1 = qMin(W, ((W * 4 / 5 + TILE - 1) / TILE) * TILE);
    if (s.x1 - s.x0 < TILE * 2 || H < 256) return s;
    const QVector<size_t> hp = rowHashes(prev, s.x0, s.x1), hc = rowHashes(cur, s.x0, s.x1);
    QHash<size_t, int> freq, where;
    for (int y = 0; y < H; ++y) { ++freq[hp[y]]; where[hp[y]] = y; }
    QHash<int, int> votes;
    int sampled = 0;
    for (int y = 0; y < H; y += 3) {
        if (freq.value(hc[y], 0) != 1) continue;           // строка встречается не один раз (однотонный фон) — для сдвига ничего не говорит
        ++sampled;
        const int dy = y - where.value(hc[y]);
        if (dy != 0 && qAbs(dy) < H * 3 / 4) ++votes[dy];
    }
    int best = 0, bestVotes = 0;
    for (auto it = votes.constBegin(); it != votes.constEnd(); ++it) if (it.value() > bestVotes) { bestVotes = it.value(); best = it.key(); }
    if (bestVotes < 8) return s;
    int runA = 0, runLen = 0, curA = -1;
    for (int y = 0; y <= H; ++y) {
        const bool match = y < H && y - best >= 0 && y - best < H && hc[y] == hp[y - best];
        if (match) { if (curA < 0) curA = y; }
        else if (curA >= 0) { if (y - curA > runLen) { runLen = y - curA; runA = curA; } curA = -1; }
    }
    if (runLen < H / 5 || runLen < 96) return s;
    s.ok = true; s.dy = best; s.ya = runA; s.yb = runA + runLen;
    return s;
}
}

bool ScreenCaptureSession::submitImage(const QImage &image, const QSize &sourceSize)
{
    m_frameTimer.restart();
    ++m_inFlight;
    const qint64 captureMs = QDateTime::currentMSecsSinceEpoch();

    const int maxW = m_maxWidth, quality = m_jpegQuality;
    const QRectF region = m_region;
    const QSize source = sourceSize;
    const bool forced = m_forceKey.exchange(false);
    const bool tiles = m_tiles.load();
    const bool raw = m_raw;                // у сырого потока область и размер уже выбраны wf-recorder: вырезать и масштабировать нечего

    m_pool.start([this, image, maxW, quality, region, source, forced, captureMs, tiles, raw]() {
        // 1) нужный участок экрана и размер
        QImage cur = image;
        if (!raw && (region.width() < 0.999 || region.height() < 0.999)) {
            const QRect r(int(region.x() * image.width()), int(region.y() * image.height()),
                          qMax(16, int(region.width() * image.width())), qMax(16, int(region.height() * image.height())));
            cur = image.copy(r.intersected(image.rect()));
        }
        if (cur.width() > maxW)
            cur = cur.scaledToWidth(maxW, cur.width() > maxW * 2 ? Qt::FastTransformation : Qt::SmoothTransformation);
        if (cur.format() != QImage::Format_RGB32) cur = cur.convertToFormat(QImage::Format_RGB32);
        const int W = cur.width(), H = cur.height();

        // 2) что изменилось с прошлого кадра
        const int qDelta = qMin(92, quality + 16);                      // небольшие участки — качеством выше: текст и линии чёткие
        // «Черновик»: обновления идут подряд (движение, прокрутка) — целый кадр уходит ниже качеством и в разы меньше, а в покое будет доведён до чёткого
        const bool draft = tiles && captureMs - m_lastEmitMs < 350;
        const int keyQuality = draft ? qMax(28, quality - 22) : quality;
        bool key = forced || m_prev.size() != cur.size() || m_prev.isNull();
        QVector<TileRect> rects;
        QVector<FramePart> copies;                                       // «копировать у зрителя» — прокрутка
        const int tx = (W + TILE - 1) / TILE, ty = (H + TILE - 1) / TILE;
        if (m_lossy.size() != tx * ty) m_lossy = QVector<char>(tx * ty, 0);

        auto diff = [&](const QImage &base, QVector<char> *changed) {
            int count = 0;
            for (int j = 0; j < ty; ++j) {
                const int y0 = j * TILE, y1 = qMin(H, y0 + TILE);
                for (int i = 0; i < tx; ++i) {
                    const int x0 = i * TILE, w = qMin(TILE, W - x0);
                    for (int y = y0; y < y1; ++y) {
                        if (std::memcmp(cur.constScanLine(y) + x0 * 4, base.constScanLine(y) + x0 * 4, size_t(w) * 4) != 0) {
                            (*changed)[j * tx + i] = 1; ++count; break;
                        }
                    }
                }
            }
            return count;
        };

        if (!key) {
            QVector<char> changed(tx * ty, 0);
            int changedCount = diff(m_prev, &changed);
            // Много изменилось — возможно, это прокрутка: тогда зрителю уходит команда «сдвинь то, что у тебя есть», а по сети идёт только открывшаяся полоса
            if (tiles && changedCount * 5 > tx * ty) {
                const Scroll sc = findScroll(m_prev, cur);
                if (sc.ok) {
                    QImage shifted = m_prev;
                    shifted.detach();
                    const int rowBytes = (sc.x1 - sc.x0) * 4;
                    for (int y = sc.ya; y < sc.yb; ++y)
                        std::memcpy(shifted.scanLine(y) + sc.x0 * 4, m_prev.constScanLine(y - sc.dy) + sc.x0 * 4, size_t(rowBytes));
                    QVector<char> changed2(tx * ty, 0);
                    const int count2 = diff(shifted, &changed2);
                    if (count2 < changedCount * 7 / 10) {                 // сдвиг действительно выгоден
                        FramePart cp;
                        cp.fmt = 'c'; cp.x = sc.x0; cp.y = sc.ya; cp.w = sc.x1 - sc.x0; cp.h = sc.yb - sc.ya; cp.sx = sc.x0; cp.sy = sc.ya - sc.dy;
                        copies.append(cp);
                        // у перенесённого содержимого признак «ушло с потерями» переезжает вместе с ним (по плиткам, с запасом)
                        const QVector<char> old = m_lossy;
                        for (int j = sc.ya / TILE; j <= (sc.yb - 1) / TILE && j < ty; ++j)
                            for (int i = sc.x0 / TILE; i < sc.x1 / TILE && i < tx; ++i) {
                                const int srcRow = (j * TILE - sc.dy) / TILE;
                                char v = 0;
                                for (int k = srcRow - 1; k <= srcRow + 1; ++k) if (k >= 0 && k < ty) v |= old[k * tx + i];
                                m_lossy[j * tx + i] = v;
                            }
                        changed = changed2; changedCount = count2;
                    }
                }
            }
            if (changedCount == 0 && copies.isEmpty()) {            // экран не изменился — ничего не слать
                QMetaObject::invokeMethod(this, [this]() { --m_inFlight; }, Qt::QueuedConnection);
                return;
            }
            if (changedCount * 2 > tx * ty || QDateTime::currentMSecsSinceEpoch() - m_lastKeyMs > 6000) {
                key = true;                      // изменилась половина экрана или давно не было целого кадра
                copies.clear();
            } else if (changedCount > 0) {
                // горизонтальные отрезки подряд идущих плиток → прямоугольники (одинаковые по ширине отрезки в соседних рядах сливаются)
                QVector<TileRect> open, done;
                for (int j = 0; j < ty; ++j) {
                    QVector<TileRect> next;
                    int i = 0;
                    while (i < tx) {
                        if (!changed[j * tx + i]) { ++i; continue; }
                        int e = i;
                        while (e < tx && changed[j * tx + e]) ++e;
                        bool merged = false;
                        for (int k = 0; k < open.size(); ++k) {
                            if (open[k].x0 == i && open[k].x1 == e && open[k].y1 == j) {
                                TileRect r = open.takeAt(k); r.y1 = j + 1; next.append(r); merged = true; break;
                            }
                        }
                        if (!merged) next.append({i, e, j, j + 1});
                        i = e;
                    }
                    done += open;       // не продолжившиеся в этом ряду
                    open = next;
                }
                done += open;
                if (done.size() > 64) {         // слишком много кусочков: один общий прямоугольник дешевле
                    TileRect b{tx, 0, ty, 0};
                    for (const TileRect &r : std::as_const(done)) { b.x0 = qMin(b.x0, r.x0); b.x1 = qMax(b.x1, r.x1); b.y0 = qMin(b.y0, r.y0); b.y1 = qMax(b.y1, r.y1); }
                    done = {b};
                }
                rects = done;
            }
        }

        // 3) сжатие (участки кодируются параллельно)
        auto mark = [&](const QRect &px, char v) {
            for (int j = px.y() / TILE; j <= (px.bottom()) / TILE && j < ty; ++j)
                for (int i = px.x() / TILE; i <= (px.right()) / TILE && i < tx; ++i) m_lossy[j * tx + i] = v;
        };
        QVector<FramePart> parts;
        if (key) {
            // целый кадр — несколькими горизонтальными полосами: они кодируются параллельно
            const int strips = H >= 512 ? 4 : 1;
            QVector<QRect> pxs;
            for (int s = 0; s < strips; ++s) {
                const int y0 = (H * s / strips / TILE) * TILE, y1 = s + 1 == strips ? H : (H * (s + 1) / strips / TILE) * TILE;
                if (y1 > y0) pxs.append(QRect(0, y0, W, y1 - y0));
            }
            parts = encodeAll(pxs, cur, keyQuality, false);
            m_lastKeyMs = QDateTime::currentMSecsSinceEpoch();
            m_lossy.fill(1);                     // целый кадр ушёл с потерями — в покое он будет доведён до чёткого
        } else {
            parts += copies;
            QVector<QRect> pxs;
            for (const TileRect &r : std::as_const(rects))
                pxs.append(QRect(r.x0 * TILE, r.y0 * TILE, qMin(W, r.x1 * TILE) - r.x0 * TILE, qMin(H, r.y1 * TILE) - r.y0 * TILE));
            const QVector<FramePart> enc = encodeAll(pxs, cur, draft ? qMax(30, qDelta - 18) : qDelta, tiles);
            for (int i = 0; i < enc.size(); ++i) { mark(pxs[i], enc[i].fmt == 'j' ? 1 : 0); parts.append(enc[i]); }
        }
        m_prev = cur;
        m_lastRegion = region; m_lastSource = source;
        m_lastEmitMs = QDateTime::currentMSecsSinceEpoch();
        QMetaObject::invokeMethod(this, [this, parts, W, H, key, region, source, captureMs, tiles]() {
            --m_inFlight;
            if (!parts.isEmpty()) emit frameReady(parts, W, H, key, region, source, captureMs);
            if (tiles) m_refineTimer.start();             // экран затихнет — участки с потерями будут досланы чёткими
        }, Qt::QueuedConnection);
    });
    return true;
}

// Доводка в покое: пока на экране ничего не менялось, участки, которые ушли JPEG-ом, досылаются без потерь — небольшими порциями,
// чтобы не занять канал. Любое новое изменение экрана откладывает доводку
void ScreenCaptureSession::refineTick()
{
    if (!m_tiles.load()) return;
    if (m_gate && !m_gate()) { m_refineTimer.start(250); return; }
    if (m_inFlight.load() > 0) { m_refineTimer.start(120); return; }
    ++m_inFlight;
    m_pool.start([this]() {
        const QImage cur = m_prev;
        bool more = false;
        QVector<FramePart> parts;
        const int W = cur.width(), H = cur.height();
        if (!cur.isNull()) {
            const int tx = (W + TILE - 1) / TILE, ty = (H + TILE - 1) / TILE;
            if (m_lossy.size() == tx * ty) {
                int budget = 48;                    // плиток за один раз
                for (int j = 0; j < ty && budget > 0; ++j) {
                    int i = 0;
                    while (i < tx && budget > 0) {
                        if (!m_lossy[j * tx + i]) { ++i; continue; }
                        int e = i;
                        while (e < tx && m_lossy[j * tx + e] && e - i < budget) ++e;
                        const QRect px(i * TILE, j * TILE, qMin(W, e * TILE) - i * TILE, qMin(H, (j + 1) * TILE) - j * TILE);
                        parts.append(encodePart(px, cur.copy(px), 0, true, true));
                        for (int k = i; k < e; ++k) m_lossy[j * tx + k] = 0;
                        budget -= e - i;
                        i = e;
                    }
                }
                more = m_lossy.contains(1);
            }
        }
        const QRectF region = m_lastRegion;
        const QSize source = m_lastSource;
        QMetaObject::invokeMethod(this, [this, parts, W, H, region, source, more]() {
            --m_inFlight;
            if (!parts.isEmpty()) emit frameReady(parts, W, H, false, region, source, QDateTime::currentMSecsSinceEpoch());
            if (more) m_refineTimer.start(40);
        }, Qt::QueuedConnection);
    });
}
