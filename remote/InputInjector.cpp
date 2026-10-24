#include "InputInjector.h"
#include "HyprIpc.h"
#include "KWinBridge.h"
#include <QGuiApplication>
#include <QScreen>

#ifdef ORBIT_DESKTOP_LINUX
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <cstring>
#include <QProcess>
#include <QThread>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QPair>
#include <QStringList>
#endif

namespace {
#ifdef ORBIT_DESKTOP_LINUX
// имена кнопок/клавиш, которые понимает протокол — соответствие кодам ядра Linux (linux/input-event-codes.h)
QHash<QString, int> keyMap()
{
    static const QHash<QString, int> m = [] {
        QHash<QString, int> h;
        h[QStringLiteral("escape")] = KEY_ESC; h[QStringLiteral("backspace")] = KEY_BACKSPACE; h[QStringLiteral("tab")] = KEY_TAB;
        h[QStringLiteral("enter")] = KEY_ENTER; h[QStringLiteral("space")] = KEY_SPACE; h[QStringLiteral("capslock")] = KEY_CAPSLOCK;
        h[QStringLiteral("home")] = KEY_HOME; h[QStringLiteral("end")] = KEY_END; h[QStringLiteral("insert")] = KEY_INSERT; h[QStringLiteral("delete")] = KEY_DELETE;
        h[QStringLiteral("pageup")] = KEY_PAGEUP; h[QStringLiteral("pagedown")] = KEY_PAGEDOWN;
        h[QStringLiteral("arrowup")] = KEY_UP; h[QStringLiteral("arrowdown")] = KEY_DOWN; h[QStringLiteral("arrowleft")] = KEY_LEFT; h[QStringLiteral("arrowright")] = KEY_RIGHT;
        h[QStringLiteral("mute")] = KEY_MUTE; h[QStringLiteral("volumedown")] = KEY_VOLUMEDOWN; h[QStringLiteral("volumeup")] = KEY_VOLUMEUP;
        h[QStringLiteral("playpause")] = KEY_PLAYPAUSE; h[QStringLiteral("nexttrack")] = KEY_NEXTSONG; h[QStringLiteral("prevtrack")] = KEY_PREVIOUSSONG;
        h[QStringLiteral("brightnessdown")] = KEY_BRIGHTNESSDOWN; h[QStringLiteral("brightnessup")] = KEY_BRIGHTNESSUP;
        const char digits[] = "1234567890"; const int digitKeys[] = {KEY_1,KEY_2,KEY_3,KEY_4,KEY_5,KEY_6,KEY_7,KEY_8,KEY_9,KEY_0};
        for (int i = 0; i < 10; ++i) h[QString(QChar::fromLatin1(digits[i]))] = digitKeys[i];
        const char letters[] = "qwertyuiopasdfghjklzxcvbnm"; const int letterKeys[] = {
            KEY_Q,KEY_W,KEY_E,KEY_R,KEY_T,KEY_Y,KEY_U,KEY_I,KEY_O,KEY_P,KEY_A,KEY_S,KEY_D,KEY_F,KEY_G,KEY_H,KEY_J,KEY_K,KEY_L,
            KEY_Z,KEY_X,KEY_C,KEY_V,KEY_B,KEY_N,KEY_M};
        for (int i = 0; i < 26; ++i) h[QString(QChar::fromLatin1(letters[i]))] = letterKeys[i];
        for (int i = 0; i < 12; ++i) h[QStringLiteral("f%1").arg(i + 1)] = KEY_F1 + i;
        h[QStringLiteral("-")] = KEY_MINUS; h[QStringLiteral("=")] = KEY_EQUAL; h[QStringLiteral("[")] = KEY_LEFTBRACE; h[QStringLiteral("]")] = KEY_RIGHTBRACE;
        h[QStringLiteral(";")] = KEY_SEMICOLON; h[QStringLiteral("'")] = KEY_APOSTROPHE; h[QStringLiteral("`")] = KEY_GRAVE; h[QStringLiteral("\\")] = KEY_BACKSLASH;
        h[QStringLiteral(",")] = KEY_COMMA; h[QStringLiteral(".")] = KEY_DOT; h[QStringLiteral("/")] = KEY_SLASH;
        return h;
    }();
    return m;
}
QHash<QString, int> modMap()
{
    static const QHash<QString, int> m = {{QStringLiteral("ctrl"), KEY_LEFTCTRL}, {QStringLiteral("shift"), KEY_LEFTSHIFT},
                                          {QStringLiteral("alt"), KEY_LEFTALT}, {QStringLiteral("meta"), KEY_LEFTMETA}};
    return m;
}
int buttonCode(const QString &b)
{
    if (b == QLatin1String("right")) return BTN_RIGHT;
    if (b == QLatin1String("middle")) return BTN_MIDDLE;
    return BTN_LEFT;
}
#endif
}

