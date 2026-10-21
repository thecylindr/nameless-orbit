#ifndef KEYBOARDGRAB_H
#define KEYBOARDGRAB_H

#include <QList>
#include <QObject>
#include <QSet>

class QSocketNotifier;

// Захват клавиатуры целиком (Linux, evdev): пока идёт «второй экран», все клавиши — и сочетания системы (Super, Alt+Tab, Ctrl+Alt+…) —
// уходят на другой компьютер, а не в Hyprland этого. Клавиатуры захватываются EVIOCGRAB: композитор их не видит.
// Scroll Lock — вернуть клавиатуру себе (released). При снятии захвата зажатые клавиши отпускаются у той стороны
class KeyboardGrab : public QObject
{
    Q_OBJECT
public:
    explicit KeyboardGrab(QObject *parent = nullptr);
    ~KeyboardGrab() override;

    static bool available();                  // настольный Linux, клавиатуры доступны на чтение (группа input)
    bool grab();                              // false — ни одной клавиатуры захватить не удалось
    void release();
    bool active() const { return !m_devs.isEmpty(); }

signals:
    void key(int code, int value);            // код evdev, 1 — нажата, 0 — отпущена (автоповтор не передаётся)
    void released();                          // человек нажал Scroll Lock — захват снят

private:
    void onReadable(int fd);
    struct Dev { int fd = -1; QSocketNotifier *n = nullptr; };
    QList<Dev> m_devs;
    QSet<int> m_down;                         // зажатые сейчас: при снятии захвата — отпустить
};

#endif // KEYBOARDGRAB_H
