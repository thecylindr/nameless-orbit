#include "VirtualDisplay.h"
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
#include <QVariantMap>

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

QJsonArray hyprMonitors()
{
    QByteArray o;
    if (!run(QStringLiteral("hyprctl"), {QStringLiteral("-j"), QStringLiteral("monitors")}, &o)) return {};
    return QJsonDocument::fromJson(o).array();
}

QString stateFile() { return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/secondscreen.json"); }

void remember(const QString &name)
{
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    QFile f(stateFile());
    if (name.isEmpty()) { f.remove(); return; }
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(name.toUtf8());
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
    default: return QString();
    }
}

QStringList monitorNames()
{
    QStringList out;
    if (backend() != Backend::Hyprland) return out;
    for (const QJsonValue &v : hyprMonitors()) {
        const QString n = v.toObject().value(QStringLiteral("name")).toString();
        if (!n.startsWith(QLatin1String("HEADLESS"))) out << n;
    }
    return out;
}

bool create(const Spec &spec, QString *outName, QString *error)
{
    if (backend() != Backend::Hyprland) { if (error) *error = QStringLiteral("виртуальный монитор поддерживает только Hyprland"); return false; }
    cleanupStale();
    QJsonObject main;
    bool anchored = false;
    for (const QJsonValue &v : hyprMonitors()) {
        const QJsonObject m = v.toObject();
        const QString mn = m.value(QStringLiteral("name")).toString();
        if (mn.startsWith(QLatin1String("HEADLESS"))) continue;
        if (!spec.anchor.isEmpty() && mn == spec.anchor) { main = m; anchored = true; }
        else if (!anchored && (main.isEmpty() || m.value(QStringLiteral("focused")).toBool())) main = m;
    }
    if (main.isEmpty()) { if (error) *error = QStringLiteral("не найден основной монитор"); return false; }
    // место и размер задаются правилом ДО создания монитора, а имя выбирается самим: без правила Hyprland на мгновение ставил новый выход
    // поверх основного и показывал ошибку «монитор перекрывает другие»
    const double scale = qMax(0.1, main.value(QStringLiteral("scale")).toDouble(1.0));
    Q_UNUSED(scale);
    const int W = qBound(640, spec.width, 3840), H = qBound(360, spec.height, 2400), F = qBound(24, spec.fps, 120);
    QString pos = QStringLiteral("auto-right");                                  // «auto-*» — вплотную к основному, без наложения
    if (spec.side == QLatin1String("left")) pos = QStringLiteral("auto-left");
    else if (spec.side == QLatin1String("top")) pos = QStringLiteral("auto-up");
    else if (spec.side == QLatin1String("bottom")) pos = QStringLiteral("auto-down");
    const QString name = QStringLiteral("HEADLESS-ORBIT");
    run(QStringLiteral("hyprctl"), {QStringLiteral("output"), QStringLiteral("remove"), name});      // остаток прошлого запуска, если он есть
    // Hyprland принимает только масштаб, при котором размер делится нацело; иначе подставляет свой (1,5 → 2) — выбирается ближайший подходящий
    double useScale = 1.0, bestDiff = 1e9;
    for (double c = 1.0; c <= 3.0001; c += 0.25) {
        if (qAbs(W / c - qRound(W / c)) > 1e-6 || qAbs(H / c - qRound(H / c)) > 1e-6) continue;
        if (qAbs(c - spec.scale) < bestDiff) { bestDiff = qAbs(c - spec.scale); useScale = c; }
    }
    // выбран конкретный монитор: новый ставится вплотную к его стороне, если там свободно, иначе Hyprland ставит сам («auto-*»)
    if (anchored) {
        auto rectOf = [](const QJsonObject &m) {
            const double s = qMax(0.1, m.value(QStringLiteral("scale")).toDouble(1.0));
            return QRect(m.value(QStringLiteral("x")).toInt(), m.value(QStringLiteral("y")).toInt(),
                         int(m.value(QStringLiteral("width")).toDouble() / s), int(m.value(QStringLiteral("height")).toDouble() / s));
        };
        const QRect a = rectOf(main);
        const int lw = int(W / useScale), lh = int(H / useScale);
        QRect cand(a.right() + 1, a.y(), lw, lh);
        if (spec.side == QLatin1String("left")) cand = QRect(a.x() - lw, a.y(), lw, lh);
        else if (spec.side == QLatin1String("top")) cand = QRect(a.x(), a.y() - lh, lw, lh);
        else if (spec.side == QLatin1String("bottom")) cand = QRect(a.x(), a.bottom() + 1, lw, lh);
        bool free = true;
        for (const QJsonValue &v : hyprMonitors()) {
            const QJsonObject m = v.toObject();
            if (m.value(QStringLiteral("name")).toString().startsWith(QLatin1String("HEADLESS"))) continue;
            if (rectOf(m).intersects(cand)) free = false;
        }
        if (free) pos = QStringLiteral("%1x%2").arg(cand.x()).arg(cand.y());
    }
    // рабочий стол нового монитора назначается заранее: иначе Hyprland отдаёт ему первый свободный номер, и переключение на этот номер (Super+цифра)
    // уводило курсор и фокус на виртуальный монитор. Отдельный «orbit» с номерами 1–10 не пересекается
    bool wsNum = false;
    const int wsId = spec.workspace.toInt(&wsNum);
    const QString ws = (wsNum && wsId >= 1 && wsId <= 99) ? QString::number(wsId) : QStringLiteral("name:orbit");
    run(QStringLiteral("hyprctl"), {QStringLiteral("eval"), QStringLiteral("hl.workspace_rule({ workspace = \"%1\", monitor = \"%2\", default = true })").arg(ws, name)});
    const QString lua = QStringLiteral("hl.monitor({ output = \"%1\", mode = \"%2x%3@%4\", position = \"%5\", scale = %6 })").arg(name).arg(W).arg(H).arg(F).arg(pos, QString::number(useScale, 'f', 2));
    if (!run(QStringLiteral("hyprctl"), {QStringLiteral("eval"), lua})) { if (error) *error = QStringLiteral("не удалось настроить виртуальный монитор"); return false; }
    if (!run(QStringLiteral("hyprctl"), {QStringLiteral("output"), QStringLiteral("create"), QStringLiteral("headless"), name})) {
        if (error) *error = QStringLiteral("hyprctl не создал виртуальный монитор");
        return false;
    }
    bool seen = false;
    for (int attempt = 0; attempt < 12 && !seen; ++attempt) {                    // выход появляется не мгновенно
        for (const QJsonValue &v : hyprMonitors()) if (v.toObject().value(QStringLiteral("name")).toString() == name) seen = true;
        if (!seen) QThread::msleep(100);
    }
    if (!seen) { if (error) *error = QStringLiteral("виртуальный монитор не появился"); return false; }
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
    remember(name);
    *outName = name;
    return true;
}

