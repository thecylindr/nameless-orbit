#include "EdgeCrossing.h"
#include "../core/PeerManager.h"
#include "../core/TrustStore.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QSocketNotifier>

#ifdef ORBIT_DESKTOP_LINUX
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {
constexpr int FLUSH_MS = 8;          // смещение мыши копится и уходит ~120 раз в секунду, клавиши — сразу
constexpr int ANSWER_MS = 3000;      // столько ждём ответа на «enter», потом захват не включается
constexpr int PUSH_BACK = 24;        // сколько ещё нужно «давить» в край входа, чтобы курсор вернулся домой

QString opposite(const QString &side)
{
    if (side == QLatin1String("left")) return QStringLiteral("right");
    if (side == QLatin1String("right")) return QStringLiteral("left");
    if (side == QLatin1String("top")) return QStringLiteral("bottom");
    return QStringLiteral("top");
}

#ifdef ORBIT_DESKTOP_LINUX
bool testBit(const unsigned long *bits, int n) { return bits[n / (8 * sizeof(long))] & (1UL << (n % (8 * sizeof(long)))); }
#endif

// сенсорная панель отдаёт абсолютные координаты пальца: из них получается смещение, двумя пальцами — прокрутка, короткое касание — щелчок.
// Состояние по устройству (у ноутбука может быть и панель, и мышь)
struct Touch {
    bool pad = false;          // устройство — сенсорная панель
    double k = 0.5;            // единицы панели → точки смещения
    int scrollStep = 120;      // единиц панели на один «щелчок» колеса
    bool down = false, two = false;
    int x = -1, y = -1, lx = -1, ly = -1;
    int sx = 0, sy = 0;        // накопленная прокрутка
    qint64 t0 = 0; int travel = 0;
};
QHash<int, Touch> g_touch;
}

EdgeCrossing::EdgeCrossing(PeerManager *manager, QObject *parent)
    : PluginBase(manager, QStringLiteral("crossing"), parent)
{
    m_flushT.setSingleShot(true);
    m_flushT.setInterval(FLUSH_MS);
    connect(&m_flushT, &QTimer::timeout, this, &EdgeCrossing::flush);
    m_answerT.setSingleShot(true);
    m_answerT.setInterval(ANSWER_MS);
    connect(&m_answerT, &QTimer::timeout, this, [this]() {
        if (!m_waiting) return;
        m_waiting = false; m_to.clear();
        emit activeChanged();
        emit errorOccurred(QStringLiteral("Соседний компьютер не ответил на переход курсора"));
    });
    m_edgeT.setSingleShot(true);
    m_edgeT.setInterval(25);
    connect(&m_edgeT, &QTimer::timeout, this, &EdgeCrossing::checkEdge);
    // связь оборвалась — захват снимается сразу, иначе мышь и клавиатура остались бы у пропавшего соседа
    connect(manager, &PeerManager::deviceDisconnected, this, [this](const QString &id) {
        if (id == m_to) comeBack(m_pos);
        if (id == m_from) leave(false);
    });
}

EdgeCrossing::~EdgeCrossing()
{
    releaseInput();
    if (!m_from.isEmpty()) leave(false);
}

bool EdgeCrossing::available()
{
#ifdef ORBIT_DESKTOP_LINUX
    return !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE") && InputInjector::isAvailable() && access("/dev/input/event0", R_OK) == 0;
#else
    return false;
#endif
}

// ---------- Hyprland ----------

QByteArray EdgeCrossing::hypr(const QByteArray &request)
{
    const QString path = qEnvironmentVariable("XDG_RUNTIME_DIR") + QStringLiteral("/hypr/") + qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE") + QStringLiteral("/.socket.sock");
    QLocalSocket s;
    s.connectToServer(path);
    if (!s.waitForConnected(200)) return {};
    s.write(request);
    s.waitForBytesWritten(200);
    QByteArray out;
    while (s.waitForReadyRead(300)) out += s.readAll();       // Hyprland закрывает соединение после ответа
    return out;
}

