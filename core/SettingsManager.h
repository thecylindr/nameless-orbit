#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <QObject>
#include <QSettings>

// Общие настройки приложения (не относящиеся к конкретному устройству — те в Identity, и не к списку
// доверенных — тот в TrustStore). Простые флаги, каждый — отдельное свойство QSettings, как в SettingsManager
// drones, но без профилей/ролей — Orbit не многопользовательский.
class SettingsManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool discoveryOnStart READ discoveryOnStart WRITE setDiscoveryOnStart NOTIFY discoveryOnStartChanged)
    Q_PROPERTY(bool startMinimized READ startMinimized WRITE setStartMinimized NOTIFY startMinimizedChanged)
    // работа в фоне: на компьютере окно можно закрыть, Orbit остаётся (NameLess UI видит устройства и подключает их);
    // на Android — служба переднего плана с постоянным уведомлением, чтобы система не усыпляла связь
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)     // ru | en; пусто — человек ещё не выбирал (при первом запуске показывается выбор)
    Q_PROPERTY(bool keepRunning READ keepRunning WRITE setKeepRunning NOTIFY keepRunningChanged)
    // показ экрана: потолки частоты, качества и размера кадра. Авто — «лесенка» сама подстраивается под сеть, не превышая потолков;
    // не авто — значения держатся неизменными (если сеть не справляется, кадры пропускаются, а не ухудшаются)
    Q_PROPERTY(bool streamAuto READ streamAuto WRITE setStreamAuto NOTIFY streamChanged)
    Q_PROPERTY(int streamFps READ streamFps WRITE setStreamFps NOTIFY streamChanged)
    Q_PROPERTY(int streamQuality READ streamQuality WRITE setStreamQuality NOTIFY streamChanged)
    Q_PROPERTY(int streamWidth READ streamWidth WRITE setStreamWidth NOTIFY streamChanged)
    Q_PROPERTY(bool sendReplies READ sendReplies WRITE setSendReplies NOTIFY repliesChanged)         // отправлять на устройства сообщение об ответе Claude («закончил», «ждёт ответа»)
    Q_PROPERTY(bool sendCodexReplies READ sendCodexReplies WRITE setSendCodexReplies NOTIFY repliesChanged)
    Q_PROPERTY(bool receiveReplies READ receiveReplies WRITE setReceiveReplies NOTIFY repliesChanged)   // показывать такие сообщения, пришедшие от других устройств
    Q_PROPERTY(QString streamFormat READ streamFormat WRITE setStreamFormat NOTIFY streamChanged)   // что хочет получать этот зритель: auto, h264, tiles, jpeg
    Q_PROPERTY(QString secondAnchor READ secondAnchor WRITE setSecondAnchor NOTIFY secondChanged)    // у какого монитора ставится второй экран: пусто — у того, где курсор/фокус, иначе имя (eDP-1)
    Q_PROPERTY(QString secondWorkspace READ secondWorkspace WRITE setSecondWorkspace NOTIFY secondChanged)   // рабочий стол Hyprland на втором экране: пусто — отдельный «orbit» (не мешает переключению 1–10), иначе номер 1–10
    Q_PROPERTY(int secondGraceSec READ secondGraceSec WRITE setSecondGraceSec NOTIFY secondChanged)       // сколько секунд ждать пропавшее устройство, потом второй экран освобождается и окна возвращаются на основной монитор
    Q_PROPERTY(QString secondScale READ secondScale WRITE setSecondScale NOTIFY secondChanged)       // масштаб второго экрана по умолчанию: auto или число (1, 1.25, 1.5, 2)
    // звук между устройствами (см. remote/AudioStream.h)
    Q_PROPERTY(int audioVolume READ audioVolume WRITE setAudioVolume NOTIFY audioChanged)                 // громкость принимаемого звука, 0..100
    Q_PROPERTY(int audioLatencyMs READ audioLatencyMs WRITE setAudioLatencyMs NOTIFY audioChanged)        // буфер приёма: меньше — быстрее, больше — без треска
    Q_PROPERTY(bool audioAutoSecond READ audioAutoSecond WRITE setAudioAutoSecond NOTIFY audioChanged)    // со вторым экраном звук компьютера включается сам
    Q_PROPERTY(bool audioAutoShare READ audioAutoShare WRITE setAudioAutoShare NOTIFY audioChanged)       // при просмотре чужого экрана звук включается сам
    Q_PROPERTY(QString audioSource READ audioSource WRITE setAudioSource NOTIFY audioChanged)             // что отдавать: пусто — всё, что играет на этом компьютере, иначе имя источника pactl
    Q_PROPERTY(QString audioCodec READ audioCodec WRITE setAudioCodec NOTIFY audioChanged)                // auto | pcm | opus
    Q_PROPERTY(int audioOpusKbps READ audioOpusKbps WRITE setAudioOpusKbps NOTIFY audioChanged)           // скорость Opus, кбит/с
    Q_PROPERTY(QString streamEncoder READ streamEncoder WRITE setStreamEncoder NOTIFY streamChanged)   // auto — видеокарта, если есть, иначе процессор; gpu; cpu       // 0 — по размеру окна зрителя
    // главный выключатель для удалённого экрана и ввода — отдельно от разрешений по устройствам в TrustStore;
    // выключен по умолчанию, пока пользователь сам не откроет «Удалённый доступ» в настройках
    Q_PROPERTY(bool remoteDesktopFeatureEnabled READ remoteDesktopFeatureEnabled WRITE setRemoteDesktopFeatureEnabled NOTIFY remoteDesktopFeatureEnabledChanged)
    Q_PROPERTY(int inputSessionTimeoutMin READ inputSessionTimeoutMin WRITE setInputSessionTimeoutMin NOTIFY inputSessionTimeoutMinChanged)

