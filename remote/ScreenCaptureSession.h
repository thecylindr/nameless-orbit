#ifndef SCREENCAPTURESESSION_H
#define SCREENCAPTURESESSION_H

#include <QObject>
#include <QScreenCapture>
#include <QMediaCaptureSession>
#include <QVideoSink>
#include <QProcess>
#include <QSocketNotifier>
#include <QScreen>
#include <QVariantList>
#include <QTimer>
#include <QDateTime>
#include <QElapsedTimer>
#include <QRectF>
#include <QSize>
#include <QImage>
#include <QThreadPool>
#include <QVector>
#include <QByteArray>
#include <functional>
#include <atomic>

// Один сжатый прямоугольник обновления: позиция в кадре (в пикселях кадра) и JPEG этого участка
struct FramePart
{
    int x = 0, y = 0, w = 0, h = 0;
    QByteArray jpeg;       // данные участка: JPEG (fmt 'j') или сжатая без потерь плитка (fmt 'z')
    char fmt = 'j';        // 'c' — «скопировать уже имеющееся»: участок (x,y,w,h) берётся из картинки зрителя с позиции (sx,sy); данных нет (прокрутка)
    int sx = 0, sy = 0;
};

// Захват своего экрана для демонстрации другому устройству. На Wayland (Hyprland) QScreenCapture сам показывает
// системный выбор экрана через XDG Desktop Portal (ScreenCast) при каждом запуске — согласие даёт сама ОС,
// в обход этого диалога попасть на экран нельзя; здесь только берём готовые кадры и готовим их для сети.
//
// Передаются не целые картинки на каждый кадр, а ОБНОВЛЕНИЯ — как в VNC и удалённых рабочих столах:
//   • ключевой кадр — весь экран одним JPEG: в начале, при смене размера или области, раз в несколько секунд при изменениях, по запросу зрителя;
//   • остальные кадры — только изменившиеся участки (плитки 64×64 сливаются в прямоугольники), каждый отдельным JPEG высокого качества;
//   • ничего не изменилось — ничего не передаётся.
// На обычной работе (текст, курсор, окна) это в десятки раз меньше данных, чем целый кадр, поэтому частота высокая, а текст — чёткий.
// Если экран меняется целиком (видео, игры), обновление превращается в ключевой кадр, а частоту и качество ограничивает «лесенка» в
// RemoteDesktopController. Сжатие идёт в рабочем потоке, интерфейс не подвисает.
//
// Требует запущенный ScreenCast-бэкенд портала (xdg-desktop-portal + xdg-desktop-portal-hyprland/-wlr и PipeWire);
// если его нет, QScreenCapture сообщит об ошибке через errorOccurred — это отражается в error()/errorString()
class ScreenCaptureSession : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorChanged)
    Q_PROPERTY(int targetFps READ targetFps WRITE setTargetFps NOTIFY targetFpsChanged)
    Q_PROPERTY(int jpegQuality READ jpegQuality WRITE setJpegQuality NOTIFY jpegQualityChanged)
    Q_PROPERTY(int maxWidth READ maxWidth WRITE setMaxWidth NOTIFY maxWidthChanged)

