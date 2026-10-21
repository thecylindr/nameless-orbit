#ifndef ORBITDBUS_H
#define ORBITDBUS_H

#include <QObject>
#include <QString>
#include <functional>

class PeerManager;
class BatteryPlugin;
class FindDevicePlugin;
class ClipboardPlugin;
class NotificationsPlugin;
class SharePlugin;
class RunCommandPlugin;

// Внешний интерфейс Orbit на сессионной шине D-Bus: так NameLess UI (виджет «Устройства») и любые скрипты
// видят состояние и управляют Orbit без его окна.
//   сервис  org.glassrice.NamelessOrbit, объект /org/glassrice/NamelessOrbit, интерфейс org.glassrice.NamelessOrbit
//   методы  Status() → JSON, Scan(), SetDiscovery(b), Connect(id), ConnectAddress(host, port), Disconnect(id), AcceptPairing(id),
//           RejectPairing(id), Ring(id), StopRing(id), SendClipboard(id), SendFile(id, path), RunCommand(id, commandId), ViewScreen(id), ShareScreen(id), SecondScreen(id), Notify(title, body, id|""), Show(), Quit()
//   сигнал  Changed() — состояние изменилось, можно перечитать Status(); отправляется вручную (notifyChanged), пачки склеиваются
// Сопряжение подтверждает человек: NameLess UI показывает тот же код, что и окно Orbit, и вызывает AcceptPairing только по
// нажатию. Разрешения для устройств и удалённый экран через D-Bus не управляются — они остаются в самом Orbit
class OrbitDBus : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.glassrice.NamelessOrbit")

public:
    OrbitDBus(PeerManager *pm, BatteryPlugin *battery, FindDevicePlugin *find, ClipboardPlugin *clipboard,
              NotificationsPlugin *notifications, SharePlugin *share, RunCommandPlugin *runCommand, QObject *parent = nullptr);

    void setRepliesGate(std::function<bool()> gate) { m_repliesAllowed = std::move(gate); }
    bool registerOnBus();                 // false — имя занято или шины нет (Orbit при этом работает дальше)
    QByteArray statusJson() const;        // тот же JSON отдаёт и канал SingleInstance

public slots:
    QString Status() const { return QString::fromUtf8(statusJson()); }
    void Scan();
    void SetDiscovery(bool on);
    void Connect(const QString &deviceId);
    void ConnectAddress(const QString &host, int port);
    void Disconnect(const QString &deviceId);
    void AcceptPairing(const QString &deviceId);
    void RejectPairing(const QString &deviceId);
    void Ring(const QString &deviceId);
    void StopRing(const QString &deviceId);
    void SendClipboard(const QString &deviceId);
    void SendFile(const QString &deviceId, const QString &path);
    void RunCommand(const QString &deviceId, const QString &commandId);        // быстрая команда другого устройства (из его списка)
    // уведомление на подключённые устройства: пустой deviceId — на все (так сообщается, например, что Claude закончил работу)
    void Notify(const QString &title, const QString &body, const QString &deviceId);
    // сообщение об ответе Claude (закончил / ждёт ответа) на все подключённые устройства; уходит, только если это не выключено в настройках Orbit
    void NotifyReply(const QString &title, const QString &body);
    // экран устройства: смотреть его (запрос показа и окно просмотра) или показать ему свой; выполняет главный код — через сигнал screenRequested
    void ViewScreen(const QString &deviceId) { emit screenRequested(deviceId, true); }
    void ShareScreen(const QString &deviceId) { emit screenRequested(deviceId, false); }
    // это устройство становится вторым монитором компьютера deviceId (с настройками по умолчанию); выполняет главный код — через сигнал secondScreenRequested
    void SecondScreen(const QString &deviceId) { emit secondScreenRequested(deviceId); }
    void Show() { emit showRequested(); }
    void Quit() { emit quitRequested(); }

signals:
    void showRequested();
    void screenRequested(const QString &deviceId, bool view);      // не экспортируется на шину — слушает main.cpp
    void secondScreenRequested(const QString &deviceId);           // не экспортируется на шину — слушает main.cpp
    void quitRequested();                 // не экспортируется на шину — слушает main.cpp и показывает окно

private:
    void notifyChanged();

    PeerManager *m_pm;
    BatteryPlugin *m_battery;
    FindDevicePlugin *m_find;
    ClipboardPlugin *m_clipboard;
    NotificationsPlugin *m_notifications;
    SharePlugin *m_share;
    RunCommandPlugin *m_run;
    std::function<bool()> m_repliesAllowed;
};

#endif // ORBITDBUS_H
