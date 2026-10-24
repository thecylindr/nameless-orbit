#ifndef KWINBRIDGE_H
#define KWINBRIDGE_H

#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QString>

// Связь с KWin (KDE Plasma, Wayland): то, что в Hyprland даёт его сокет, здесь получается через скрипты KWin и D-Bus.
//   положение курсора — скрипт «orbit-track» шлёт его при каждом движении (загружается только на время перехода курсора);
//   края экрана — скрипт «orbit-edge» сообщает, что курсор упёрся в наружный край раскладки (для перехода на соседний компьютер);
//   перестановка курсора — абсолютным виртуальным устройством (InputInjector), не здесь.
// Скрипты пишутся в ~/.cache и загружаются через org.kde.KWin /Scripting; вызывают обратно объект
// /org/glassrice/NamelessOrbit/KWin сервиса Orbit. Hyprland и driftwm этот класс не трогает
class KWinBridge : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.glassrice.NamelessOrbit.KWin")

public:
    static bool isKWin();                    // сессия KDE Plasma под Wayland (и не Hyprland)
    static KWinBridge *instance();

    void setTracking(bool on);               // слежение за курсором (получатель перехода)
    void setEdgeWatch(bool on);              // слежение за краями раскладки (отправитель перехода)
    bool haveCursor() const { return m_have; }
    QPointF cursor() const { return m_cursor; }
    static QRectF layout();                  // вся раскладка мониторов (логические пиксели), по экранам Qt

public slots:
    // вызываются скриптами KWin по D-Bus
    Q_SCRIPTABLE void Cursor(double x, double y);
    Q_SCRIPTABLE void Edge(const QString &side, double pos, const QString &screen);

signals:
    void edgeHit(const QString &side, double pos, const QString &screen);

private:
    explicit KWinBridge(QObject *parent = nullptr);
    bool load(const QString &plugin, const QString &source);
    void unload(const QString &plugin);
    bool m_registered = false, m_tracking = false, m_edges = false, m_have = false;
    QPointF m_cursor;
};

#endif // KWINBRIDGE_H