// монитор по имени или самый крайний со стороны side; координаты раскладки (логические, с учётом масштаба и поворота)
QRectF EdgeCrossing::monitorAt(const QString &side, const QString &name, QString *outName)
{
    const QJsonArray mons = QJsonDocument::fromJson(hypr("j/monitors")).array();
    QRectF best; QString bestName;
    for (const QJsonValue &v : mons) {
        const QJsonObject m = v.toObject();
        const double sc = qMax(0.1, m.value(QStringLiteral("scale")).toDouble(1.0));
        double w = m.value(QStringLiteral("width")).toDouble() / sc, h = m.value(QStringLiteral("height")).toDouble() / sc;
        if (m.value(QStringLiteral("transform")).toInt() % 2 == 1) std::swap(w, h);
        const QRectF r(m.value(QStringLiteral("x")).toDouble(), m.value(QStringLiteral("y")).toDouble(), w, h);
        const QString n = m.value(QStringLiteral("name")).toString();
        if (!name.isEmpty() && n == name) { if (outName) *outName = n; return r; }
        const bool better = best.isNull()
            || (side == QLatin1String("left") && r.left() < best.left()) || (side == QLatin1String("right") && r.right() > best.right())
            || (side == QLatin1String("top") && r.top() < best.top()) || (side == QLatin1String("bottom") && r.bottom() > best.bottom());
        if (better) { best = r; bestName = n; }
    }
    if (outName) *outName = bestName;
    return best;
}

static QPointF edgePoint(const QRectF &m, const QString &side, double pos, double inset)
{
    pos = qBound(0.0, pos, 1.0);
    if (side == QLatin1String("left")) return {m.left() + inset, m.top() + pos * (m.height() - 1)};
    if (side == QLatin1String("right")) return {m.right() - inset, m.top() + pos * (m.height() - 1)};
    if (side == QLatin1String("top")) return {m.left() + pos * (m.width() - 1), m.top() + inset};
    return {m.left() + pos * (m.width() - 1), m.bottom() - inset};
}

static void warpTo(const QPointF &p)
{
    // курсор переставляет композитор; варп сам по себе не событие ввода, поэтому окно под курсором узнает о нём при первом движении
    const QByteArray cmd = QStringLiteral("dispatch hl.dsp.cursor.move({ x = %1, y = %2 })").arg(qRound(p.x())).arg(qRound(p.y())).toUtf8();
    QLocalSocket s;
    const QString path = qEnvironmentVariable("XDG_RUNTIME_DIR") + QStringLiteral("/hypr/") + qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE") + QStringLiteral("/.socket.sock");
    s.connectToServer(path);
    if (!s.waitForConnected(200)) return;
    s.write(cmd);
    s.waitForBytesWritten(200);
    s.waitForReadyRead(200);
}

