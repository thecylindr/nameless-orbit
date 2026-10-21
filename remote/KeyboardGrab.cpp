#include "KeyboardGrab.h"

#include <QDir>
#include <QSocketNotifier>

#ifdef ORBIT_DESKTOP_LINUX
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

KeyboardGrab::KeyboardGrab(QObject *parent) : QObject(parent) {}
KeyboardGrab::~KeyboardGrab() { release(); }

bool KeyboardGrab::available()
{
#ifdef ORBIT_DESKTOP_LINUX
    return access("/dev/input/event0", R_OK) == 0;
#else
    return false;
#endif
}

bool KeyboardGrab::grab()
{
#ifdef ORBIT_DESKTOP_LINUX
    if (active()) return true;
    const auto bit = [](const unsigned long *bits, int n) { return (bits[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1UL; };
    for (const QString &n : QDir(QStringLiteral("/dev/input")).entryList({QStringLiteral("event*")}, QDir::System)) {
        const int fd = ::open(QByteArray("/dev/input/" + n.toLatin1()).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        char nm[256] = {0};
        ioctl(fd, EVIOCGNAME(sizeof(nm)), nm);
        unsigned long ev[EV_MAX / (8 * sizeof(long)) + 1] = {0}, key[KEY_MAX / (8 * sizeof(long)) + 1] = {0};
        ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key);
        // только настоящие клавиатуры: свои виртуальные устройства Orbit и мыши с парой кнопок-клавиш не трогаем
        const bool keyboard = bit(ev, EV_KEY) && bit(key, KEY_A) && bit(key, KEY_SPACE) && bit(key, KEY_ENTER);
        if (!keyboard || QByteArray(nm).contains("NameLess Orbit") || ioctl(fd, EVIOCGRAB, 1) < 0) { ::close(fd); continue; }
        Dev d; d.fd = fd;
        d.n = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        connect(d.n, &QSocketNotifier::activated, this, [this, fd]() { onReadable(fd); });
        m_devs.append(d);
    }
    return active();
#else
    return false;
#endif
}

void KeyboardGrab::release()
{
#ifdef ORBIT_DESKTOP_LINUX
    for (const int code : std::as_const(m_down)) emit key(code, 0);
    m_down.clear();
    for (Dev &d : m_devs) {
        if (d.n) { d.n->setEnabled(false); d.n->deleteLater(); }
        ioctl(d.fd, EVIOCGRAB, 0);
        ::close(d.fd);
    }
#endif
    m_devs.clear();
}

void KeyboardGrab::onReadable(int fd)
{
#ifdef ORBIT_DESKTOP_LINUX
    input_event evs[64];
    const ssize_t n = ::read(fd, evs, sizeof(evs));
    if (n <= 0) return;
    for (int i = 0; i < int(n / sizeof(input_event)); ++i) {
        const input_event &e = evs[i];
        if (e.type != EV_KEY || e.value == 2) continue;          // автоповтор делает система той стороны
        if (e.code == KEY_SCROLLLOCK) {
            if (e.value == 1) { release(); emit released(); return; }
            continue;
        }
        if (e.value) m_down.insert(e.code); else m_down.remove(e.code);
        emit key(e.code, e.value);
    }
#else
    Q_UNUSED(fd);
#endif
}
