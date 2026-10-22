#ifndef REMOTEDESKTOPCONTROLLER_H
#define REMOTEDESKTOPCONTROLLER_H

#include "../plugins/PluginBase.h"
#include "ScreenCaptureSession.h"
#include "InputInjector.h"
#include "VideoDecoder.h"
#include "VirtualDisplay.h"
#include <QTimer>
#include "AudioStream.h"
#include "KeyboardGrab.h"
#include <QRectF>
#include <QSize>
#include <QElapsedTimer>
#include <QQuickImageProvider>
#include <QImage>
#include <QMutex>
#include <QPointer>
#include <QVideoSink>
#include <QVideoFrame>
#include <QList>
#include <QVariantMap>
#include <QVector>
#include <QPair>
#include <QPoint>

class SettingsManager;

// Демонстрация экрана и удалённое управление — самая чувствительная часть Orbit, поэтому у неё два независимых
// согласия подряд, ни одно не пропускается в обход другого:
//   1) запрос показывается внутри Orbit («Иван просит демонстрацию экрана» — Показать/Отклонить);
//   2) только после «Показать» запускается QScreenCapture, и дальше ОС (портал XDG Desktop Portal) сама
//      спрашивает, какой именно экран делиться — этот диалог нельзя подменить или пропустить программно.
// Управление курсором и клавиатурой — ОТДЕЛЬНОЕ разрешение поверх уже идущей демонстрации: сама демонстрация
// экрана ввод не даёt. Пока оно включено, наверху постоянно висит полоса с именем того, кто управляет, и
// кнопкой «Отключить»; через SettingsManager::inputSessionTimeoutMin оно гаснет само.
//
// Ограничение первой версии: в любой момент — не больше одной исходящей демонстрации и не больше одной
// просматриваемой чужой; это сознательно простой случай «показать помощь одному человеку», а не пульт
// диспетчерской на несколько экранов сразу.
class RemoteDesktopController : public PluginBase
{
    Q_OBJECT
    Q_PROPERTY(bool featureEnabled READ featureEnabled NOTIFY featureEnabledChanged)
    Q_PROPERTY(QString sharingToDeviceId READ sharingToDeviceId NOTIFY sharingChanged)
    Q_PROPERTY(bool sharingActive READ sharingActive NOTIFY sharingChanged)
    Q_PROPERTY(QVariantMap shareInfo READ shareInfo NOTIFY sharingChanged)                    // кому и что показывается: peer, what, second, waiting (ждём разрешения системы) — для информационной полосы
    Q_PROPERTY(QString viewingDeviceId READ viewingDeviceId NOTIFY viewingChanged)
    Q_PROPERTY(int viewStage READ viewStage NOTIFY viewingChanged)
    Q_PROPERTY(QString viewNote READ viewNote NOTIFY viewingChanged)                           // что сейчас делает показывающий: «создаю монитор», «ждём выбора экрана»…                             // 0 — запрос отправлен, 1 — владелец согласился (ждём начала захвата), кадры идут — смотрите frameSeq
    Q_PROPERTY(int frameSeq READ frameSeq NOTIFY frameUpdated)
    Q_PROPERTY(int frameWidth READ frameWidth NOTIFY frameUpdated)
    Q_PROPERTY(int frameHeight READ frameHeight NOTIFY frameUpdated)
    Q_PROPERTY(QString inputArmedForDeviceId READ inputArmedForDeviceId NOTIFY inputArmedChanged)
    Q_PROPERTY(int inputRemainingSeconds READ inputRemainingSeconds NOTIFY inputArmedChanged)
    Q_PROPERTY(bool inputAvailable READ inputAvailable CONSTANT)
    Q_PROPERTY(int ladderLevel READ ladderLevel NOTIFY statsChanged)         // текущая ступень «лесенки» (1 — лучшая)
    Q_PROPERTY(int ladderSteps READ ladderSteps NOTIFY statsChanged)         // сколько ступеней в текущей лесенке (видео — девять, JPEG — восемь)
    Q_PROPERTY(int ladderFps READ ladderFps NOTIFY statsChanged)             // частота, которую сейчас просит лесенка
    Q_PROPERTY(QVariantList technologies READ technologies NOTIFY statsChanged)   // что сейчас ускоряет показ и включено ли: для окна настроек
    // «Второй экран»: это устройство — монитор другого компьютера (зритель) / другое устройство — монитор этого (хозяин)
    Q_PROPERTY(bool secondScreen READ secondScreen NOTIFY secondScreenChanged)               // зритель: идёт режим «второй экран» (страница просмотра на весь экран, касания = мышь)
    Q_PROPERTY(int secondOrientation READ secondOrientation NOTIFY secondScreenChanged)       // 1 — альбомная, 2 — портретная
    Q_PROPERTY(QString secondBackend READ secondBackend CONSTANT)                              // «Hyprland», «driftwm (бета)» или пусто — что умеет этот компьютер как хозяин
    Q_PROPERTY(QStringList secondMonitors READ secondMonitors NOTIFY secondScreenChanged)     // мониторы этого компьютера, рядом с которыми можно поставить второй экран
    // звук: состояние приёма («» выкл, waiting, on, off:причина) и что умеет этот компьютер как источник
    Q_PROPERTY(QString audioState READ audioState NOTIFY audioStateChanged)
    Q_PROPERTY(QString audioCodecIn READ audioCodecIn NOTIFY audioStateChanged)
    Q_PROPERTY(bool audioSendAvailable READ audioSendAvailable CONSTANT)
    Q_PROPERTY(bool audioOpusAvailable READ audioOpusAvailable CONSTANT)
    Q_PROPERTY(QStringList audioSources READ audioSources NOTIFY audioStateChanged)
    Q_PROPERTY(bool secondBeta READ secondBeta CONSTANT)                                       // режим пока бета (driftwm)
    Q_PROPERTY(QVariantMap viewStats READ viewStats NOTIFY statsChanged)   // для панели статистики у зрителя: кодек, размер, частота, поток, буфер, ступень отправителя, история
    Q_PROPERTY(QString viewFormat READ viewFormat NOTIFY statsChanged)    // что приходит: «H.264 · 1920×1080 · 58 к/с · 3,2 Мбит/с»
    Q_PROPERTY(QString sendFormat READ sendFormat NOTIFY statsChanged)    // что уходит зрителю — в том же виде
    Q_PROPERTY(int viewFps READ viewFps NOTIFY statsChanged)              // сколько кадров в секунду приходит сейчас
    Q_PROPERTY(int sendQuality READ sendQuality NOTIFY statsChanged)      // текущее качество и частота отправки (подстраиваются под сеть)
    Q_PROPERTY(int sendFps READ sendFps NOTIFY statsChanged)
    Q_PROPERTY(bool captureAvailable READ captureAvailable CONSTANT)      // можно ли показывать свой экран с этой системы
    Q_PROPERTY(int controlState READ controlState NOTIFY inputArmedChanged)      // у зрителя: 0 не просили, 1 ждём ответа, 2 разрешено, 3 отказано
    Q_PROPERTY(bool reconnecting READ reconnecting NOTIFY viewingChanged)
    Q_PROPERTY(bool keyboardGrabbed READ keyboardGrabbed NOTIFY keyboardGrabChanged)   // зритель второго экрана: все клавиши уходят на тот компьютер // связь с показывающим устройством оборвалась, поток восстанавливается
    Q_PROPERTY(QObject *videoSink READ videoSinkObject WRITE setVideoSinkObject NOTIFY videoSinkChanged)   // VideoOutput.videoSink страницы просмотра
    Q_PROPERTY(bool videoOnSink READ videoOnSink NOTIFY frameUpdated)     // кадр видео рисует VideoOutput — FrameView прячется
    Q_PROPERTY(QRectF frameRegion READ frameRegion NOTIFY frameUpdated)        // какую часть экрана показывает текущий кадр (в долях)
    Q_PROPERTY(double sourceAspect READ sourceAspect NOTIFY frameUpdated)      // пропорции всего экрана показывающего (ширина / высота)
    Q_PROPERTY(int frameAgeMs READ frameAgeMs NOTIFY statsChanged)       // сколько мс назад пришёл последний кадр (-1 — ещё не было)
    Q_PROPERTY(int captureFps READ captureFps NOTIFY statsChanged)         // сколько кадров в секунду отдаёт сам захват экрана
    Q_PROPERTY(int sendActualFps READ sendActualFps NOTIFY statsChanged)  // сколько обновлений в секунду реально уходит зрителю
    Q_PROPERTY(int peerDecodeMs READ peerDecodeMs NOTIFY statsChanged)      // за сколько мс зритель в среднем разжимает кадр (с учётом параллельности)
    Q_PROPERTY(int sendRtt READ sendRtt NOTIFY statsChanged)              // задержка до зрителя, мс (по подтверждениям кадров)

public:
    RemoteDesktopController(PeerManager *manager, SettingsManager *settings, QQmlEngine *qmlEngine, QObject *parent = nullptr);