InputInjector::InputInjector(QObject *parent) : QObject(parent) {}
InputInjector::~InputInjector() { closeDevice(); }

bool InputInjector::isAvailable()
{
#ifdef ORBIT_DESKTOP_LINUX
    return access("/dev/uinput", W_OK) == 0;
#else
    return false;   // Windows/macOS: свой backend — следующий шаг
#endif
}

bool InputInjector::openDevice()
{
#ifdef ORBIT_DESKTOP_LINUX
    if (m_fd >= 0) return true;
    const int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { m_lastError = QStringLiteral("нет доступа к /dev/uinput"); return false; }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    for (int code : keyMap().values()) ioctl(fd, UI_SET_KEYBIT, code);
    for (int code : modMap().values()) ioctl(fd, UI_SET_KEYBIT, code);
    for (int code : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA}) ioctl(fd, UI_SET_KEYBIT, code);
    for (int code = KEY_ESC; code <= KEY_MICMUTE; ++code) ioctl(fd, UI_SET_KEYBIT, code);     // вся клавиатура: переход курсора передаёт коды клавиш как есть
    for (int code : {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL}) ioctl(fd, UI_SET_RELBIT, code);

    uinput_setup setup{};
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x1209;    // pid.codes — открытый, бесплатный диапазон для самодельных устройств
    setup.id.product = 0x0a02;
    setup.id.version = 1;
    std::strncpy(setup.name, "NameLess Orbit Virtual Input", sizeof(setup.name) - 1);
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        m_lastError = QStringLiteral("не удалось создать виртуальное устройство ввода");
        ::close(fd);
        return false;
    }
    m_fd = fd;
    // Hyprland переставляет курсор своей командой; остальным композиторам (driftwm и др.) такой команды нет — для них создаётся
    // второе виртуальное устройство с абсолютными координатами (как USB-планшет): курсор попадает ровно в нужную точку экрана
    if (qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")) openAbsolutePointer();
    QThread::msleep(150);   // композитору нужно время подхватить новое устройство ввода
    emit openChanged();
    return true;
#else
    m_lastError = QStringLiteral("удалённый ввод на этой платформе пока не реализован");
    return false;
#endif
}

void InputInjector::closeDevice()
{
#ifdef ORBIT_DESKTOP_LINUX
    if (m_absFd >= 0) { ioctl(m_absFd, UI_DEV_DESTROY); ::close(m_absFd); m_absFd = -1; }
    if (m_fd < 0) return;
    ioctl(m_fd, UI_DEV_DESTROY);
    ::close(m_fd);
    m_fd = -1;
    emit openChanged();
#endif
}

void InputInjector::emitEvent(quint16 type, quint16 code, qint32 value)
{
#ifdef ORBIT_DESKTOP_LINUX
    if (m_fd < 0) return;
    input_event ev{};
    ev.type = type; ev.code = code; ev.value = value;
    if (::write(m_fd, &ev, sizeof(ev)) < 0) { /* устройство могло пропасть (вышли из сессии) — не фатально */ }
#else
    Q_UNUSED(type); Q_UNUSED(code); Q_UNUSED(value);
#endif
}

void InputInjector::syn()
{
#ifdef ORBIT_DESKTOP_LINUX
    emitEvent(EV_SYN, SYN_REPORT, 0);
#endif
}

void InputInjector::rawEvent(quint16 type, quint16 code, qint32 value)
{
#ifdef ORBIT_DESKTOP_LINUX
    if (type != EV_KEY && type != EV_REL) return;                     // только клавиши, кнопки и смещения: остальное устройство не объявляло
    if (type == EV_REL && code != REL_X && code != REL_Y && code != REL_WHEEL && code != REL_HWHEEL) return;
    emitEvent(type, code, value);
#else
    Q_UNUSED(type); Q_UNUSED(code); Q_UNUSED(value);
#endif
}

void InputInjector::rawSync() { syn(); }