void EdgeCrossing::emitCrossing(const QString &dir, const QString &side, double pos, const QString &screen, const QString &peer)
{
    QJsonObject o;
    o[QStringLiteral("dir")] = dir; o[QStringLiteral("side")] = side; o[QStringLiteral("pos")] = pos;
    o[QStringLiteral("screen")] = screen; o[QStringLiteral("peer")] = peer;
    emit crossing(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
}

// ---------- отправитель ----------

bool EdgeCrossing::crossTo(const QString &deviceId, const QString &side, double pos, const QString &screen)
{
    if (!available() || !m_to.isEmpty() || !m_from.isEmpty() || !m_manager->isConnected(deviceId)) return false;
    m_to = deviceId; m_side = side; m_pos = qBound(0.0, pos, 1.0); m_screen = screen;
    m_waiting = true;
    QJsonObject p;
    p[QStringLiteral("kind")] = QStringLiteral("enter");
    p[QStringLiteral("side")] = opposite(side);          // у соседа курсор входит с противоположного края
    p[QStringLiteral("pos")] = m_pos;
    m_manager->sendPlugin(deviceId, name(), p);
    m_answerT.start();
    emit activeChanged();
    return true;
}

void EdgeCrossing::stop()
{
    if (!m_to.isEmpty()) {
        if (!m_waiting) { QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("leave"); m_manager->sendPlugin(m_to, name(), p); }
        comeBack(m_pos);
    }
    if (!m_from.isEmpty()) leave(true);
}

bool EdgeCrossing::grabInput()
{
#ifdef ORBIT_DESKTOP_LINUX
    releaseInput();
    const QStringList nodes = QDir(QStringLiteral("/dev/input")).entryList({QStringLiteral("event*")}, QDir::System);
    for (const QString &n : nodes) {
        const int fd = ::open(QByteArray("/dev/input/" + n.toLatin1()).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        char nm[256] = {0};
        ioctl(fd, EVIOCGNAME(sizeof(nm)), nm);
        if (QByteArray(nm).contains("NameLess Orbit")) { ::close(fd); continue; }      // своё виртуальное устройство
        unsigned long ev[EV_MAX / (8 * sizeof(long)) + 1] = {0}, key[KEY_MAX / (8 * sizeof(long)) + 1] = {0}, rel[REL_MAX / (8 * sizeof(long)) + 1] = {0}, abs[ABS_MAX / (8 * sizeof(long)) + 1] = {0};
        ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key);
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
        const bool mouse = testBit(ev, EV_REL) && testBit(rel, REL_X);
        const bool keyboard = testBit(ev, EV_KEY) && testBit(key, KEY_A) && testBit(key, KEY_SPACE);
        const bool pad = testBit(ev, EV_ABS) && testBit(abs, ABS_X) && testBit(key, BTN_TOUCH) && testBit(key, BTN_TOOL_FINGER);
        if (!(mouse || keyboard || pad) || ioctl(fd, EVIOCGRAB, 1) < 0) { ::close(fd); continue; }
        Touch t;
        if (pad) {
            input_absinfo ax{};
            ioctl(fd, EVIOCGABS(ABS_X), &ax);
            t.pad = true;
            // разрешение — единиц на миллиметр: миллиметр пальца ≈ 3,5 точки, как у libinput без ускорения
            t.k = ax.resolution > 0 ? 3.5 / ax.resolution : qMax(0.2, 1500.0 / qMax(1, ax.maximum - ax.minimum));
            t.scrollStep = ax.resolution > 0 ? ax.resolution * 4 : 120;
        }
        g_touch.insert(fd, t);
        Dev d; d.fd = fd;
        d.n = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        connect(d.n, &QSocketNotifier::activated, this, [this, fd]() { onReadable(fd); });
        m_devs.append(d);
    }
    return !m_devs.isEmpty();
#else
    return false;
#endif
}

void EdgeCrossing::releaseInput()
{
#ifdef ORBIT_DESKTOP_LINUX
    for (Dev &d : m_devs) {
        if (d.n) { d.n->setEnabled(false); d.n->deleteLater(); }
        ioctl(d.fd, EVIOCGRAB, 0);
        ::close(d.fd);
        g_touch.remove(d.fd);
    }
#endif
    m_devs.clear();
    m_batch = QJsonArray(); m_dx = 0; m_dy = 0;
    m_flushT.stop();
}

void EdgeCrossing::onReadable(int fd)
{
#ifdef ORBIT_DESKTOP_LINUX
    input_event evs[64];
    const ssize_t n = ::read(fd, evs, sizeof(evs));
    if (n <= 0) return;
    Touch &t = g_touch[fd];
    bool urgent = false;
    const auto push = [this](int type, int code, int value) {
        if (m_dx || m_dy) {          // смещение — до клавиши: щелчок должен прийти туда, куда уже доехал курсор
            if (m_dx) m_batch.append(QJsonArray{EV_REL, REL_X, m_dx});
            if (m_dy) m_batch.append(QJsonArray{EV_REL, REL_Y, m_dy});
            m_dx = 0; m_dy = 0;
        }
        m_batch.append(QJsonArray{type, code, value});
    };
    for (int i = 0; i < int(n / sizeof(input_event)); ++i) {
        const input_event &e = evs[i];
        if (e.type == EV_KEY) {
            if (e.code == KEY_SCROLLLOCK) { if (e.value == 1) { stop(); return; } continue; }     // аварийный возврат
            if (t.pad && (e.code == BTN_TOUCH || e.code == BTN_TOOL_FINGER || e.code == BTN_TOOL_DOUBLETAP || e.code == BTN_TOOL_TRIPLETAP || e.code == BTN_TOOL_QUADTAP)) {
                if (e.code == BTN_TOOL_DOUBLETAP) { t.two = e.value != 0; t.sx = t.sy = 0; }
                if (e.code == BTN_TOUCH) {
                    const qint64 now = qint64(e.input_event_sec) * 1000 + e.input_event_usec / 1000;
                    if (e.value) { t.down = true; t.lx = t.ly = -1; t.t0 = now; t.travel = 0; }
                    else {
                        // короткое касание без движения — щелчок левой кнопкой (как «касание для щелчка»)
                        if (!t.two && now - t.t0 < 180 && t.travel < 8) { push(EV_KEY, BTN_LEFT, 1); push(EV_KEY, BTN_LEFT, 0); urgent = true; }
                        t.down = false; t.two = false;
                    }
                }
                continue;
            }
            if (e.value == 2) continue;        // автоповтор делает сама система у соседа
            push(e.type, e.code, e.value);
            urgent = true;
        } else if (e.type == EV_REL) {
            if (e.code == REL_X) m_dx += e.value;
            else if (e.code == REL_Y) m_dy += e.value;
            else if (e.code == REL_WHEEL || e.code == REL_HWHEEL) { push(e.type, e.code, e.value); urgent = true; }
        } else if (e.type == EV_ABS && t.pad && t.down) {
            if (e.code == ABS_X) t.x = e.value;
            else if (e.code == ABS_Y) t.y = e.value;
        } else if (e.type == EV_SYN && t.pad && t.down && t.x >= 0 && t.y >= 0) {
            if (t.lx >= 0) {
                const int ddx = t.x - t.lx, ddy = t.y - t.ly;
                t.travel += qAbs(ddx) + qAbs(ddy);
                if (t.two) {
                    // два пальца — прокрутка: щелчок колеса на каждые ~4 мм
                    t.sy += ddy; t.sx += ddx;
                    while (qAbs(t.sy) >= t.scrollStep) { push(EV_REL, REL_WHEEL, t.sy > 0 ? 1 : -1); t.sy -= t.sy > 0 ? t.scrollStep : -t.scrollStep; urgent = true; }
                    while (qAbs(t.sx) >= t.scrollStep) { push(EV_REL, REL_HWHEEL, t.sx > 0 ? -1 : 1); t.sx -= t.sx > 0 ? t.scrollStep : -t.scrollStep; urgent = true; }
                } else {
                    m_dx += qRound(ddx * t.k); m_dy += qRound(ddy * t.k);
                }
            }
            t.lx = t.x; t.ly = t.y;
        }
    }
    if (m_waiting) { m_batch = QJsonArray(); m_dx = m_dy = 0; return; }     // сосед ещё не ответил — ввод никуда не идёт
    if (urgent) flush();
    else if ((m_dx || m_dy) && !m_flushT.isActive()) m_flushT.start();
#else
    Q_UNUSED(fd);
#endif
}

void EdgeCrossing::flush()
{
    m_flushT.stop();
    if (m_to.isEmpty()) return;
    if (m_dx) m_batch.append(QJsonArray{2 /* EV_REL */, 0 /* REL_X */, m_dx});
    if (m_dy) m_batch.append(QJsonArray{2, 1 /* REL_Y */, m_dy});
    m_dx = 0; m_dy = 0;
    if (m_batch.isEmpty()) return;
    QJsonObject p;
    p[QStringLiteral("kind")] = QStringLiteral("ev");
    p[QStringLiteral("e")] = m_batch;
    m_manager->sendPlugin(m_to, name(), p);
    m_batch = QJsonArray();
}

void EdgeCrossing::comeBack(double pos)
{
    if (m_to.isEmpty()) return;
    const QString peer = m_manager->trustStore()->device(m_to).name;
    releaseInput();
    m_answerT.stop();
    const bool wasActive = !m_waiting;
    m_waiting = false;
    m_to.clear();
    emit activeChanged();
    if (!wasActive) return;
    QString scr;
    const QRectF mon = monitorAt(m_side, m_screen, &scr);
    if (!mon.isNull()) warpTo(edgePoint(mon, m_side, pos, 6));      // чуть внутрь от края, чтобы не уйти обратно сразу
    emitCrossing(QStringLiteral("in"), m_side, pos, scr, peer);
}

// ---------- получатель ----------

void EdgeCrossing::enter(const QString &deviceId, const QString &side, double pos)
{
    const auto deny = [&](const QString &why) {
        QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("denied"); p[QStringLiteral("reason")] = why;
        m_manager->sendPlugin(deviceId, name(), p);
    };
    if (!m_manager->trustStore()->pluginAllowed(deviceId, QStringLiteral("remoteDesktop"))) { deny(QStringLiteral("на том компьютере для вас не включена «Демонстрация экрана»")); return; }
    if (!available()) { deny(QStringLiteral("переход курсора на том компьютере недоступен (нужен Hyprland и доступ к /dev/uinput)")); return; }
    if (!m_to.isEmpty() || (!m_from.isEmpty() && m_from != deviceId)) { deny(QStringLiteral("тот компьютер сейчас занят другим переходом")); return; }
    if (!m_inj.isOpen() && !m_inj.openDevice()) { deny(m_inj.lastError()); return; }
    m_from = deviceId; m_inSide = side; m_push = 0; m_down.clear();
    m_mon = monitorAt(side, QString(), &m_inScreen);
    if (!m_mon.isNull()) warpTo(edgePoint(m_mon, side, pos, 2));
    QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("entered");
    m_manager->sendPlugin(deviceId, name(), p);
    emit activeChanged();
    emitCrossing(QStringLiteral("in"), side, pos, m_inScreen, m_manager->trustStore()->device(deviceId).name);
}

void EdgeCrossing::leave(bool tellPeer)
{
    if (m_from.isEmpty()) return;
#ifdef ORBIT_DESKTOP_LINUX
    for (int code : std::as_const(m_down)) m_inj.rawEvent(EV_KEY, quint16(code), 0);       // зажатое у соседа отпускается
    m_inj.rawSync();
#endif
    m_down.clear();
    m_edgeT.stop();
    double pos = 0.5;
    const QJsonObject c = QJsonDocument::fromJson(hypr("j/cursorpos")).object();
    if (!m_mon.isNull()) {
        const bool vertical = m_inSide == QLatin1String("left") || m_inSide == QLatin1String("right");
        pos = vertical ? (c.value(QStringLiteral("y")).toDouble() - m_mon.top()) / qMax(1.0, m_mon.height())
                       : (c.value(QStringLiteral("x")).toDouble() - m_mon.left()) / qMax(1.0, m_mon.width());
    }
    if (tellPeer) { QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("back"); p[QStringLiteral("pos")] = qBound(0.0, pos, 1.0); m_manager->sendPlugin(m_from, name(), p); }
    const QString peer = m_manager->trustStore()->device(m_from).name;
    m_from.clear();
    emit activeChanged();
    emitCrossing(QStringLiteral("out"), m_inSide, qBound(0.0, pos, 1.0), m_inScreen, peer);
}

void EdgeCrossing::checkEdge()
{
    if (m_from.isEmpty() || m_mon.isNull()) return;
    const QJsonObject c = QJsonDocument::fromJson(hypr("j/cursorpos")).object();
    const double x = c.value(QStringLiteral("x")).toDouble(), y = c.value(QStringLiteral("y")).toDouble();
    bool atEdge = false;
    if (m_inSide == QLatin1String("left")) atEdge = x <= m_mon.left() + 1 && y >= m_mon.top() && y <= m_mon.bottom();
    else if (m_inSide == QLatin1String("right")) atEdge = x >= m_mon.right() - 2 && y >= m_mon.top() && y <= m_mon.bottom();
    else if (m_inSide == QLatin1String("top")) atEdge = y <= m_mon.top() + 1 && x >= m_mon.left() && x <= m_mon.right();
    else atEdge = y >= m_mon.bottom() - 2 && x >= m_mon.left() && x <= m_mon.right();
    if (!atEdge) { m_push = 0; return; }
    if (m_push >= PUSH_BACK) leave(true);
}

void EdgeCrossing::handleMessage(const QString &deviceId, const QJsonObject &payload)
{
    const QString kind = payload.value(QStringLiteral("kind")).toString();
    // ---- получатель ----
    if (kind == QLatin1String("enter")) { enter(deviceId, payload.value(QStringLiteral("side")).toString(), payload.value(QStringLiteral("pos")).toDouble(0.5)); return; }
    if (kind == QLatin1String("ev")) {
        if (deviceId != m_from) return;
        int toward = 0;          // смещение в сторону края входа: по нему решается возврат
        for (const QJsonValue &v : payload.value(QStringLiteral("e")).toArray()) {
            const QJsonArray a = v.toArray();
            if (a.size() != 3) continue;
            const int type = a.at(0).toInt(), code = a.at(1).toInt(), value = a.at(2).toInt();
            if (type == 1 /* EV_KEY */) { if (value) m_down.insert(code); else m_down.remove(code); }
            if (type == 2 /* EV_REL */) {
                if (code == 0 && (m_inSide == QLatin1String("left") || m_inSide == QLatin1String("right"))) toward += m_inSide == QLatin1String("left") ? -value : value;
                if (code == 1 && (m_inSide == QLatin1String("top") || m_inSide == QLatin1String("bottom"))) toward += m_inSide == QLatin1String("top") ? -value : value;
            }
            m_inj.rawEvent(quint16(type), quint16(code), value);
        }
        m_inj.rawSync();
        if (toward > 0) { m_push += toward; if (!m_edgeT.isActive()) m_edgeT.start(); }
        else if (toward < 0) m_push = 0;
        return;
    }
    if (kind == QLatin1String("leave")) { if (deviceId == m_from) leave(false); return; }
    // ---- отправитель ----
    if (deviceId != m_to) return;
    if (kind == QLatin1String("entered")) {
        if (!m_waiting) return;
        m_answerT.stop();
        if (!grabInput()) {
            m_waiting = false;
            QJsonObject p; p[QStringLiteral("kind")] = QStringLiteral("leave"); m_manager->sendPlugin(m_to, name(), p);
            m_to.clear(); emit activeChanged();
            emit errorOccurred(QStringLiteral("Не удалось захватить мышь и клавиатуру: нужен доступ к /dev/input (группа input)"));
            return;
        }
        m_waiting = false;
        emitCrossing(QStringLiteral("out"), m_side, m_pos, m_screen, m_manager->trustStore()->device(m_to).name);
        return;
    }
    if (kind == QLatin1String("denied")) {
        m_answerT.stop(); m_waiting = false; m_to.clear(); emit activeChanged();
        emit errorOccurred(QStringLiteral("Переход курсора: ") + payload.value(QStringLiteral("reason")).toString());
        return;
    }
    if (kind == QLatin1String("back")) { comeBack(payload.value(QStringLiteral("pos")).toDouble(m_pos)); return; }
}