    bool featureEnabled() const;
    bool isEnabled() const override { return featureEnabled(); }

    QString sharingToDeviceId() const { return m_sharingTo; }
    QVariantMap shareInfo() const;
    bool sharingActive() const { return !m_sharingTo.isEmpty() && (m_capture.isActive() || m_externalActive); }     // «показ идёт» — только когда есть кому показывать
    QString viewingDeviceId() const { return m_viewing; }
    int viewStage() const { return m_viewStage; }
    QString viewNote() const { return m_viewNote; }
    int frameSeq() const { return m_frameSeq; }
    int frameWidth() const { return m_frameW; }
    int frameHeight() const { return m_frameH; }
    QString inputArmedForDeviceId() const { return m_inputArmedFor; }
    int inputRemainingSeconds() const;
    bool inputAvailable() const { return InputInjector::isAvailable(); }
    int viewFps() const { return m_viewFps; }
    bool secondScreen() const { return m_secondViewing; }
    int secondOrientation() const { return m_secondOrient; }
    QString secondBackend() const { return VirtualDisplay::backendName(); }
    QString audioState() const { return m_audioState; }
    QString audioCodecIn() const { return m_audioCodecIn; }
    bool audioSendAvailable() const { return AudioStream::captureAvailable(); }
    bool audioOpusAvailable() const { return AudioStream::opusAvailable(); }
    QStringList audioSources() const { return AudioStream::sources(); }
    QStringList secondMonitors() const { return VirtualDisplay::monitorNames(); }
    bool secondBeta() const { return VirtualDisplay::backend() == VirtualDisplay::Backend::Driftwm; }
    int ladderLevel() const { return m_level + 1; }
    int ladderSteps() const { return ladderSize(); }
    int ladderFps() const { return m_capture.targetFps(); }
    QVariantList technologies() const;
    QVariantMap viewStats() const;
    Q_INVOKABLE QVariantList availableFormats() const;     // для настроек: как это устройство отдаёт свой экран — Авто / H.264 / Тайлы / JPEG и что из этого доступно
    QString viewFormat() const { return m_viewing.isEmpty() || m_rxCodec.isEmpty() ? QString() : formatLine(m_rxCodec, m_frameW, m_frameH, m_viewFps, m_rxKbit); }
    QString sendFormat() const { return m_sharingTo.isEmpty() || m_txCodec.isEmpty() ? QString() : formatLine(m_txCodec, m_lastSentW, m_lastSentH, m_sentFps, m_txKbit) + (m_txCodec == QLatin1String("JPEG") && !m_videoWhy.isEmpty() ? QStringLiteral(" (") + m_videoWhy + QLatin1Char(')') : QString()); }
    int sendQuality() const { return m_capture.jpegQuality(); }
    int sendFps() const { return m_capture.targetFps(); }
    bool captureAvailable() const;
    bool reconnecting() const { return m_reconnecting; }
    int controlState() const { return m_controlState; }
    int sendRtt() const { return int(m_rttMs); }
    int peerDecodeMs() const { return int(m_peerDecodeMs); }
    int sendActualFps() const { return m_sentFps; }
    int captureFps() const { return m_capture.captureFps(); }
    // снимок собранной картинки для отрисовки (FrameView, поток рендера): сама картинка, какую часть экрана она занимает и номер версии
    bool canvasSnapshot(QImage *image, QRectF *region, quint64 *serial);
    int frameAgeMs() const;
    QRectF frameRegion() const { return m_inRegion; }
    QObject *videoSinkObject() const { return m_videoSink.data(); }
    void setVideoSinkObject(QObject *o) { QVideoSink *s = qobject_cast<QVideoSink *>(o); if (s != m_videoSink) { m_videoSink = s; if (!s) m_videoOnSink = false; emit videoSinkChanged(); } }
    bool videoOnSink() const { return m_videoOnSink; }
    double sourceAspect() const { return m_inSource.height() > 0 ? double(m_inSource.width()) / m_inSource.height() : 0.0; }