void InputInjector::moveRelative(int dx, int dy)
{
    dx = qBound(-500, dx, 500); dy = qBound(-500, dy, 500);
#ifdef ORBIT_DESKTOP_LINUX
    if (dx) emitEvent(EV_REL, REL_X, dx);
    if (dy) emitEvent(EV_REL, REL_Y, dy);
    syn();
#endif
}

bool InputInjector::openAbsolutePointer()
{
#ifdef ORBIT_DESKTOP_LINUX
    if (m_absFd >= 0) return true;
    const int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return false;
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);          // без кнопки libinput не признаёт устройство указателем
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    for (int axis : {ABS_X, ABS_Y}) {
        uinput_abs_setup abs{};
        abs.code = axis;
        abs.absinfo.minimum = 0;
        abs.absinfo.maximum = 65535;
        abs.absinfo.resolution = 0;
        ioctl(fd, UI_ABS_SETUP, &abs);
    }
    uinput_setup setup{};
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x1209;
    setup.id.product = 0x0a03;
    setup.id.version = 1;
    std::strncpy(setup.name, "NameLess Orbit Virtual Pointer", sizeof(setup.name) - 1);
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) { ::close(fd); return false; }
    m_absFd = fd;
    return true;
#else
    return false;
#endif
}

void InputInjector::setTarget(const QString &output, const QRectF &frac)
{
    if (output != m_targetOutput) restoreCursor();
    m_targetOutput = output;
    m_targetFrac = frac.isValid() ? frac : QRectF(0, 0, 1, 1);
    // KDE Plasma: абсолютное устройство KWin растягивает на всю раскладку — доли монитора-цели переводятся в доли раскладки
    if (KWinBridge::isKWin() && !output.isEmpty()) {
        const QRectF L = KWinBridge::layout();
        for (const QScreen *s : QGuiApplication::screens()) {
            if (s->name() != output || L.width() <= 0 || L.height() <= 0) continue;
            const QRectF g = s->geometry();
            m_targetFrac = QRectF((g.x() - L.x() + m_targetFrac.x() * g.width()) / L.width(), (g.y() - L.y() + m_targetFrac.y() * g.height()) / L.height(),
                                  m_targetFrac.width() * g.width() / L.width(), m_targetFrac.height() * g.height() / L.height());
        }
    }
    m_geomAt = 0;                                      // геометрия цели спрашивается заново
}

bool InputInjector::warpCursor(double nx, double ny)
{
    // доля внутри цели (окно приложения) → доля всей цели
    nx = m_targetFrac.x() + qBound(0.0, nx, 1.0) * m_targetFrac.width();
    ny = m_targetFrac.y() + qBound(0.0, ny, 1.0) * m_targetFrac.height();
#ifdef ORBIT_DESKTOP_LINUX
    if (m_absFd >= 0) {                          // driftwm и другие: абсолютное устройство
        auto put = [this](quint16 type, quint16 code, qint32 value) {
            input_event ev{};
            ev.type = type; ev.code = code; ev.value = value;
            if (::write(m_absFd, &ev, sizeof(ev)) < 0) { }
        };
        put(EV_ABS, ABS_X, int(qBound(0.0, nx, 1.0) * 65535));
        put(EV_ABS, ABS_Y, int(qBound(0.0, ny, 1.0) * 65535));
        put(EV_SYN, SYN_REPORT, 0);
        return true;
    }
#endif
    return warpCursorHyprland(nx, ny);
}

bool InputInjector::warpLayout(const QPointF &p)
{
    if (!qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")) {
        HyprIpc::request(QStringLiteral("dispatch hl.dsp.cursor.move({ x = %1, y = %2 })").arg(qRound(p.x())).arg(qRound(p.y())).toUtf8());
        return true;
    }
#ifdef ORBIT_DESKTOP_LINUX
    if (m_absFd < 0 && !openAbsolutePointer()) return false;
    const QRectF L = KWinBridge::layout();
    if (L.width() <= 0 || L.height() <= 0) return false;
    auto put = [this](quint16 type, quint16 code, qint32 value) {
        input_event ev{};
        ev.type = type; ev.code = code; ev.value = value;
        if (::write(m_absFd, &ev, sizeof(ev)) < 0) { }
    };
    put(EV_ABS, ABS_X, int(qBound(0.0, (p.x() - L.x()) / L.width(), 1.0) * 65535));
    put(EV_ABS, ABS_Y, int(qBound(0.0, (p.y() - L.y()) / L.height(), 1.0) * 65535));
    put(EV_SYN, SYN_REPORT, 0);
    return true;
#else
    Q_UNUSED(p);
    return false;
#endif
}