public:
    explicit ScreenCaptureSession(QObject *parent = nullptr);

    bool isActive() const;
    QString errorString() const { return m_errorString; }

    int targetFps() const { return m_targetFps; }
    void setTargetFps(int fps);
    int jpegQuality() const { return m_jpegQuality; }
    void setJpegQuality(int q);
    int maxWidth() const { return m_maxWidth; }
    void setMaxWidth(int w);
    // «можно ли сейчас отправить кадр»: пока сеть не успевает, кадр даже не сжимается — задержка не копится, процессор не греется
    void setGate(std::function<bool()> gate) { m_gate = std::move(gate); }
    // область экрана в долях (0..1), которую нужно слать: увеличенный участок снимается в полном разрешении источника,
    // а не растягивается из общей картинки. Полный экран — QRectF(0, 0, 1, 1)
    void setRegion(const QRectF &r);
    QRectF region() const { return m_region; }
    // какой экран показывать (имя вывода, например eDP-1); пусто — основной. Особое значение «portal» — системный выбор через портал
    void setScreenName(const QString &name) { m_screenName = name; }
    QString screenName() const { return m_screenName; }
    // «Второй экран»: виртуальный монитор нужного размера вместо реального (его нет среди экранов Qt сразу после создания)
    void setVirtualSize(const QSize &size) { m_virtualSize = size; }
    QSize virtualSize() const { return m_virtualSize; }
    // окна, которые сейчас видны на экранах (Hyprland): address, label — для выбора «показать окно»; имя источника — «window:<address>»
    static QVariantList visibleWindows();
    // строка в журнал ~/.cache/<приложение>/orbit-video.log: какой путь захвата выбран и почему (для разбора «почему JPEG»)
    static void log(const QString &line);
    int captureFps() const { return m_capFps; }               // сколько кадров в секунду реально отдаёт сам захват экрана (до всяких ограничений)
    void requestKeyFrame();                                // следующее обновление — целым кадром (в видео-режиме — перезапуск кодера, если ключевой кадр был давно)
    // видео-режим: вместо JPEG-кадров поток H.264 (кодирует x264 в wf-recorder): в разы меньше данных и аппаратное разжатие у зрителя.
    // Включается, только когда зритель умеет такой поток (телефон); без wf-recorder остаётся JPEG
    void setVideoMode(bool on);
    // «Тайлы Orbit» — универсальный формат без аппаратных декодеров: меняются только участки 64×64, текст и интерфейс идут без потерь (zlib),
    // фото и видео внутри кадра — JPEG, а в покое всё лосси-доводится до идеальной чёткости. Работает у любого зрителя
    void setTilesMode(bool on);
    bool tilesMode() const { return m_tiles.load(); }
    void submitExternal(const QImage &image);           // кадр от внешнего источника (Android-служба захвата) → тот же движок плиток
    // кодер видео: auto — видеокарта (VAAPI: Intel/AMD), а если она не отдала кадров — процессор (x264); gpu / cpu — принудительно
    void setEncoderPreference(const QString &pref);
    bool gpuEncoding() const { return m_video && m_gpu; }
    bool videoMode() const { return m_video; }
    bool wantsVideo() const { return m_wantVideo; }
    qint64 encoderAgeMs() const { return m_wfStartMs > 0 ? QDateTime::currentMSecsSinceEpoch() - m_wfStartMs : 0; }   // сколько работает текущий процесс кодера          // человек (зритель) хочет видео — для Android-кодера, который сам отдельный от wf-recorder
    QString videoUnavailableReason() const;                // пусто — видео возможно; иначе почему будет JPEG          // следующее обновление — целым кадром (зритель потерял картинку, вернулся, увеличил)

public slots:
    void start();    // показывает системный выбор экрана (только на Wayland); на X11 захватывает сразу
    void stop();

signals:
    void activeChanged();
    void errorChanged();
    void targetFpsChanged();
    void jpegQualityChanged();
    void maxWidthChanged();
    // готовое обновление: parts — изменившиеся участки (весь кадр, если key), width×height — размер кадра целиком,
    // region — какая часть экрана в кадре, source — размер всего экрана в пикселях (от него считаются пропорции у зрителя)
    // captureMs — время снимка (миллисекунды эпохи): по нему зритель выдерживает ровный темп показа независимо от времени сжатия
    // готовая единица видео (один кадр H.264 в формате Annex B с метками AUD; ключевой — с SPS/PPS): key — можно начинать разжатие с неё
    void sourceLost();                 // показываемое окно закрыто — захватывать больше нечего
    void videoReady(const QByteArray &au, int width, int height, bool key, const QRectF &region, const QSize &source, qint64 captureMs);
    void frameReady(const QVector<FramePart> &parts, int width, int height, bool key, const QRectF &region, const QSize &source, qint64 captureMs);

private slots:
    void onVideoFrameChanged(const QVideoFrame &frame);