    void handleMessage(const QString &deviceId, const QJsonObject &payload) override;
    Q_INVOKABLE void setAudio(bool on, const QString &codecPref = QString());      // зритель: включить или выключить звук показывающего устройства
    // зритель второго экрана: захватить клавиатуру целиком (сочетания системы тоже уходят туда) или вернуть; Scroll Lock — вернуть самому
    Q_INVOKABLE void setKeyboardGrab(bool on);
    bool keyboardGrabbed() const { return m_kbGrab.active(); }
    bool needsPermission(const QJsonObject &p) const override
    {
        const QString k = p.value(QStringLiteral("kind")).toString();
        return k == QLatin1String("shareRequest") || k == QLatin1String("shareOffer") || k == QLatin1String("controlRequest") || k == QLatin1String("input")
               || k == QLatin1String("audioRequest") || k == QLatin1String("secondScreenRequest") || k == QLatin1String("secondScreenOffer");       // второй экран — это и показ экрана, и управление: без разрешения «Демонстрация экрана» не принимается
    }
    QQuickImageProvider *createImageProvider();     // для engine.addImageProvider("orbit", ...)

public slots:
    // кадры и состояние от службы захвата экрана телефона (Android); вызываются через очередь из потока Java
    void onExternalVideo(const QByteArray &au, int w, int h, bool key);     // Android: кадр H.264 от кодера телефона — зрителю
    void onExternalVideoFailed(const QString &reason);                       // кодер телефона не заработал: показ идёт тайлами
    void onExternalFrame(const QByteArray &jpeg, int width, int height) { onFrameReady(jpeg, width, height); }     // у телефона кадр всегда целый
    void onExternalImage(const QImage &img) { m_capture.submitExternal(img); }      // Android, «Тайлы Orbit»: сырой кадр экрана телефона в общий движок плиток
    void onExternalState(int state);          // 1 — показ начался, 2 — остановлен системой, 3 — человек не разрешил