bool InputInjector::warpCursorHyprland(double nx, double ny)
{
    // Wayland не даёт клиенту переставить системный курсор напрямую — это делает композитор (Hyprland, через IPC)
#ifdef ORBIT_DESKTOP_LINUX
    // геометрия монитора в координатах раскладки (с учётом масштаба): спрашивается у Hyprland раз в 10 секунд, а не на каждое движение.
    // Раньше значения вынимались вручную из текста и всегда получались нулевыми, поэтому курсор не двигался совсем
    double &ox = m_ox, &oy = m_oy, &w = m_w, &h = m_h;
    qint64 &askedAt = m_geomAt;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (w <= 0 || now - askedAt > 10000) {
        // сокет Hyprland, а не процесс hyprctl: запрос ~1 мс и без ожидания до полутора секунд на главном потоке
        const QJsonArray mons = QJsonDocument::fromJson(HyprIpc::request("j/monitors")).array();
        if (mons.isEmpty()) return false;
        QJsonObject pick;
        for (const QJsonValue &v : mons) {                       // нужный монитор («второй экран») по имени, иначе с фокусом, иначе первый
            const QJsonObject o = v.toObject();
            if (!m_targetOutput.isEmpty() && o.value(QStringLiteral("name")).toString() == m_targetOutput) { pick = o; break; }
            if (m_targetOutput.isEmpty() && (pick.isEmpty() || o.value(QStringLiteral("focused")).toBool())) pick = o;
        }
        if (pick.isEmpty()) return false;
        const double scale = qMax(0.1, pick.value(QStringLiteral("scale")).toDouble(1.0));
        ox = pick.value(QStringLiteral("x")).toDouble();
        oy = pick.value(QStringLiteral("y")).toDouble();
        w = pick.value(QStringLiteral("width")).toDouble() / scale;
        h = pick.value(QStringLiteral("height")).toDouble() / scale;
        askedAt = now;
    }
    if (w <= 0 || h <= 0) return false;
    const int x = int(ox + qBound(0.0, nx, 1.0) * (w - 1));
    const int y = int(oy + qBound(0.0, ny, 1.0) * (h - 1));
    if (!m_targetOutput.isEmpty()) {
        // касания второго экрана уводят системный курсор на виртуальный монитор; запоминается прежнее место, чтобы вернуть его после касаний
        if (!m_haveBack) {
            const QJsonObject c = QJsonDocument::fromJson(HyprIpc::request("j/cursorpos")).object();
            if (c.contains(QStringLiteral("x"))) { m_backX = c.value(QStringLiteral("x")).toInt(); m_backY = c.value(QStringLiteral("y")).toInt(); m_haveBack = true; }
        }
        if (m_backT.interval() != 900) {
            m_backT.setSingleShot(true); m_backT.setInterval(900);
            connect(&m_backT, &QTimer::timeout, this, &InputInjector::restoreCursor);
        }
        m_backT.start();
    }
    // раньше — отдельный процесс hyprctl на каждое движение (до 30 в секунду): fork и запуск отнимали процессор у захвата и кодера
    HyprIpc::request(QStringLiteral("dispatch hl.dsp.cursor.move({ x = %1, y = %2 })").arg(x).arg(y).toUtf8());
    return true;
#else
    Q_UNUSED(nx); Q_UNUSED(ny);
    return false;
#endif
}

void InputInjector::restoreCursor()
{
#ifdef ORBIT_DESKTOP_LINUX
    if (m_held > 0) { m_backT.start(); return; }
    m_backT.stop();
    if (!m_haveBack) return;
    m_haveBack = false;
    HyprIpc::request(QStringLiteral("dispatch hl.dsp.cursor.move({ x = %1, y = %2 })").arg(m_backX).arg(m_backY).toUtf8());
#endif
}

void InputInjector::moveAbsolute(double nx, double ny)
{
    if (!warpCursor(nx, ny)) return;
#ifdef ORBIT_DESKTOP_LINUX
    if (m_absFd >= 0) return;      // абсолютное устройство даёт настоящее событие движения — подталкивать не нужно
    // варп сам по себе Wayland-клиенты под курсором не считают движением мыши — довставляем 1px туда-обратно
    moveRelative(1, 0);
    moveRelative(-1, 0);
#endif
}