private:
    // Быстрый путь для wlroots-композиторов (Hyprland, driftwm): готовые JPEG-кадры из wf-recorder (wlr-screencopy) напрямую,
    // без портала, PipeWire и копий через Qt — Qt-захват давал около 12 к/с, этот путь — сотни. Если wf-recorder нет или он
    // не запустился, работает прежний QScreenCapture
    bool wfAvailable() const;
    void startWf();
    void stopWf(bool keepActive = false);
    void scheduleWfRestart(int delayMs = 1200);       // перезапуск кодера с новыми параметрами (изменения в пределах задержки копятся)
    // то же для подстройки «лесенки» под сеть: не чаще раза в WF_TUNE_GAP_MS — каждый перезапуск кодера это 0,3–0,8 с без кадров,
    // а сеть колеблется постоянно; последние значения применит ближайший разрешённый перезапуск
    void scheduleTuneRestart();
    qint64 m_wfStartedMs = 0;
    void onWfData();
    void onWfFrame(const QByteArray &jpeg);
    void onVideoData();                                      // разбор потока H.264 на кадры
    void emitVideoUnit(const QByteArray &au);
    bool m_wantVideo = false, m_video = false;
    QString m_encPref = QStringLiteral("auto");
    bool m_gpu = false, m_gpuFailed = false;               // процесс кодирует на видеокарте / видеокарта на этой системе не сработала
    bool fallbackFromGpu();                                  // видеокарта не дала кадров → тот же показ на x264               // запрошен видео-режим / процесс запущен в нём
    bool m_dropUntilKey = false;                             // кадры пропущены — до следующего ключевого ничего не отправляется (иначе картинка рассыплется)
    int m_vidW = 0, m_vidH = 0;                              // размер из SPS
    qint64 m_lastKeyUnitMs = 0;
    // общий вход для QScreenCapture и сырого потока: кадр целиком → обновление (участки) в рабочем потоке
    bool acceptNow();                                        // можно ли брать кадр сейчас (частота, очередь, готовность зрителя)
    bool submitImage(const QImage &image, const QSize &source);
    void refineTick();                                       // в покое дослать участки, ушедшие в JPEG, без потерь
    std::atomic<bool> m_tiles{false};
    QTimer m_refineTimer;
    QVector<char> m_lossy;                                   // плитки, чья картинка у зрителя пока с потерями (только рабочий поток)
    QRectF m_lastRegion{0, 0, 1, 1};
    QSize m_lastSource;
    // сырой поток кадров от wf-recorder через именованный канал с большим буфером (обычный канал отдаёт 55 МБ/с — это 6 к/с)
    bool m_raw = false;
    int m_rawFd = -1, m_rawW = 0, m_rawH = 0, m_rawFill = 0;
    QSocketNotifier *m_rawNotifier = nullptr;
    QString m_rawPath;
    QByteArray m_rawFrame;
    QSize m_rawSource;
    void onRawReadable();
    void stopRaw();
    qint64 m_lastEmitMs = 0;                                 // когда ушло последнее обновление: частые подряд — «черновик» с пониженным качеством
    QByteArray m_lastJpeg;                                   // прошлый отправленный JPEG: точные повторы пропускаются
    qint64 m_wfStartMs = 0;
    void startQtCapture();

    QScreen *pickScreen() const;
    bool windowSource() const { return m_screenName.startsWith(QLatin1String("window:")) || m_screenName.startsWith(QLatin1String("dwm:")); }
    bool virtualSource() const { return m_screenName.startsWith(QLatin1String("HEADLESS")); }    // виртуальный монитор «второго экрана» (Hyprland)
    bool currentWindowRect(QRect *r) const;                // где окно-источник сейчас (Hyprland — hyprctl, driftwm — driftwm msg)
    void checkWindow();                    // окно закрыто → sourceLost; сдвинуто → перезапуск захвата на новом месте
    QSize m_virtualSize;
    QRect m_winRect;                       // где окно было при запуске захвата
    QTimer m_winTimer;                     // следит, не сдвинулось ли и не закрылось ли окно
    QString m_screenName;
    QProcess *m_wf = nullptr;
    QByteArray m_wfBuf;
    int m_wfScan = 0;
    bool m_wfActive = false;
    bool m_wfGotFrame = false;
    int m_wfFps = 0, m_wfWidth = 0, m_wfQuality = 0;      // параметры, с которыми запущен процесс
    QRectF m_wfRegion{0, 0, 1, 1};
    QTimer m_wfRestart;

    QScreenCapture m_capture;
    QMediaCaptureSession m_session;
    QVideoSink m_sink;

    std::function<bool()> m_gate;
    QRectF m_region{0, 0, 1, 1};
    // Конвейер: пока рабочий поток сжимает кадр N, поток интерфейса уже снимает кадр N+1 (не больше двух заданий в работе).
    // Один поток-исполнитель гарантирует порядок и целостность m_prev
    std::atomic<int> m_inFlight{0};
    QThreadPool m_pool;
    std::atomic<bool> m_forceKey{true};     // следующее обновление — ключевой кадр
    QImage m_prev;                          // прошлый кадр для сравнения (трогается только рабочим потоком, по одному заданию за раз)
    qint64 m_lastKeyMs = 0;        // кадр сейчас сжимается в рабочем потоке — следующий не берём
    int m_targetFps = 10;
    int m_jpegQuality = 60;
    int m_maxWidth = 1280;
    QElapsedTimer m_frameTimer;
    QElapsedTimer m_capClock;
    int m_capCount = 0, m_capFps = 0;
    QString m_errorString;
};

#endif // SCREENCAPTURESESSION_H