    // просмотр чужого экрана
    void requestShare(const QString &deviceId);
    void offerShare(const QString &deviceId, const QString &source = QString());                   // самому предложить показать свой экран тому устройству (оно должно согласиться)                 // попросить ТО устройство начать демонстрацию
    void stopViewing();
    // второй экран: зритель просит; хозяин отвечает (из диалога). spec: orientation (auto|landscape|portrait), res (native|1080|720), fps, side (left|right|top|bottom)
    // хозяин сам предлагает устройству стать вторым экраном: то на своей стороне подтверждает (или доверяет) и присылает обычный запрос, который хозяин
    // принимает уже без вопросов. windowChoice — окно driftwm (бета), для Hyprland не нужно
    void offerSecondScreen(const QString &deviceId, const QVariantMap &spec, const QString &windowChoice = QString());
    void acceptSecondScreenOffer(const QString &deviceId);
    void declineSecondScreenOffer(const QString &deviceId);
    Q_INVOKABLE QVariantList driftwmWindows() const { return VirtualDisplay::driftwmWindows(); }       // окна driftwm для выбора (бета)
    void requestSecondScreen(const QString &deviceId, const QVariantMap &spec);
    void acceptSecondScreen(const QString &deviceId, const QString &windowChoice = QString());     // windowChoice — номер окна driftwm или имя монитора Hyprland, рядом с которым ставится новый
    void declineSecondScreen(const QString &deviceId, const QString &reason = QString());
    void enterPip();
    void onVideoFrame(const QVideoFrame &frame, qint64 ptsUs);   // Android: готовый YUV-кадр от аппаратного декодера (из потока Java — через очередь)
    void onVideoFailed(const QString &reason);                     // декодер не запустился или завис: просим отправителя вернуться к JPEG
    void setFullscreenView(bool on, int orientation);           // Android: полный экран и ориентация (0 по системе, 1 альбом, 2 портрет, 3 по датчику)
    void setViewRegion(double x, double y, double w, double h);   // зритель увеличил картинку: прислать только этот участок экрана в полном разрешении
    void setViewerWidth(int pixels);                            // просмотр сообщает показывающему, сколько пикселей в ширину у его окна (поворот экрана, картинка в картинке)                                            // Android: свернуть просмотр в маленькое окно поверх других приложений

