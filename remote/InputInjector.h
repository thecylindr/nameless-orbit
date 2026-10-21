#ifndef INPUTINJECTOR_H
#define INPUTINJECTOR_H

#include <QObject>
#include <QRectF>
#include <QString>
#include <QTimer>

// Виртуальная мышь и клавиатура этого компьютера — то, чем удалённое устройство на самом деле управляет курсором
// и вводом текста. На Linux — через /dev/uinput (доступен текущему пользователю без root благодаря правилу udev
// uaccess, как и у остальных устройств ввода в сессии); на других системах класс компилируется, но isAvailable()
// возвращает false, пока не появится свой backend (Windows — SendInput, macOS — CGEvent; следующий шаг).
//
// Этот класс сам НИКОГО не спрашивает и ничего не решает о том, можно ли сейчас принимать ввод — он открывает
// устройство, только когда RemoteDesktopController явно вызывает open() после того, как пользователь включил
// сессию управления, и closes() сразу, как только сессия заканчивается (отключение, таймаут, нажатие «Стоп»).
class InputInjector : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool open READ isOpen NOTIFY openChanged)

public:
    explicit InputInjector(QObject *parent = nullptr);
    ~InputInjector() override;

    static bool isAvailable();   // доступен ли /dev/uinput на запись (не открывая устройство)

    bool openDevice();           // true — удалось; false — см. lastError()
    void closeDevice();
    bool isOpen() const { return m_fd >= 0; }
    QString lastError() const { return m_lastError; }

public slots:
    // координаты — относительное смещение в «точках» виртуальной мыши (как и обычная мышь, не абсолютные пиксели)
    void moveRelative(int dx, int dy);
    // абсолютная точка экрана (0..1 по каждой оси) — для «нажать туда, куда коснулись на телефоне»;
    // средствами Wayland напрямую не делается, поэтому переставляет курсор композитором (Hyprland) и довставляет
    // крошечным relative-сдвигом, чтобы окно под курсором увидело движение (Wayland не считает варп событием ввода)
    void moveAbsolute(double nx, double ny);
    // Куда попадают абсолютные координаты (0..1): вместо основного монитора — выход output (виртуальный монитор «второго экрана») и/или
    // доля экрана frac (окно приложения). Пустое имя и весь прямоугольник — как обычно
    void setTarget(const QString &output, const QRectF &frac = QRectF(0, 0, 1, 1));
    void mouseButton(const QString &button, bool down);   // "left" | "right" | "middle"
    void click(const QString &button, int count = 1);
    void scroll(int dy, int dx = 0);
    void keyPress(const QString &keyName, const QStringList &modifiers, bool down);
    void keyTap(const QString &keyName, const QStringList &modifiers);
    // печать текста: печатаемая латиница — по клавишам; остальное (кириллица, эмодзи) — через буфер обмена и Ctrl+V,
    // так же, как сделано для надёжной передачи любого текста без таблиц раскладок на обеих сторонах
    void typeText(const QString &text);

signals:
    void openChanged();

private:
    void emitEvent(quint16 type, quint16 code, qint32 value);
    void syn();
    bool warpCursor(double nx, double ny);
    bool warpCursorHyprland(double nx, double ny);
    void restoreCursor();             // курсор возвращается туда, где был до касаний второго экрана
    QTimer m_backT;
    bool m_haveBack = false;
    int m_backX = 0, m_backY = 0;
    int m_held = 0;                   // сколько кнопок мыши зажато (пока зажаты — курсор не возвращается)
    QString m_targetOutput;
    QRectF m_targetFrac{0, 0, 1, 1};
    double m_ox = 0, m_oy = 0, m_w = 0, m_h = 0;      // геометрия цели на раскладке Hyprland (кэш)
    qint64 m_geomAt = 0;
    bool openAbsolutePointer();       // не-Hyprland (driftwm и другие): курсор ставится виртуальным «планшетом» с абсолютными координатами

    int m_fd = -1;
    int m_absFd = -1;
    QString m_lastError;
};

#endif // INPUTINJECTOR_H
