#include "KWinBridge.h"
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QScreen>
#include <QStandardPaths>

#ifdef ORBIT_DESKTOP_LINUX
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#endif

namespace {
const char *kPath = "/org/glassrice/NamelessOrbit/KWin";

// скрипт KWin: положение курсора при каждом движении
const char *kTrackJs = R"JS(
function report() {
    var p = workspace.cursorPos;
    callDBus("org.glassrice.NamelessOrbit", "/org/glassrice/NamelessOrbit/KWin", "org.glassrice.NamelessOrbit.KWin", "Cursor", p.x, p.y);
}
workspace.cursorPosChanged.connect(report);
report();
)JS";

// скрипт KWin: курсор коснулся наружного края раскладки (за ним нет другого монитора) — сторона, место вдоль края, монитор;
// ушёл с края — пустая сторона. Задержку перед переходом выдерживает Orbit
const char *kEdgeJs = R"JS(
var last = "";
function outer(p) {
    var ss = workspace.screens;
    for (var i = 0; i < ss.length; ++i) {
        var g = ss[i].geometry;
        if (p.x < g.x || p.x >= g.x + g.width || p.y < g.y || p.y >= g.y + g.height) continue;
        var side = "", pos = 0;
        if (p.x <= g.x) { side = "left"; pos = (p.y - g.y) / g.height; }
        else if (p.x >= g.x + g.width - 1) { side = "right"; pos = (p.y - g.y) / g.height; }
        else if (p.y <= g.y) { side = "top"; pos = (p.x - g.x) / g.width; }
        else if (p.y >= g.y + g.height - 1) { side = "bottom"; pos = (p.x - g.x) / g.width; }
        if (side === "") return null;
        // за краем есть соседний монитор — это не край раскладки
        var nx = p.x + (side === "right" ? 2 : (side === "left" ? -2 : 0)), ny = p.y + (side === "bottom" ? 2 : (side === "top" ? -2 : 0));
        for (var j = 0; j < ss.length; ++j) {
            var h = ss[j].geometry;
            if (nx >= h.x && nx < h.x + h.width && ny >= h.y && ny < h.y + h.height) return null;
        }
        return { side: side, pos: pos, screen: ss[i].name };
    }
    return null;
}
workspace.cursorPosChanged.connect(function () {
    var e = outer(workspace.cursorPos);
    var key = e ? e.side + e.screen : "";
    if (key === last) return;
    last = key;
    callDBus("org.glassrice.NamelessOrbit", "/org/glassrice/NamelessOrbit/KWin", "org.glassrice.NamelessOrbit.KWin", "Edge",
             e ? e.side : "", e ? e.pos : 0, e ? e.screen : "");
});
)JS";
}

bool KWinBridge::isKWin()
{
    if (!qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")) return false;
    const QString d = qEnvironmentVariable("XDG_CURRENT_DESKTOP") + QLatin1Char(':') + qEnvironmentVariable("XDG_SESSION_DESKTOP");
    return d.contains(QLatin1String("KDE"), Qt::CaseInsensitive) && QGuiApplication::platformName() == QLatin1String("wayland");
}

KWinBridge *KWinBridge::instance()
{
    static KWinBridge *b = new KWinBridge(qApp);
    return b;
}

KWinBridge::KWinBridge(QObject *parent) : QObject(parent) {}

QRectF KWinBridge::layout()
{
    QRectF r;
    for (const QScreen *s : QGuiApplication::screens()) r = r.united(QRectF(s->geometry()));
    return r;
}

bool KWinBridge::load(const QString &plugin, const QString &source)
{
#ifdef ORBIT_DESKTOP_LINUX
    if (!m_registered) {
        // объект для обратных вызовов скриптов; сам сервис регистрирует OrbitDBus
        m_registered = QDBusConnection::sessionBus().registerObject(QString::fromLatin1(kPath), this, QDBusConnection::ExportScriptableSlots);
    }
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    const QString file = dir + QLatin1Char('/') + plugin + QStringLiteral(".js");
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(source.toUtf8());
    f.close();
    QDBusInterface scripting(QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting"), QStringLiteral("org.kde.kwin.Scripting"));
    if (!scripting.isValid()) return false;
    scripting.call(QStringLiteral("unloadScript"), plugin);                // остаток прошлого запуска
    const QDBusReply<int> id = scripting.call(QStringLiteral("loadScript"), file, plugin);
    if (!id.isValid() || id.value() < 0) return false;
    scripting.call(QStringLiteral("start"));                               // запускает загруженные, но ещё не запущенные скрипты
    return true;
#else
    Q_UNUSED(plugin); Q_UNUSED(source);
    return false;
#endif
}

void KWinBridge::unload(const QString &plugin)
{
#ifdef ORBIT_DESKTOP_LINUX
    QDBusInterface scripting(QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting"), QStringLiteral("org.kde.kwin.Scripting"));
    if (scripting.isValid()) scripting.call(QDBus::NoBlock, QStringLiteral("unloadScript"), plugin);
#else
    Q_UNUSED(plugin);
#endif
}

void KWinBridge::setTracking(bool on)
{
    if (on == m_tracking) return;
    m_tracking = on;
    if (on) { m_have = false; load(QStringLiteral("nameless-orbit-track"), QString::fromLatin1(kTrackJs)); }
    else unload(QStringLiteral("nameless-orbit-track"));
}

void KWinBridge::setEdgeWatch(bool on)
{
    if (on == m_edges) return;
    m_edges = on;
    if (on) load(QStringLiteral("nameless-orbit-edge"), QString::fromLatin1(kEdgeJs));
    else unload(QStringLiteral("nameless-orbit-edge"));
}

void KWinBridge::Cursor(double x, double y)
{
    m_cursor = QPointF(x, y);
    m_have = true;
}

void KWinBridge::Edge(const QString &side, double pos, const QString &screen)
{
    emit edgeHit(side, qBound(0.0, pos, 1.0), screen);
}