    // демонстрация своего экрана — вызывается ПОСЛЕ того, как пользователь принял запрос в UI
    void acceptShareRequest(const QString &deviceId, const QString &screenName = QString());   // screenName: какой экран показать; «portal» — выбор системным окном
    QVariantList availableScreens() const;       // экраны для выбора: name, label, primary; последним — «portal» (системное окно), если захват не на Android
    void declineShareRequest(const QString &deviceId);
    void stopSharing();

    // управление: разрешить/забрать прямо сейчас, независимо от того, кто и когда попросит
    void armInput(const QString &deviceId, bool allow);
    void requestControl();      // с просматривающей стороны: попросить владельца экрана разрешить управление
    void extendInputSession();  // сбросить таймер бездействия (вызывается при получении реального input-сообщения)

    // отправка ввода на то устройство, чей экран сейчас просматривается (действует, только если оно разрешило)
    void sendMove(double dx, double dy);
    void sendMoveAbsolute(double nx, double ny);
    void sendButton(const QString &button, bool down);
    void sendClick(const QString &button);
    void sendScroll(int dy, int dx);
    void sendKey(const QString &key, const QStringList &modifiers);
    void sendType(const QString &text);

signals:
    void featureEnabledChanged();
    void audioStateChanged();
    void sharingChanged();
    void viewingChanged();
    void frameUpdated();
    void inputArmedChanged();
    // входящий запрос на демонстрацию — ПЕРВОЕ согласие (второе, системное, идёт дальше само при acceptShareRequest)
    void shareRequested(const QString &deviceId, const QString &name);
    void sourceChoiceNeeded(const QString &deviceId, const QString &name);  // человек решил показать экран: сначала нужно выбрать, что именно показывать
    void secondScreenRequested(const QString &deviceId, const QString &name, const QString &description, const QVariantList &windows);   // хозяину: устройство просит стать вторым экраном
    void secondScreenOffered(const QString &deviceId, const QString &name, const QString &description);   // зрителю: компьютер предлагает стать его вторым экраном
    void secondScreenChanged();
    void shareOffered(const QString &deviceId, const QString &name);       // устройство хочет показать свой экран мне — нужен ответ человека
    void statsChanged();
    void shareDeclinedByPeer(const QString &deviceId);
    void controlRequested(const QString &deviceId, const QString &name);   // «Иван просит управление» на стороне показывающего
    void errorOccurred(const QString &message);
    void videoSinkChanged();
    void keyboardGrabChanged();
    void keyboardReleasedByUser();       // Scroll Lock: захват снят человеком — страница не захватывает снова, пока окно не активируют заново

private slots:
    void onFrameReady(const QByteArray &jpeg, int width, int height);
    void onCaptureError();

private:
    void sendToPeer(const QString &deviceId, const QJsonObject &payload);
    void sendStatus(const QString &deviceId, const QString &text);    // зрителю: ход запуска показа
    void endViewing();                                                // просмотр закрыт другой стороной (без ответа ей)
    void stopInputSession();
    void sendFrame(const QVector<FramePart> &parts, int width, int height, bool key);     // обновление экрана зрителю
    // ---- приём обновлений: разжатие и сборка картинки идут в рабочем потоке, интерфейс получает только готовый результат ----
    struct Update {
        int w = 0, h = 0; bool key = false;
        QVector<FramePart> parts;                 // сжатые участки (как пришли)
        QVector<QPair<QPoint, QImage>> patches;   // они же разжатые — готовы к наложению
        QVector<QPair<QRect, QPoint>> copies;     // «сдвинуть то, что уже есть»: куда (прямоугольник) и откуда (левый верхний угол источника) — прокрутка; применяются до участков
        QRectF region{0, 0, 1, 1}; QSize source;
        qint64 t = 0;                             // время снимка у показывающего (его часы)
        qint64 due = 0;                           // когда показать (часы зрителя); 0 — сразу
        double seq = -1;                          // номер кадра у отправителя: подтверждается после разжатия (или отбрасывания)
    };
    void presentTick();
    void applyUpdate(const Update &u);
    void enqueueUpdate(const Update &u);
    void decodeLoop();
    void sendAck(double seq, int decodeMs);
    void sendVideo(const QByteArray &au, int width, int height, bool key);   // кадр H.264 вместо JPEG-обновления
    bool m_viewerTiles = false;           // зритель умеет «Тайлы Orbit»
    void chooseFormat();
    void beginViewing(const QString &deviceId);       // общее начало просмотра (обычного и «второго экрана»)
    void releaseSecondScreen();                       // убрать виртуальный монитор / вернуть ввод на основной экран
    struct SecondSpec { int w = 1920, h = 1080, fps = 60; double scale = 1.0; bool scaleAuto = true; QString side = QStringLiteral("right"); QString mode = QStringLiteral("extend"); QString workspace; };      // mode: extend — новый монитор, mirror — повтор существующего
    QHash<QString, QString> m_ssOffered;              // хозяин: кому предложен второй экран (и какое окно driftwm выбрано) — запрос от него принимается без вопроса
    QHash<QString, QVariantMap> m_ssOfferSpec;        // зритель: что предложил компьютер (ориентация, разрешение, частота, сторона)
    QHash<QString, SecondSpec> m_ssPending;           // запросы второго экрана, ждущие ответа хозяина
    int m_viewStage = 0;
    int m_viewGen = 0;
    int m_shareGen = 0;                               // номер запуска показа: сторож первого кадра относится только к своему
    QString m_viewNote;                               // зритель: последнее сообщение о ходе запуска от показывающего
    AudioStream m_audioOut;                           // показывающий: звук этого компьютера — в сеть
    AudioStream m_audioIn;                            // зритель: звук показывающего — в динамики
    QString m_audioState, m_audioCodecIn;             // состояние приёма звука
    bool m_audioWant = false;
    KeyboardGrab m_kbGrab;                            // зритель второго экрана: клавиатура целиком
    double m_hostVolume = 1.0;                        // зритель: громкость выхода показывающего (доля, шкала системы) — звук повторяет её
    bool m_hostMuted = false;
    QString m_audioOverride = QStringLiteral("auto"), m_audioCodecPref;   // выбор из окна второго экрана: auto | on | off и кодек
    void applyAudioSettings();
    QString who(const QString &deviceId) const;      // имя устройства для журнала
    void stopAudioIn(const QString &state = QString());
    bool m_ssMirror = false;                          // хозяин: второй экран повторяет существующий монитор, а не создаёт новый
    QString m_shareSource;                            // что показывается сейчас: имя экрана, «window:…», пусто — основной
    QTimer m_ssKeep;                                  // следит, чтобы на виртуальном мониторе оставался только выбранный стол
    QString m_ssWorkspace;
    QString m_ssOutput;                               // хозяин: имя виртуального монитора (Hyprland)
    int m_ssFps = 0, m_ssWidth = 0;                   // хозяин: пока идёт второй экран — его частота и ширина кадра вместо общих настроек
    bool m_secondViewing = false;                     // зритель: идёт второй экран
    int m_secondOrient = 1;
    bool feedDecoder(const QByteArray &au, int w, int h, bool key, qint64 ptsUs);    // видеокадр в декодер: MediaCodec на телефоне, libavcodec на ПК
    VideoDecoder *m_decoder = nullptr;
    bool m_viewerH264 = false;            // зритель умеет разжимать H.264 (телефон)
    QString m_offeredSource;              // что человек выбрал показывать, когда сам предложил показ
    int m_videoFails = 0;
    bool m_extDropUntilKey = false;       // кадры кодера телефона пропущены — до следующего ключевого ничего не уходит
    bool m_extVideo = false;              // сейчас идёт видео от кодера телефона (подпись формата)
    int m_extBitrateKbps = 6000;          // скорость кодера телефона по ступени лесенки
    QString m_videoWhy;                   // почему идёт JPEG, а не видео — показывается на экране отправителя
    // выравнивание показа видео: кадр выводится в «свой» момент (время съёмки + дорога + короткая задержка по разбросу прихода), а не по готовности
    struct VideoFrame { QVideoFrame frame; qint64 due = 0; QRectF region; QSize source; };
    QPointer<QVideoSink> m_videoSink;        // приёмник VideoOutput страницы просмотра: YUV-кадр рисует видеочип
    bool m_videoOnSink = false;              // картинка сейчас идёт через VideoOutput (а не через FrameView)
    QHash<qint64, VideoFrame> m_videoMeta;      // по номеру кадра: когда показать и какая область (картинка ещё разжимается)
    QList<VideoFrame> m_videoQueue;             // разжатые, ждут своего момента
    QTimer m_videoTimer;
    void presentVideo();
    void showVideoFrame(const VideoFrame &f);
    qint64 m_lastNeedKeyMs = 0;   // подтверждение кадра отправителю; decodeMs — сколько заняло разжатие у зрителя (0 — не разжимался)
    void clearCanvas();
    double m_peerDecodeMs = 0;            // сколько в среднем зритель разжимает один кадр (приходит в подтверждениях): частота не выше его возможностей
    bool canSendFrame();                  // можно ли сейчас готовить и слать кадр (не больше трёх неподтверждённых, очередь сокета не растёт)
    void applyLevel();                    // качество, частота и размер кадра по текущей ступени «лесенки» под сеть и экран зрителя
    void updateBackgroundHold();          // Android: не дать системе усыпить процесс, пока идёт просмотр или показ
    void resumeViewing();                 // после обрыва связи снова попросить показ у того же устройства

