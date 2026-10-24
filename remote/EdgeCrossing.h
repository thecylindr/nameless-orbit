#ifndef EDGECROSSING_H
#define EDGECROSSING_H

#include "../plugins/PluginBase.h"
#include "InputInjector.h"
#include <QHash>
#include <QJsonArray>
#include <QRectF>
#include <QSet>
#include <QTimer>
#include <QVariantMap>
#include <QPointF>

class QSocketNotifier;

// Переход курсора между компьютерами: курсор упёрся в край экрана — мышь и клавиатура этого компьютера начинают
// управлять соседним, пока курсор там не упрётся в обратный край (или не нажат Scroll Lock).
//   отправитель: край ловит NameLess UI (полоска в 1 px) и зовёт CrossTo по D-Bus; Orbit захватывает мышь и клавиатуру
//                (EVIOCGRAB — локальный рабочий стол их не видит) и пересылает события evdev пачками
//   получатель:  ставит курсор на свой край, вводит события виртуальным устройством (InputInjector) и сам замечает,
//                что курсор упёрся в край, через который вошёл, — тогда управление возвращается
// Разрешение — то же, что у управления экраном: «Демонстрация экрана» для этого устройства. Эффекты на краях рисует
// NameLess UI по сигналу crossing (D-Bus Crossing) на обеих сторонах
class EdgeCrossing : public PluginBase
{
    Q_OBJECT
    Q_PROPERTY(QString activeDeviceId READ activeDeviceId NOTIFY activeChanged)

public:
    explicit EdgeCrossing(PeerManager *manager, QObject *parent = nullptr);
    ~EdgeCrossing() override;

    static bool available();                 // Linux с Hyprland или KDE Plasma, есть доступ к /dev/input и /dev/uinput
    // KDE Plasma: какой компьютер стоит за каждым краем ({ left|right|top|bottom: deviceId }); в Hyprland края задаёт NameLess UI
    void setKwinLinks(const QVariantMap &links);
    bool isEnabled() const override { return available(); }
    void handleMessage(const QString &deviceId, const QJsonObject &payload) override;
    bool needsPermission(const QJsonObject &) const override { return false; }    // проверяется здесь: разрешение «remoteDesktop»
    QString activeDeviceId() const { return m_to.isEmpty() ? m_from : m_to; }

public slots:
    // side — край ЭТОГО экрана (left|right|top|bottom), pos — место вдоль края (0..1), screen — имя монитора
    bool crossTo(const QString &deviceId, const QString &side, double pos, const QString &screen);
    void stop();                             // вернуть курсор домой (отправитель) или отдать управление (получатель)

signals:
    void crossing(const QString &json);      // {dir: out|in, side, pos, screen, peer} — для эффектов NameLess UI
    void activeChanged();
    void errorOccurred(const QString &message);

private:
    // ---- отправитель ----
    struct Dev { int fd = -1; QSocketNotifier *n = nullptr; };
    bool grabInput();
    void releaseInput();
    void onReadable(int fd);
    void flush();                            // пачка событий — получателю (смещение копится, клавиши уходят по порядку)
    void comeBack(double pos);               // курсор вернулся: захват снят, свой курсор — на край выхода
    QString m_to, m_side, m_screen;
    double m_pos = 0.5;
    bool m_waiting = false;                  // «enter» отправлен, ответа ещё нет
    QList<Dev> m_devs;
    QJsonArray m_batch;
    int m_dx = 0, m_dy = 0;
    QTimer m_flushT, m_answerT;

    // ---- получатель ----
    void enter(const QString &deviceId, const QString &side, double pos);
    void leave(bool tellPeer);
    void checkEdge();                        // упёрся ли курсор в край входа
    QString m_from, m_inSide, m_inScreen;
    InputInjector m_inj;
    QSet<int> m_down;                        // зажатые клавиши и кнопки: при уходе отпускаются, иначе «залипнут»
    int m_push = 0;                          // сколько смещения ушло в край входа, пока курсор стоял на нём
    QTimer m_edgeT;
    QRectF m_mon;                            // монитор входа (координаты раскладки)

    // ---- Hyprland ----
    static QByteArray hypr(const QByteArray &request);      // запрос к сокету Hyprland без запуска hyprctl
    void warpTo(const QPointF &p);
    QPointF cursorPos() const;
    // ---- KDE Plasma ----
    QVariantMap m_kwinLinks;
    QString m_kwinSide, m_kwinScreen;
    double m_kwinPos = 0.5;
    QTimer m_kwinDwell;
    static QRectF monitorAt(const QString &side, const QString &name, QString *outName);
    void emitCrossing(const QString &dir, const QString &side, double pos, const QString &screen, const QString &peer);
};

#endif // EDGECROSSING_H