void destroy(const QString &outName)
{
    if (outName.isEmpty()) return;
    // без ожидания: hyprctl мог отвечать секундами, пока Hyprland пересобирает мониторы, — окно Orbit в это время подвисало
    QProcess::startDetached(QStringLiteral("hyprctl"), {QStringLiteral("output"), QStringLiteral("remove"), outName});
    remember(QString());
}

void cleanupStale()
{
    QFile f(stateFile());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QString name = QString::fromUtf8(f.readAll()).trimmed();
    f.close();
    if (name.startsWith(QLatin1String("HEADLESS")) && backend() == Backend::Hyprland) run(QStringLiteral("hyprctl"), {QStringLiteral("output"), QStringLiteral("remove"), name});
    f.remove();
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
bool create(const Spec &, QString *, QString *error) { if (error) *error = QStringLiteral("второй экран есть только на Linux"); return false; }
void destroy(const QString &) {}
void cleanupStale() {}
QRect outputRect(const QString &) { return {}; }
QVariantList driftwmWindows() { return {}; }
bool driftwmPrepare(int, int, int, QRect *, QString *, QString *error) { if (error) *error = QStringLiteral("второй экран есть только на Linux"); return false; }
bool driftwmWindowRect(int, QRect *) { return false; }
}

#endif