    SettingsManager *m_settings;
    ScreenCaptureSession m_capture;
    InputInjector m_input;

    QString m_sharingTo;          // кому я сейчас показываю экран
    bool m_externalActive = false;      // идёт захват через Java-службу (Android)
    QTimer m_paramsTimer;               // раз в 100 мс отдаёт Java-стороне качество, частоту и «можно ли слать»
    QString m_offeredTo;          // кому предложил показать — его ответный запрос принимается без второго вопроса
    QTimer m_statsTimer;          // раз в секунду: частота кадров, подстройка качества
    int m_viewFramesInSecond = 0, m_viewFps = 0;
    int m_shownInSecond = 0, m_shownFps = 0;   // сколько кадров реально показано (после выравнивания и пропусков)
    QList<int> m_fpsHistory;                  // частота прихода за последние 40 секунд — для графика
    int m_senderLevel = 0, m_senderSteps = 0, m_senderCapFps = 0;   // что сообщает отправитель в каждом кадре
    int m_videoPlayMs = 0;                    // задержка выравнивания видео
    QString m_rxCodec, m_txCodec;                 // «H.264» или «JPEG» — формат последнего принятого / отправленного кадра
    qint64 m_rxBytes = 0, m_txBytes = 0;          // за текущую секунду
    int m_rxKbit = 0, m_txKbit = 0;               // кбит/с за прошлую секунду
    static QString formatLine(const QString &codec, int w, int h, int fps, int kbit);
    int m_sentInSecond = 0, m_sentFps = 0;
    qint64 m_outCaptureMs = 0;
    int m_dropsInPeriod = 0, m_calmSeconds = 0;

