#include "CodexNotify.h"
#include "SingleInstance.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

int runCodexNotify(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 3) return 2;
    const QByteArray input(argv[2]);
    if (input.size() > 1024 * 1024) return 2;
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(input, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return 2;
    const auto event = doc.object();
    if (event.value(QStringLiteral("type")).toString() != QLatin1String("agent-turn-complete")) return 0;
    // Never forward user prompts, paths, tokens, or the complete transcript.
    QJsonObject command;
    command[QStringLiteral("command")] = QStringLiteral("codexReply");
    command[QStringLiteral("body")] = event.value(QStringLiteral("last-assistant-message")).toString().left(600);
    const QString name = QStringLiteral("nameless-orbit-") +
        qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME", QStringLiteral("user")));
    QByteArray reply;
    if (!SingleInstance::ask(name, QJsonDocument(command).toJson(QJsonDocument::Compact), &reply, 1500)) {
        qWarning() << "Orbit is not running; Codex notification was not delivered.";
        return 1;
    }
    return QJsonDocument::fromJson(reply).object().value(QStringLiteral("ok")).toBool() ? 0 : 1;
}
