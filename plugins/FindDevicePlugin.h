#ifndef FINDDEVICEPLUGIN_H
#define FINDDEVICEPLUGIN_H

#include "PluginBase.h"
#include <QTimer>
#include <QSoundEffect>

// «Найти устройство»: просьба с одного устройства — другое громко звонит (сирена), вибрирует и показывает заметный экран
// «Я здесь!». Всё прекращается, когда:
//   • человек нажал «Нашёл» на самом устройстве (dismiss) — звук и вибрация гаснут сразу, а тому, кто искал, приходит «нашлось»;
//   • тот, кто искал, отменил поиск со своей стороны (stop);
//   • прошла минута (чтобы забытый сигнал не звучал вечно)
class FindDevicePlugin : public PluginBase
{
    Q_OBJECT
    Q_PROPERTY(bool ringing READ ringing NOTIFY ringingChanged)

public:
    explicit FindDevicePlugin(PeerManager *manager, QObject *parent = nullptr);

    bool ringing() const { return m_ringing; }

    Q_INVOKABLE void ring(const QString &deviceId);      // попросить то устройство просигналить
    Q_INVOKABLE void stop(const QString &deviceId);      // отменить поиск (там же гаснет сигнал)
    Q_INVOKABLE void dismiss();                          // «Нашёл»: погасить сигнал здесь и сообщить тому, кто искал

    void handleMessage(const QString &deviceId, const QJsonObject &payload) override;
    bool needsPermission(const QJsonObject &p) const override { return p.value(QStringLiteral("action")).toString() != QLatin1String("found"); }

signals:
    void ringRequested(const QString &fromDeviceId, const QString &fromName);   // это устройство должно засигналить
    void ringStopped();                                                         // сигнал здесь погашен
    void deviceFound(const QString &deviceId, const QString &name);             // «нашлось» — ответ на мой поиск
    void ringingChanged();

private:
    void startAlert(const QString &fromId, const QString &fromName);
    void stopAlert();
    QString ensureSoundFile();

    QTimer m_vibrateTimer;
    QTimer m_limitTimer;
    QSoundEffect m_sound;
    bool m_ringing = false;
    QString m_ringerId;          // кто ищет: ему уходит «нашёл»
};

#endif // FINDDEVICEPLUGIN_H