    // --- отправка кадров: счётчик и подтверждения зрителя дают и защиту от очереди, и измерение задержки ---
    quint32 m_sendSeq = 0, m_ackedSeq = 0;
    qint64 m_lastAckMs = 0;
    QHash<quint32, qint64> m_sentAt;
    double m_rttMs = 0;
    double m_rttBase = 0;                 // наименьшая задержка в спокойное время: от неё считаются пороги «медленно» и «быстро»
    int m_climbPenalty[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};   // сколько лишних секунд спокойствия нужно для подъёма на ступень (растёт, если подъём сразу сорвался)
    int m_cpuLagSeconds = 0;              // сколько секунд подряд кодер на процессоре не успевает
    int m_encCeilFps = 0;                 // сколько кадров в секунду кодер реально выдал, когда не успевал: потолок частоты на время m_encCeilUntil
    qint64 m_encCeilUntil = 0;
    qint64 m_levelSince = 0;              // когда ступень изменилась в последний раз
    int ladderSize() const;               // ступеней в лесенке: у видео девять с мелкими шагами частоты, у JPEG восемь
    bool videoLadder() const;             // идёт (или будет идти) видео H.264
    int m_level = 2;                      // ступень в таблице качества (0 — лучшее)
    int m_goodSeconds = 0, m_badSeconds = 0;
    bool m_queueBlocked = false;          // за последнюю секунду очередь сокета упиралась в предел
    QByteArray m_lastSentJpeg;            // последний отправленный кадр: когда экран не меняется, он повторяется раз в пару секунд (признак «жив»)
    int m_lastSentW = 0, m_lastSentH = 0;
    QRectF m_lastSentReg{0, 0, 1, 1};
    QSize m_lastSentSrc;
    qint64 m_lastSentMs = 0;
    qint64 m_lastFrameRxMs = 0;           // когда пришёл последний кадр (на стороне зрителя)
    int m_viewerWidthSent = 0;
    QRectF m_inRegion{0, 0, 1, 1};        // что содержит принятый кадр (зритель)
    QSize m_inSource;                     // размер всего экрана показывающего (зритель)
    QRectF m_outRegion{0, 0, 1, 1};       // что содержит отправляемый кадр (показывающий)
    QSize m_outSource;
    QRectF m_lastRegionSent{0, 0, 1, 1};
    int m_viewerWidth = 0;                // сколько пикселей в ширину у экрана зрителя: больше слать нет смысла
    QString m_graceFor;                   // зритель пропал: показ ждёт его возвращения (экран не перезапрашивается заново)
    QTimer m_graceTimer;

