#include "VirtualDisplay.h"
#include "HyprIpc.h"
#include "KWinBridge.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QVariantMap>
#include <atomic>

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)

namespace {
// запуск команды с ожиданием; stdout — в out. false — не запустилась или не уложилась в срок
bool run(const QString &prog, const QStringList &args, QByteArray *out = nullptr, int timeoutMs = 2500)
{
    QProcess p;
    p.setStandardInputFile(QProcess::nullDevice());
    p.start(prog, args);
    if (!p.waitForFinished(timeoutMs)) { p.kill(); return false; }
    if (out) *out = p.readAllStandardOutput();
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

// запрос к Hyprland: через его сокет (~1 мс, без процесса); сокет не ответил — процессом hyprctl.
// Раньше каждый запрос был отдельным hyprctl, а создание монитора делало их два десятка подряд — окно Orbit замирало
QByteArray hyprJson(const char *what)
{
    QByteArray o = HyprIpc::request(QByteArray("j/") + what);
    if (o.isEmpty()) run(QStringLiteral("hyprctl"), {QStringLiteral("-j"), QString::fromLatin1(what)}, &o);
    return o;
}

QJsonArray hyprMonitors() { return QJsonDocument::fromJson(hyprJson("monitors")).array(); }

void dispatch(const QString &lua)
{
    if (HyprIpc::request("dispatch " + lua.toUtf8()).isEmpty()) run(QStringLiteral("hyprctl"), {QStringLiteral("dispatch"), lua});
}

void outputRemove(const QString &name)
{
    if (HyprIpc::request("output remove " + name.toUtf8(), 1500).isEmpty()) run(QStringLiteral("hyprctl"), {QStringLiteral("output"), QStringLiteral("remove"), name});
}

// монитор по имени из списка Hyprland; пусто — его нет
QJsonObject monitorByName(const QString &name)
{
    for (const QJsonValue &v : hyprMonitors()) if (v.toObject().value(QStringLiteral("name")).toString() == name) return v.toObject();
    return {};
}

QString stateFile() { return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/secondscreen.json"); }

void remember(const QString &name)
{
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    QFile f(stateFile());
    if (name.isEmpty()) { f.remove(); return; }
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(name.toUtf8());
}
// Страж: отдельный процесс, переживающий Orbit. Если Orbit убит (kill -9, падение, принудительное закрытие показа), виртуальный монитор остался бы в Hyprland:
// курсор и окна уходили на невидимый экран, рабочий стол не возвращался, захват висел. Страж ждёт смерти Orbit и, если файл «осталось от запуска»
// ещё содержит имя монитора (при штатном выходе destroy() его стирает), убирает монитор и добивает осиротевший wf-recorder этого монитора.
// Имя читается из файла в момент смерти: у каждого второго экрана своё имя, а страж один на запуск
void guard()
{
    static bool started = false;
    if (started) return;
    started = true;
    const QString script = QStringLiteral(
        "pid=\"$1\"; state=\"$2\"; "
        "while kill -0 \"$pid\" 2>/dev/null; do sleep 1; done; "
        "sleep 1; name=\"$(cat \"$state\" 2>/dev/null)\"; case \"$name\" in HEADLESS*) ;; *) exit 0 ;; esac; "
        "for p in $(pgrep -x wf-recorder); do tr '\\0' ' ' < /proc/$p/cmdline 2>/dev/null | grep -q -- \" -o $name \" && kill \"$p\" 2>/dev/null; done; "
        "sleep 0.3; hyprctl output remove \"$name\" >/dev/null 2>&1; rm -f \"$state\"");
    QProcess::startDetached(QStringLiteral("sh"), {QStringLiteral("-c"), script, QStringLiteral("orbit-guard"),
                                                   QString::number(QCoreApplication::applicationPid()), stateFile()});
}


QJsonObject driftwmState()
{
    QByteArray o;
    if (!run(QStringLiteral("driftwm"), {QStringLiteral("msg"), QStringLiteral("--json"), QStringLiteral("state")}, &o)) return {};
    const QJsonObject root = QJsonDocument::fromJson(o).object();
    return root.contains(QStringLiteral("State")) ? root.value(QStringLiteral("State")).toObject() : root;
}

// какому выходу принадлежит прямоугольник окна на раскладке: по имени выхода берётся геометрия Qt (логические пиксели)
QRect screenRectByName(const QString &name)
{
    for (const QScreen *s : QGuiApplication::screens()) if (s->name() == name) return s->geometry();
    return {};
}
}

namespace VirtualDisplay {

Backend backend()
{
    static Backend cached = Backend::None;
    static bool known = false;
    if (known) return cached;
    if (!qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")) cached = Backend::Hyprland;
    else if (KWinBridge::isKWin()) cached = Backend::KWin;
    else if (qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains(QLatin1String("driftwm"), Qt::CaseInsensitive)
             || qEnvironmentVariable("XDG_SESSION_DESKTOP").contains(QLatin1String("driftwm"), Qt::CaseInsensitive)) cached = Backend::Driftwm;
    else if (!driftwmState().isEmpty()) cached = Backend::Driftwm;        // IPC driftwm отвечает — значит, это он
    known = true;
    return cached;
}

QString backendName()
{
    switch (backend()) {
    case Backend::Hyprland: return QStringLiteral("Hyprland");
    case Backend::Driftwm: return QStringLiteral("driftwm (бета)");
    case Backend::KWin: return QStringLiteral("KDE Plasma");
    default: return QString();
    }
}

QStringList monitorNames()
{
    QStringList out;
    if (backend() == Backend::KWin) { for (const QScreen *s : QGuiApplication::screens()) out << s->name(); return out; }
    if (backend() != Backend::Hyprland) return out;
    for (const QJsonValue &v : hyprMonitors()) {
        const QString n = v.toObject().value(QStringLiteral("name")).toString();
        if (!n.startsWith(QLatin1String("HEADLESS"))) out << n;
    }
    return out;
}

bool create(const Spec &spec, QString *outName, QSize *outSize, QString *error)
{
    if (backend() != Backend::Hyprland) { if (error) *error = QStringLiteral("виртуальный монитор поддерживает только Hyprland"); return false; }
    // прошлый второй экран ещё снимается с раскладки (destroy убирает его с паузой после остановки захвата) — ждём, потом убираем остатки
    for (const QJsonValue &v : hyprMonitors())
        if (v.toObject().value(QStringLiteral("name")).toString().startsWith(QLatin1String("HEADLESS-ORBIT"))) { QThread::msleep(400); break; }
    cleanupStale();
    QJsonObject main;
    bool anchored = false;
    const QJsonArray before = hyprMonitors();
    for (const QJsonValue &v : before) {
        const QJsonObject m = v.toObject();
        const QString mn = m.value(QStringLiteral("name")).toString();
        if (mn.startsWith(QLatin1String("HEADLESS"))) continue;
        if (!spec.anchor.isEmpty() && mn == spec.anchor) { main = m; anchored = true; }
        else if (!anchored && (main.isEmpty() || m.value(QStringLiteral("focused")).toBool())) main = m;
    }
    if (main.isEmpty()) { if (error) *error = QStringLiteral("не найден основной монитор"); return false; }
    // место и размер задаются правилом ДО создания монитора: без правила Hyprland на мгновение ставил новый выход поверх основного
    const int W = qBound(640, spec.width, 3840), H = qBound(360, spec.height, 3840), F = qBound(24, spec.fps, 120);
    QString pos = QStringLiteral("auto-right");                                  // «auto-*» — вплотную к основному, без наложения
    if (spec.side == QLatin1String("left")) pos = QStringLiteral("auto-left");
    else if (spec.side == QLatin1String("top")) pos = QStringLiteral("auto-up");
    else if (spec.side == QLatin1String("bottom")) pos = QStringLiteral("auto-down");
    // своё имя у каждого второго экрана: удаление прошлого монитора (оно идёт с задержкой, см. destroy) не может снести новый с тем же именем
    static std::atomic<int> seq{0};
    const QString name = QStringLiteral("HEADLESS-ORBIT-%1").arg(++seq);
    // Hyprland принимает только масштаб, при котором размер делится нацело; иначе подставляет свой (1,5 → 2) — выбирается ближайший подходящий
    double useScale = 1.0, bestDiff = 1e9;
    for (double c = 1.0; c <= 3.0001; c += 0.25) {
        if (qAbs(W / c - qRound(W / c)) > 1e-6 || qAbs(H / c - qRound(H / c)) > 1e-6) continue;
        if (qAbs(c - spec.scale) < bestDiff) { bestDiff = qAbs(c - spec.scale); useScale = c; }
    }
    auto rectOf = [](const QJsonObject &m) {
        const double s = qMax(0.1, m.value(QStringLiteral("scale")).toDouble(1.0));
        return QRect(m.value(QStringLiteral("x")).toInt(), m.value(QStringLiteral("y")).toInt(),
                     int(m.value(QStringLiteral("width")).toDouble() / s), int(m.value(QStringLiteral("height")).toDouble() / s));
    };
    // выбран конкретный монитор: новый ставится вплотную к его стороне, если там свободно, иначе Hyprland ставит сам («auto-*»)
    if (anchored) {
        const QRect a = rectOf(main);
        const int lw = int(W / useScale), lh = int(H / useScale);
        QRect cand(a.right() + 1, a.y(), lw, lh);
        if (spec.side == QLatin1String("left")) cand = QRect(a.x() - lw, a.y(), lw, lh);
        else if (spec.side == QLatin1String("top")) cand = QRect(a.x(), a.y() - lh, lw, lh);
        else if (spec.side == QLatin1String("bottom")) cand = QRect(a.x(), a.bottom() + 1, lw, lh);
        bool free = true;
        for (const QJsonValue &v : before) {
            const QJsonObject m = v.toObject();
            if (m.value(QStringLiteral("name")).toString().startsWith(QLatin1String("HEADLESS"))) continue;
            if (rectOf(m).intersects(cand)) free = false;
        }
        if (free) pos = QStringLiteral("%1x%2").arg(cand.x()).arg(cand.y());
    }
    // Куда вернуть пользователя после создания: Hyprland отдаёт новому выходу фокус, курсор и рабочий стол «ID+1» вместе с окнами.
    // Правило hl.workspace_rule для этого не годится: через eval оно не снимается и копится
    QString homeMon = main.value(QStringLiteral("name")).toString();
    int homeWs = 0;
    for (const QJsonValue &v : before) {
        const QJsonObject m = v.toObject();
        if (m.value(QStringLiteral("focused")).toBool() && !m.value(QStringLiteral("name")).toString().startsWith(QLatin1String("HEADLESS"))) {
            homeMon = m.value(QStringLiteral("name")).toString();
            homeWs = m.value(QStringLiteral("activeWorkspace")).toObject().value(QStringLiteral("id")).toInt();
        }
    }
    QPoint homeCursor(-1, -1);
    {
        const QJsonObject c = QJsonDocument::fromJson(hyprJson("cursorpos")).object();
        if (c.contains(QStringLiteral("x"))) homeCursor = QPoint(c.value(QStringLiteral("x")).toInt(), c.value(QStringLiteral("y")).toInt());
    }
    bool wsNum = false;
    const int wsId = spec.workspace.toInt(&wsNum);
    const bool numberedWs = wsNum && wsId >= 1 && wsId <= 99;
    const QString lua = QStringLiteral("hl.monitor({ output = \"%1\", mode = \"%2x%3@%4\", position = \"%5\", scale = %6 })").arg(name).arg(W).arg(H).arg(F).arg(pos, QString::number(useScale, 'f', 2));
    if (!run(QStringLiteral("hyprctl"), {QStringLiteral("eval"), lua})) { if (error) *error = QStringLiteral("не удалось настроить виртуальный монитор"); return false; }
    if (!run(QStringLiteral("hyprctl"), {QStringLiteral("output"), QStringLiteral("create"), QStringLiteral("headless"), name})) {
        if (error) *error = QStringLiteral("hyprctl не создал виртуальный монитор");
        return false;
    }
    remember(name);                                                          // с этой минуты монитор есть: при падении Orbit его уберёт страж
    guard();
    QJsonObject mon;
    for (int attempt = 0; attempt < 20 && mon.isEmpty(); ++attempt) {        // выход появляется не мгновенно
        mon = monitorByName(name);
        if (mon.isEmpty()) QThread::msleep(60);
    }
    if (mon.isEmpty()) { outputRemove(name); remember(QString()); if (error) *error = QStringLiteral("виртуальный монитор не появился"); return false; }
    // Проверка размера: Hyprland иногда создаёт выход с режимом по умолчанию (правило применилось позже, режим не принят) —
    // тогда кадр не совпадал с экраном зрителя и второй экран занимал его не целиком. Правило применяется повторно
    auto pixelSize = [](const QJsonObject &m) { return QSize(m.value(QStringLiteral("width")).toInt(), m.value(QStringLiteral("height")).toInt()); };
    for (int attempt = 0; attempt < 3 && pixelSize(mon) != QSize(W, H); ++attempt) {
        run(QStringLiteral("hyprctl"), {QStringLiteral("eval"), lua});
        QThread::msleep(150);
        const QJsonObject again = monitorByName(name);
        if (!again.isEmpty()) mon = again;
    }
    const QSize got = pixelSize(mon);
    if (outSize) *outSize = got.isValid() && !got.isEmpty() ? got : QSize(W, H);
    // раскладка столов: на виртуальном мониторе — только выбранный стол (или отдельный «orbit»), остальные остаются на основном
    {
        const int gotWs = mon.value(QStringLiteral("activeWorkspace")).toObject().value(QStringLiteral("id")).toInt();      // стол, который Hyprland выдал новому выходу сам
        const bool keep = numberedWs && gotWs == wsId;
        if (!keep && gotWs > 0)                                              // чужой стол с окнами — обратно на основной монитор
            dispatch(QStringLiteral("hl.dsp.workspace.move({ workspace = %1, monitor = \"%2\" })").arg(gotWs).arg(homeMon));
        if (!keep) {
            dispatch(QStringLiteral("hl.dsp.focus({ monitor = \"%1\" })").arg(name));          // возврат чужого стола увёл фокус на основной
            bool exists = false;                                              // стол уже есть (с окнами) на другом мониторе — переносим, иначе создаём на этом
            if (numberedWs)
                for (const QJsonValue &w : QJsonDocument::fromJson(hyprJson("workspaces")).array()) if (w.toObject().value(QStringLiteral("id")).toInt() == wsId) exists = true;
            if (numberedWs && exists) dispatch(QStringLiteral("hl.dsp.workspace.move({ workspace = %1, monitor = \"%2\" })").arg(wsId).arg(name));
            else dispatch(QStringLiteral("hl.dsp.focus({ workspace = %1 })").arg(numberedWs ? QString::number(wsId) : QStringLiteral("\"name:orbit\"")));
        }
        // проверка: когда нужный стол переносят или он пустеет, Hyprland подставляет монитору следующий свободный номер — выбирается нужный заново
        const QString wantName = numberedWs ? QString::number(wsId) : QStringLiteral("orbit");
        for (int attempt = 0; attempt < 4; ++attempt) {
            const QString haveName = monitorByName(name).value(QStringLiteral("activeWorkspace")).toObject().value(QStringLiteral("name")).toString();
            if (haveName == wantName) break;
            QThread::msleep(120);
            dispatch(QStringLiteral("hl.dsp.focus({ monitor = \"%1\" })").arg(name));
            dispatch(QStringLiteral("hl.dsp.focus({ workspace = %1 })").arg(numberedWs ? QString::number(wsId) : QStringLiteral("\"name:orbit\"")));
        }
        // пользователя — на прежнее место: монитор, стол, курсор
        dispatch(QStringLiteral("hl.dsp.focus({ monitor = \"%1\" })").arg(homeMon));
        if (homeWs > 0 && homeWs != wsId) dispatch(QStringLiteral("hl.dsp.focus({ workspace = %1 })").arg(homeWs));
        if (homeCursor.x() >= 0) dispatch(QStringLiteral("hl.dsp.cursor.move({ x = %1, y = %2 })").arg(homeCursor.x()).arg(homeCursor.y()));
    }
    // обои на новом мониторе: демон обоев сам их не рисует до следующей смены — ставятся текущие
    {
        QFile wf(QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + QStringLiteral("/.local/state/hypr-theme/wallpaper"));
        const QString img = wf.open(QIODevice::ReadOnly) ? QString::fromUtf8(wf.readAll()).trimmed() : QString();
        if (!img.isEmpty() && QFile::exists(img)) {
            QThread::msleep(300);                                   // демон обоев замечает новый выход не сразу
            if (!run(QStringLiteral("awww"), {QStringLiteral("img"), img, QStringLiteral("--outputs"), name, QStringLiteral("--transition-type"), QStringLiteral("none")}))
                run(QStringLiteral("hyprctl"), {QStringLiteral("hyprpaper"), QStringLiteral("wallpaper"), name + QLatin1Char(',') + img});
        }
    }
    *outName = name;
    return true;
}

void keepWorkspace(const QString &outName, const QString &workspace)
{
    if (outName.isEmpty()) return;
    const QString want = workspace.isEmpty() ? QStringLiteral("orbit") : workspace;
    QString have, home;
    int haveId = 0;
    for (const QJsonValue &v : hyprMonitors()) {
        const QJsonObject m = v.toObject();
        const QString n = m.value(QStringLiteral("name")).toString();
        if (n == outName) {
            // пользователь сейчас работает на втором экране (фокус на нём): столы не трогаются — раньше Orbit «отбирал» стол, едва его открывали,
            // и экраны перекидывало туда-обратно
            if (m.value(QStringLiteral("focused")).toBool()) return;
            const QJsonObject w = m.value(QStringLiteral("activeWorkspace")).toObject();
            have = w.value(QStringLiteral("name")).toString();
            haveId = w.value(QStringLiteral("id")).toInt();
        } else if (!n.startsWith(QLatin1String("HEADLESS")) && (home.isEmpty() || m.value(QStringLiteral("focused")).toBool())) home = n;
    }
    if (have.isEmpty() || have == want || home.isEmpty()) return;
    // на виртуальный монитор попал чужой стол (окно открылось по правилу) — он возвращается на настоящий монитор, выбранный стол встаёт на место
    if (haveId > 0) dispatch(QStringLiteral("hl.dsp.workspace.move({ workspace = %1, monitor = \"%2\" })").arg(haveId).arg(home));
    bool exists = false;
    for (const QJsonValue &w : QJsonDocument::fromJson(hyprJson("workspaces")).array()) if (w.toObject().value(QStringLiteral("name")).toString() == want) exists = true;
    if (exists) dispatch(QStringLiteral("hl.dsp.workspace.move({ workspace = \"%1\", monitor = \"%2\" })").arg(want, outName));
}

void destroy(const QString &outName, bool now)
{
    if (outName.isEmpty()) return;
    // Монитор убирается с паузой 0,8 с: NameLess UI получил сигнал VirtualScreen «closing» и убирает с него свои окна (иначе Quickshell
    // падал при удалении монитора с окном), а wf-recorder только что убит, и Hyprland ещё держит его кадр этого выхода —
    // удаление выхода с незавершённым захватом роняло или подвешивало композитор. Отдельным процессом: окно Orbit не ждёт пересборки мониторов
    const QString script = now ? QStringLiteral("hyprctl output remove \"$1\" >/dev/null 2>&1")
                               : QStringLiteral("sleep 0.8; hyprctl output remove \"$1\" >/dev/null 2>&1");
    QProcess::startDetached(QStringLiteral("sh"), {QStringLiteral("-c"), script, QStringLiteral("orbit-remove"), outName});
    QFile f(stateFile());
    if (f.open(QIODevice::ReadOnly) && QString::fromUtf8(f.readAll()).trimmed() == outName) { f.close(); remember(QString()); }
}

void cleanupStale()
{
    if (backend() != Backend::Hyprland) return;
    QFile f(stateFile());
    if (f.open(QIODevice::ReadOnly)) {
        const QString name = QString::fromUtf8(f.readAll()).trimmed();
        f.close();
        if (name.startsWith(QLatin1String("HEADLESS"))) outputRemove(name);
        f.remove();
    }
    // мониторы Orbit, оставшиеся без метки (старые версии с общим именем HEADLESS-ORBIT, падение до записи метки)
    for (const QJsonValue &v : hyprMonitors()) {
        const QString n = v.toObject().value(QStringLiteral("name")).toString();
        if (n.startsWith(QLatin1String("HEADLESS-ORBIT"))) outputRemove(n);
    }
}

QRect outputRect(const QString &outName)
{
    for (const QJsonValue &v : hyprMonitors()) {
        const QJsonObject m = v.toObject();
        if (m.value(QStringLiteral("name")).toString() != outName) continue;
        const double s = qMax(0.1, m.value(QStringLiteral("scale")).toDouble(1.0));
        return QRect(m.value(QStringLiteral("x")).toInt(), m.value(QStringLiteral("y")).toInt(),
                     int(m.value(QStringLiteral("width")).toDouble() / s), int(m.value(QStringLiteral("height")).toDouble() / s));
    }
    return {};
}

QVariantList driftwmWindows()
{
    QVariantList out;
    const QJsonObject st = driftwmState();
    for (const QJsonValue &v : st.value(QStringLiteral("windows")).toArray()) {
        const QJsonObject w = v.toObject();
        if (w.value(QStringLiteral("is_widget")).toBool() || w.value(QStringLiteral("suspended")).toBool()) continue;
        const QString app = w.value(QStringLiteral("app_id")).toString(), title = w.value(QStringLiteral("title")).toString();
        if (app == QLatin1String("nameless-orbit")) continue;
        QString label = app.isEmpty() ? title : (title.isEmpty() || title == app ? app : app + QStringLiteral(" — ") + title);
        if (label.size() > 52) label = label.left(51) + QChar(0x2026);
        QVariantMap m;
        m[QStringLiteral("id")] = w.value(QStringLiteral("id")).toInt();
        m[QStringLiteral("label")] = label;
        out.append(m);
    }
    return out;
}

// Окно на раскладке: центр окна в канве (Y вверх) → пиксели выхода: x = W/2 + (cx − камера_x)·zoom, y = H/2 − (cy − камера_y)·zoom;
// выход берётся тот, что показывает окно (его обзор содержит центр). Размеры — в пикселях канвы, умножаются на zoom
static bool locate(const QJsonObject &st, int id, QRect *rect, QString *output)
{
    QJsonObject win;
    for (const QJsonValue &v : st.value(QStringLiteral("windows")).toArray()) if (v.toObject().value(QStringLiteral("id")).toInt() == id) win = v.toObject();
    if (win.isEmpty()) return false;
    const QJsonArray pos = win.value(QStringLiteral("position")).toArray(), sz = win.value(QStringLiteral("size")).toArray();
    if (pos.size() < 2 || sz.size() < 2) return false;
    const double cx = pos[0].toDouble(), cy = pos[1].toDouble(), ww = sz[0].toDouble(), wh = sz[1].toDouble();
    for (const QJsonValue &ov : st.value(QStringLiteral("outputs")).toArray()) {
        const QJsonObject o = ov.toObject();
        const QJsonArray cam = o.value(QStringLiteral("camera")).toArray(), osz = o.value(QStringLiteral("size")).toArray();
        const double zoom = qMax(0.05, o.value(QStringLiteral("zoom")).toDouble(1.0));
        if (cam.size() < 2 || osz.size() < 2) continue;
        const double W = osz[0].toDouble(), H = osz[1].toDouble();
        const double sx = W / 2 + (cx - cam[0].toDouble()) * zoom, sy = H / 2 - (cy - cam[1].toDouble()) * zoom;
        const double pw = ww * zoom, ph = wh * zoom;
        if (sx < 0 || sx > W || sy < 0 || sy > H) continue;                  // центр окна вне обзора этого выхода
        const QString name = o.value(QStringLiteral("name")).toString();
        const QRect origin = screenRectByName(name);
        if (origin.isEmpty()) continue;
        QRect r(int(sx - pw / 2), int(sy - ph / 2), int(pw), int(ph));
        r = r.intersected(QRect(0, 0, int(W), int(H)));                      // видимая часть
        if (r.width() < 16 || r.height() < 16) continue;
        *rect = r.translated(origin.topLeft());
        if (output) *output = name;
        return true;
    }
    return false;
}

bool driftwmWindowRect(int windowId, QRect *rect)
{
    return locate(driftwmState(), windowId, rect, nullptr);
}

bool driftwmPrepare(int windowId, int width, int height, QRect *rect, QString *outputName, QString *error)
{
    QJsonObject st = driftwmState();
    if (st.isEmpty()) { if (error) *error = QStringLiteral("driftwm не отвечает на driftwm msg"); return false; }
    // выход с фокусом (иначе первый) — на нём окно и будет стоять; его камера — центр обзора
    QJsonObject out;
    for (const QJsonValue &ov : st.value(QStringLiteral("outputs")).toArray()) {
        const QJsonObject o = ov.toObject();
        if (out.isEmpty() || o.value(QStringLiteral("active")).toBool()) out = o;
    }
    const QJsonArray cam = out.value(QStringLiteral("camera")).toArray();
    if (cam.size() < 2) { if (error) *error = QStringLiteral("нет данных о камере driftwm"); return false; }
    const QString id = QString::number(windowId);
    // размер окна — в канве; на выходе он умножится на zoom, поэтому для точного размера кадра делим на zoom
    const double zoom = qMax(0.05, out.value(QStringLiteral("zoom")).toDouble(1.0));
    run(QStringLiteral("driftwm"), {QStringLiteral("msg"), QStringLiteral("resize"), QStringLiteral("--id"), id,
                                    QString::number(int(width / zoom)), QString::number(int(height / zoom))});
    run(QStringLiteral("driftwm"), {QStringLiteral("msg"), QStringLiteral("move"), QStringLiteral("--id"), id,
                                    QString::number(cam[0].toDouble(), 'f', 1), QString::number(cam[1].toDouble(), 'f', 1)});
    QThread::msleep(250);                                                    // окну нужно время принять новый размер
    if (!locate(driftwmState(), windowId, rect, outputName)) { if (error) *error = QStringLiteral("окно не удалось показать на экране"); return false; }
    return true;
}

}

#else   // не настольный Linux: второй экран недоступен

namespace VirtualDisplay {
Backend backend() { return Backend::None; }
QStringList monitorNames() { return {}; }
QString backendName() { return QString(); }
bool create(const Spec &, QString *, QSize *, QString *error) { if (error) *error = QStringLiteral("второй экран есть только на Linux"); return false; }
void destroy(const QString &, bool) {}
void keepWorkspace(const QString &, const QString &) {}
void cleanupStale() {}
QRect outputRect(const QString &) { return {}; }
QVariantList driftwmWindows() { return {}; }
bool driftwmPrepare(int, int, int, QRect *, QString *, QString *error) { if (error) *error = QStringLiteral("второй экран есть только на Linux"); return false; }
bool driftwmWindowRect(int, QRect *) { return false; }
}

#endif
