#include "CommandStore.h"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>
#include <QVariantMap>
#include <QSet>
#include <QSettings>

QString CommandStore::storePath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + QStringLiteral("/commands.json");
}

CommandStore::CommandStore(QObject *parent) : QObject(parent)
{
    load();
    seedDefaults();
}

namespace {
struct Std { const char *name, *icon, *command; bool confirm; };

// Стандартный набор по платформам. Команды для другого устройства выполняются у НЕГО через /bin/sh; без нужной программы просто ничего не произойдёт
#if defined(ORBIT_DESKTOP_LINUX)
const Std kStd[] = {
    {"Заблокировать экран", "shield", "loginctl lock-session", false},
    {"Спящий режим", "stop", "systemctl suspend", false},
    {"Пауза / воспроизведение", "play", "playerctl play-pause", false},
    {"Следующий трек", "play", "playerctl next", false},
    {"Предыдущий трек", "play", "playerctl previous", false},
    {"Громче", "speaker", "pactl set-sink-volume @DEFAULT_SINK@ +10%", false},
    {"Тише", "speaker", "pactl set-sink-volume @DEFAULT_SINK@ -10%", false},
    {"Без звука", "speaker", "pactl set-sink-mute @DEFAULT_SINK@ toggle", false},
    {"Ярче экран", "monitor", "brightnessctl set +10%", false},
    {"Темнее экран", "monitor", "brightnessctl set 10%-", false},
    {"Скриншот", "monitor", "mkdir -p ~/Pictures && f=~/Pictures/orbit-$(date +%F_%H-%M-%S).png && grim \"$f\" && notify-send Orbit \"Скриншот: $f\"", false},
    {"Выключить экран", "monitor", "sleep 0.5; hyprctl dispatch dpms off", false},
    {"Перезагрузить", "refresh", "systemctl reboot", true},
    {"Выключить компьютер", "stop", "systemctl poweroff", true},
};
#elif defined(Q_OS_ANDROID)
const Std kStd[] = {
    {"Фонарик", "bolt", "orbit:flashlight", false},      // встроенное действие телефона
};
#elif defined(Q_OS_WIN)
const Std kStd[] = {
    {"Заблокировать экран", "shield", "rundll32.exe user32.dll,LockWorkStation", false},
    {"Спящий режим", "stop", "rundll32.exe powrprof.dll,SetSuspendState 0,1,0", true},
    {"Перезагрузить", "refresh", "shutdown /r /t 5", true},
    {"Выключить компьютер", "stop", "shutdown /s /t 5", true},
};
#elif defined(Q_OS_MACOS)
const Std kStd[] = {
    {"Заблокировать экран", "shield", "/System/Library/CoreServices/Menu\\ Extras/User.menu/Contents/Resources/CGSession -suspend", false},
    {"Спящий режим", "stop", "pmset sleepnow", false},
    {"Скриншот", "monitor", "screencapture -x ~/Desktop/orbit-$(date +%F_%H-%M-%S).png", false},
};
#else
const Std kStd[] = {};
#endif
constexpr int kStdVersion = 2;       // растёт, когда в набор добавляются команды: у уже работающих установок недостающие подмешиваются один раз
}

// Добавляет стандартные команды, которых ещё нет (по названию). Один раз на версию набора: удалённую человеком команду потом не возвращаем
void CommandStore::seedDefaults()
{
    QSettings st;
    if (st.value(QStringLiteral("commandsSeed"), 0).toInt() >= kStdVersion) return;
    QSet<QString> have;
    for (const OrbitCommand &c : m_commands) have.insert(c.name.toLower());
    bool added = false;
    for (const Std &s : kStd) {
        const QString name = QString::fromUtf8(s.name);
        if (have.contains(name.toLower())) continue;
        m_commands.append({QUuid::createUuid().toString(QUuid::WithoutBraces), name, QString::fromUtf8(s.icon), QString::fromUtf8(s.command), s.confirm});
        added = true;
    }
    st.setValue(QStringLiteral("commandsSeed"), kStdVersion);
    if (added) save();
}

void CommandStore::load()
{
    QFile f(storePath());
    if (!f.open(QIODevice::ReadOnly)) return;
    for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).array()) {
        const QJsonObject o = v.toObject();
        OrbitCommand c;
        c.id = o.value(QStringLiteral("id")).toString();
        c.name = o.value(QStringLiteral("name")).toString();
        c.icon = o.value(QStringLiteral("icon")).toString();
        c.command = o.value(QStringLiteral("command")).toString();
        c.confirmLocally = o.value(QStringLiteral("confirm")).toBool();
        if (!c.id.isEmpty() && !c.command.isEmpty()) m_commands.append(c);
    }
}