    // --- просмотр: восстановление после обрыва ---
    bool m_reconnecting = false;
    int m_controlState = 0;
    QTimer m_reconnectTimeout;
    QString m_viewing;            // чей экран сейчас смотрю я
    QString m_inputArmedFor;      // кому я сейчас разрешил управлять мной
    QElapsedTimer m_inputArmedAt;
    QTimer m_inputTimeoutTimer;

    QImage m_canvas;              // собранная картинка экрана показывающего (под m_frameMutex): ключевой кадр + накладываемые обновления
    QImage m_lastFrame;
    QList<Update> m_decodeQueue;  // принятые, ещё не разжатые обновления (под m_decodeMutex)
    QList<Update> m_ready;        // разжатые, ждут своего часа показа (под m_decodeMutex)
    QMutex m_decodeMutex;
    int m_decoders = 0;           // сколько рабочих потоков сейчас разжимает (под m_decodeMutex); целые кадры независимы и разжимаются параллельно
    double m_lastKeySeq = -1;     // номер последнего целого кадра, попавшего в m_ready: всё более старое показывать уже не нужно
    double m_lastEnqSeq = -1;
    QTimer m_presentTimer;        // выдерживает ровный темп: обновления накладываются не по приходу, а по времени снимка плюс небольшая задержка
    double m_minTransit = 1e12;   // наименьшая «дорога» кадра (приход − снимок) — разница часов и сети
    double m_jitter = 10;         // разброс дороги: по нему подбирается задержка показа
    qint64 m_playoutMs = 60;
    quint64 m_canvasSerial = 0;
    QRectF m_canvasRegion{0, 0, 1, 1};
    double m_avgMsgBytes = 80000; // средний размер сообщения с кадром: от него зависит допустимая очередь сокета
    QMutex m_frameMutex;
    int m_frameSeq = 0;
    int m_frameW = 0, m_frameH = 0;

    class ImageProvider : public QQuickImageProvider
    {
    public:
        ImageProvider(RemoteDesktopController *owner) : QQuickImageProvider(QQuickImageProvider::Image), m_owner(owner) {}
        QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
    private:
        RemoteDesktopController *m_owner;
    };
};

#endif // REMOTEDESKTOPCONTROLLER_H
