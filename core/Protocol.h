#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <QByteArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QString>

// Общий формат сообщений внутри защищённого канала: одна строка — один JSON-объект с полем "type".
// Список типов — здесь, чтобы не разойтись между Peer (транспорт) и плагинами (смысл).
namespace Protocol {

// системные, идут поверх уже установленного SecureChannel (не во время рукопожатия)
constexpr auto TYPE_PING = "ping";
constexpr auto TYPE_PONG = "pong";
constexpr auto TYPE_CAPABILITIES = "capabilities";     // список плагинов, включённых на этой стороне
constexpr auto TYPE_PLUGIN = "plugin";                 // полезная нагрузка плагина: {type:"plugin", plugin:"...", ...}
constexpr auto TYPE_INTRO = "intro";                   // знакомство до доверия: имена быстрых команд (без текста), чтобы человек видел, с чем соглашается
constexpr auto TYPE_PAIR = "pair";                     // решение о сопряжении: {type:"pair", ok:true|false}; ходит до того, как устройство стало доверенным
constexpr auto TYPE_BYE = "bye";                       // вежливое закрытие с причиной (в отличие от обрыва сети)

inline QByteArray encode(const QJsonObject &obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
}

inline QJsonObject decode(const QByteArray &line, bool *ok = nullptr)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (ok) *ok = (err.error == QJsonParseError::NoError && doc.isObject());
    return doc.isObject() ? doc.object() : QJsonObject();
}

// обёртка плагинового сообщения: {"type":"plugin","plugin":"battery","payload":{...}}
inline QJsonObject pluginEnvelope(const QString &plugin, const QJsonObject &payload)
{
    QJsonObject o;
    o[QStringLiteral("type")] = QString::fromLatin1(TYPE_PLUGIN);
    o[QStringLiteral("plugin")] = plugin;
    o[QStringLiteral("payload")] = payload;
    return o;
}

} // namespace Protocol

#endif // PROTOCOL_H