void CommandStore::save() const
{
    QJsonArray arr;
    for (const OrbitCommand &c : m_commands) {
        QJsonObject o;
        o[QStringLiteral("id")] = c.id;
        o[QStringLiteral("name")] = c.name;
        o[QStringLiteral("icon")] = c.icon;
        o[QStringLiteral("command")] = c.command;
        o[QStringLiteral("confirm")] = c.confirmLocally;
        arr.append(o);
    }
    QFile f(storePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    }
}

const OrbitCommand *CommandStore::find(const QString &id) const
{
    for (const OrbitCommand &c : m_commands) if (c.id == id) return &c;
    return nullptr;
}

// Значки в интерфейсе — имена из Icon.qml. Команды, сохранённые прежними версиями, несли символы шрифта (их не нарисовать), а
// пустой значок оставлял кнопку без картинки: подбираем по смыслу команды, иначе общий «молния»
QString CommandStore::displayIcon(const OrbitCommand &c)
{
    static const QSet<QString> known = {
        QStringLiteral("shield"), QStringLiteral("stop"), QStringLiteral("bolt"), QStringLiteral("monitor"), QStringLiteral("bell"),
        QStringLiteral("plug"), QStringLiteral("find"), QStringLiteral("clipboard"), QStringLiteral("file"), QStringLiteral("send"),
        QStringLiteral("download"), QStringLiteral("upload"), QStringLiteral("speaker"), QStringLiteral("link"), QStringLiteral("settings"),
        QStringLiteral("refresh"), QStringLiteral("wifi"), QStringLiteral("play"), QStringLiteral("pause"), QStringLiteral("cursor"),
        QStringLiteral("phone"), QStringLiteral("laptop"), QStringLiteral("desktop"), QStringLiteral("server"), QStringLiteral("battery"), QStringLiteral("trash")};
    if (known.contains(c.icon)) return c.icon;
    const QString cmd = c.command.toLower(), name = c.name.toLower();
    auto has = [&](const char *w) { return cmd.contains(QLatin1String(w)) || name.contains(QLatin1String(w)); };
    if (has("lock") || has("блок")) return QStringLiteral("shield");
    if (has("suspend") || has("sleep") || has("poweroff") || has("shutdown") || has("reboot") || has("сон") || has("спящ") || has("выключ")) return QStringLiteral("stop");
    if (has("screenshot") || has("скрин") || has("grim")) return QStringLiteral("monitor");
    if (has("volume") || has("громк") || has("звук") || has("pactl") || has("wpctl")) return QStringLiteral("speaker");
    if (has("notify") || has("уведом")) return QStringLiteral("bell");
    if (has("wifi") || has("nmcli")) return QStringLiteral("wifi");
    return QStringLiteral("bolt");
}

QString CommandStore::commandText(const QString &id) const
{
    const OrbitCommand *c = find(id);
    return c ? c->command : QString();
}

QVariantList CommandStore::commandsForQml() const
{
    QVariantList out;
    for (const OrbitCommand &c : m_commands) {
        QVariantMap m;
        m[QStringLiteral("id")] = c.id;
        m[QStringLiteral("name")] = c.name;
        m[QStringLiteral("icon")] = displayIcon(c);
        m[QStringLiteral("confirm")] = c.confirmLocally;
        // поле command намеренно не кладём — редактор локальных команд читает его отдельным путём, не через это свойство
        out.append(m);
    }
    return out;
}

void CommandStore::addCommand(const QString &name, const QString &icon, const QString &command, bool confirm)
{
    if (name.trimmed().isEmpty() || command.trimmed().isEmpty()) return;
    m_commands.append({QUuid::createUuid().toString(QUuid::WithoutBraces), name.trimmed().left(40), icon, command, confirm});
    save();
    emit changed();
}

void CommandStore::updateCommand(const QString &id, const QString &name, const QString &icon, const QString &command, bool confirm)
{
    for (OrbitCommand &c : m_commands) {
        if (c.id != id) continue;
        if (!name.trimmed().isEmpty()) c.name = name.trimmed().left(40);
        c.icon = icon;
        if (!command.trimmed().isEmpty()) c.command = command;
        c.confirmLocally = confirm;
        save();
        emit changed();
        return;
    }
}

void CommandStore::removeCommand(const QString &id)
{
    const int before = m_commands.size();
    m_commands.removeIf([&id](const OrbitCommand &c) { return c.id == id; });
    if (m_commands.size() != before) { save(); emit changed(); }
}