void InputInjector::mouseButton(const QString &button, bool down)
{
#ifdef ORBIT_DESKTOP_LINUX
    emitEvent(EV_KEY, buttonCode(button), down ? 1 : 0);
    syn();
    m_held = qMax(0, m_held + (down ? 1 : -1));
    if (m_haveBack) m_backT.start();              // после отпускания курсор вернётся с задержкой
#else
    Q_UNUSED(button); Q_UNUSED(down);
#endif
}

void InputInjector::click(const QString &button, int count)
{
    // нажатие и отпускание — по таймеру, а не сном главного потока: раньше каждый щелчок останавливал показ на 32 мс
    count = qBound(1, count, 3);
    for (int i = 0; i < count; ++i) {
        QTimer::singleShot(i * 32, this, [this, button]() { mouseButton(button, true); });
        QTimer::singleShot(i * 32 + 12, this, [this, button]() { mouseButton(button, false); });
    }
}

void InputInjector::scroll(int dy, int dx)
{
#ifdef ORBIT_DESKTOP_LINUX
    if (dy) emitEvent(EV_REL, REL_WHEEL, dy > 0 ? -1 : 1);
    if (dx) emitEvent(EV_REL, REL_HWHEEL, dx > 0 ? 1 : -1);
    syn();
#else
    Q_UNUSED(dy); Q_UNUSED(dx);
#endif
}

void InputInjector::keyPress(const QString &keyName, const QStringList &modifiers, bool down)
{
#ifdef ORBIT_DESKTOP_LINUX
    const auto keys = keyMap();
    const auto mods = modMap();
    const int code = keys.value(keyName.toLower(), -1);
    if (code < 0) return;
    if (down) for (const QString &m : modifiers) if (mods.contains(m)) emitEvent(EV_KEY, mods.value(m), 1);
    emitEvent(EV_KEY, code, down ? 1 : 0);
    if (!down) for (const QString &m : modifiers) if (mods.contains(m)) emitEvent(EV_KEY, mods.value(m), 0);
    syn();
#else
    Q_UNUSED(keyName); Q_UNUSED(modifiers); Q_UNUSED(down);
#endif
}

void InputInjector::keyTap(const QString &keyName, const QStringList &modifiers)
{
    keyPress(keyName, modifiers, true);
    keyPress(keyName, modifiers, false);
}

void InputInjector::typeText(const QString &text)
{
#ifdef ORBIT_DESKTOP_LINUX
    static const QHash<QChar, QPair<QString, bool>> shifted = [] {
        QHash<QChar, QPair<QString, bool>> h;
        const QString plain = QStringLiteral("1234567890-=[];'`\\,./");
        const QString shift = QStringLiteral("!@#$%^&*()_+{}:\"~|<>?");
        for (int i = 0; i < plain.size(); ++i) h[shift[i]] = {QString(plain[i]), true};
        return h;
    }();
    bool asciiOnly = true;
    for (const QChar &c : text) if (c.unicode() > 127 && c != QLatin1Char('\n') && c != QLatin1Char('\t')) { asciiOnly = false; break; }

    if (asciiOnly) {
        for (const QChar &c : text.left(2000)) {
            if (c == QLatin1Char('\n')) { keyTap(QStringLiteral("enter"), {}); continue; }
            if (c == QLatin1Char('\t')) { keyTap(QStringLiteral("tab"), {}); continue; }
            if (c.isUpper()) { keyTap(QString(c.toLower()), {QStringLiteral("shift")}); continue; }
            if (shifted.contains(c)) { keyTap(shifted[c].first, {QStringLiteral("shift")}); continue; }
            keyTap(QString(c), {});
            QThread::usleep(4000);
        }
        return;
    }
    // кириллица и прочее — через буфер обмена: надёжнее, чем гадать раскладку адресата
    QProcess copy;
    copy.start(QStringLiteral("wl-copy"));
    if (!copy.waitForStarted(1000)) return;
    copy.write(text.left(2000).toUtf8());
    copy.closeWriteChannel();
    copy.waitForFinished(1500);
    QThread::msleep(60);
    keyTap(QStringLiteral("v"), {QStringLiteral("ctrl")});
#else
    Q_UNUSED(text);
#endif
}