public:
    explicit SettingsManager(QObject *parent = nullptr);

    // уведомления системы (Android): включены ли, запросить разрешение, открыть системные настройки
    Q_INVOKABLE bool notificationsAllowed() const;
    Q_INVOKABLE void requestNotificationPermission() const;
    Q_INVOKABLE void openNotificationSettings() const;
    Q_INVOKABLE void testNotification() const;
    Q_INVOKABLE void openBatterySettings() const;      // Android: окно «не ограничивать батарею» для Orbit      // показать пробное уведомление в шторке этого устройства

    bool discoveryOnStart() const { return m_settings.value(QStringLiteral("discoveryOnStart"), true).toBool(); }
    void setDiscoveryOnStart(bool v) { m_settings.setValue(QStringLiteral("discoveryOnStart"), v); emit discoveryOnStartChanged(); }

    QString language() const { const QString v = m_settings.value(QStringLiteral("language")).toString(); return v == QLatin1String("ru") || v == QLatin1String("en") ? v : QString(); }
    void setLanguage(const QString &v) { m_settings.setValue(QStringLiteral("language"), v == QLatin1String("en") ? QStringLiteral("en") : QStringLiteral("ru")); emit languageChanged(); }
    bool keepRunning() const { return m_settings.value(QStringLiteral("keepRunning"), true).toBool(); }
    void setKeepRunning(bool v) { m_settings.setValue(QStringLiteral("keepRunning"), v); emit keepRunningChanged(); }

    bool streamAuto() const { return m_settings.value(QStringLiteral("streamAuto"), true).toBool(); }
    bool sendReplies() const { return m_settings.value(QStringLiteral("sendReplies"), true).toBool(); }
    void setSendReplies(bool v) { m_settings.setValue(QStringLiteral("sendReplies"), v); emit repliesChanged(); }
    Q_INVOKABLE QString configureCodexNotifications();
    bool sendCodexReplies() const { return m_settings.value(QStringLiteral("sendCodexReplies"), false).toBool(); }
    void setSendCodexReplies(bool v) { m_settings.setValue(QStringLiteral("sendCodexReplies"), v); emit repliesChanged(); }
    bool receiveReplies() const { return m_settings.value(QStringLiteral("receiveReplies"), true).toBool(); }
    void setReceiveReplies(bool v) { m_settings.setValue(QStringLiteral("receiveReplies"), v); emit repliesChanged(); }
    QString streamFormat() const { const QString v = m_settings.value(QStringLiteral("streamFormat"), QStringLiteral("auto")).toString(); return v == QLatin1String("h264") || v == QLatin1String("tiles") || v == QLatin1String("jpeg") ? v : QStringLiteral("auto"); }
    void setStreamFormat(const QString &v) { m_settings.setValue(QStringLiteral("streamFormat"), v); emit streamChanged(); }
    QString secondAnchor() const { return m_settings.value(QStringLiteral("secondAnchor")).toString(); }
    void setSecondAnchor(const QString &v) { m_settings.setValue(QStringLiteral("secondAnchor"), v); emit secondChanged(); }
    QString secondWorkspace() const { return m_settings.value(QStringLiteral("secondWorkspace")).toString(); }
    void setSecondWorkspace(const QString &v) { m_settings.setValue(QStringLiteral("secondWorkspace"), v); emit secondChanged(); }
    int secondGraceSec() const { return qBound(3, m_settings.value(QStringLiteral("secondGraceSec"), 10).toInt(), 120); }
    void setSecondGraceSec(int v) { m_settings.setValue(QStringLiteral("secondGraceSec"), qBound(3, v, 120)); emit secondChanged(); }
    QString secondScale() const { return m_settings.value(QStringLiteral("secondScale"), QStringLiteral("auto")).toString(); }
    void setSecondScale(const QString &v) { m_settings.setValue(QStringLiteral("secondScale"), v); emit secondChanged(); }
    int audioVolume() const { return qBound(0, m_settings.value(QStringLiteral("audioVolume"), 80).toInt(), 100); }
    void setAudioVolume(int v) { m_settings.setValue(QStringLiteral("audioVolume"), qBound(0, v, 100)); emit audioChanged(); }
    int audioLatencyMs() const { return qBound(40, m_settings.value(QStringLiteral("audioLatencyMs"), 120).toInt(), 600); }
    void setAudioLatencyMs(int v) { m_settings.setValue(QStringLiteral("audioLatencyMs"), qBound(40, v, 600)); emit audioChanged(); }
    bool audioAutoSecond() const { return m_settings.value(QStringLiteral("audioAutoSecond"), true).toBool(); }
    void setAudioAutoSecond(bool v) { m_settings.setValue(QStringLiteral("audioAutoSecond"), v); emit audioChanged(); }
    bool audioAutoShare() const { return m_settings.value(QStringLiteral("audioAutoShare"), false).toBool(); }
    void setAudioAutoShare(bool v) { m_settings.setValue(QStringLiteral("audioAutoShare"), v); emit audioChanged(); }
    QString audioSource() const { return m_settings.value(QStringLiteral("audioSource")).toString(); }
    void setAudioSource(const QString &v) { m_settings.setValue(QStringLiteral("audioSource"), v); emit audioChanged(); }
    QString audioCodec() const { const QString v = m_settings.value(QStringLiteral("audioCodec"), QStringLiteral("auto")).toString(); return v == QLatin1String("pcm") || v == QLatin1String("opus") ? v : QStringLiteral("auto"); }
    void setAudioCodec(const QString &v) { m_settings.setValue(QStringLiteral("audioCodec"), v); emit audioChanged(); }
    int audioOpusKbps() const { return qBound(16, m_settings.value(QStringLiteral("audioOpusKbps"), 64).toInt(), 256); }
    void setAudioOpusKbps(int v) { m_settings.setValue(QStringLiteral("audioOpusKbps"), qBound(16, v, 256)); emit audioChanged(); }
    QString streamEncoder() const { const QString v = m_settings.value(QStringLiteral("streamEncoder"), QStringLiteral("auto")).toString(); return v == QLatin1String("gpu") || v == QLatin1String("cpu") ? v : QStringLiteral("auto"); }
    void setStreamEncoder(const QString &v) { m_settings.setValue(QStringLiteral("streamEncoder"), v); emit streamChanged(); }
    void setStreamAuto(bool v) { m_settings.setValue(QStringLiteral("streamAuto"), v); emit streamChanged(); }
    int streamFps() const { return qBound(5, m_settings.value(QStringLiteral("streamFps"), 60).toInt(), 60); }
    void setStreamFps(int v) { m_settings.setValue(QStringLiteral("streamFps"), qBound(5, v, 60)); emit streamChanged(); }
    int streamQuality() const { return qBound(30, m_settings.value(QStringLiteral("streamQuality"), 75).toInt(), 95); }
    void setStreamQuality(int v) { m_settings.setValue(QStringLiteral("streamQuality"), qBound(30, v, 95)); emit streamChanged(); }
    int streamWidth() const { return m_settings.value(QStringLiteral("streamWidth"), 0).toInt(); }
    void setStreamWidth(int v) { m_settings.setValue(QStringLiteral("streamWidth"), v <= 0 ? 0 : qBound(480, v, 3840)); emit streamChanged(); }

    bool startMinimized() const { return m_settings.value(QStringLiteral("startMinimized"), false).toBool(); }
    void setStartMinimized(bool v) { m_settings.setValue(QStringLiteral("startMinimized"), v); emit startMinimizedChanged(); }

    bool remoteDesktopFeatureEnabled() const { return m_settings.value(QStringLiteral("remoteDesktopFeatureEnabled"), false).toBool(); }
    void setRemoteDesktopFeatureEnabled(bool v) { m_settings.setValue(QStringLiteral("remoteDesktopFeatureEnabled"), v); emit remoteDesktopFeatureEnabledChanged(); }

    // сессия удалённого управления сама выключается через столько минут бездействия (на случай, если забыли
    // остановить вручную); 0 — без ограничения (не рекомендуется, предупреждение показывает UI)
    int inputSessionTimeoutMin() const { return m_settings.value(QStringLiteral("inputSessionTimeoutMin"), 15).toInt(); }
    void setInputSessionTimeoutMin(int v) { m_settings.setValue(QStringLiteral("inputSessionTimeoutMin"), qBound(0, v, 180)); emit inputSessionTimeoutMinChanged(); }

signals:
    void discoveryOnStartChanged();
    void startMinimizedChanged();
    void streamChanged();
    void secondChanged();
    void audioChanged();
    void repliesChanged();
    void keepRunningChanged();
    void languageChanged();
    void remoteDesktopFeatureEnabledChanged();
    void inputSessionTimeoutMinChanged();

private:
    QSettings m_settings;
};

#endif // SETTINGSMANAGER_H
