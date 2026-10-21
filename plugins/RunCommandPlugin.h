#ifndef RUNCOMMANDPLUGIN_H
#define RUNCOMMANDPLUGIN_H

#include "PluginBase.h"
#include <QHash>
#include <QVariantList>

class CommandStore;

// Запуск заранее заданных команд по просьбе доверенного устройства — аналог плагина runcommand в KDE Connect,
// но без произвольного текста по сети: удалённая сторона посылает только id, а что реально выполнится,
// решает исключительно CommandStore на ЭТОЙ машине (список редактируется локально, видно в DeviceDetailPage).
// Если у команды стоит «подтверждать здесь» — перед запуском пять секунд ждёт локального подтверждения.
class RunCommandPlugin : public PluginBase
{
    Q_OBJECT

public:
    RunCommandPlugin(PeerManager *manager, CommandStore *store, QObject *parent = nullptr);

    // список команд (id/название/значок — без текста) для показа тому, кто подключился
    Q_INVOKABLE void sendAvailableCommands(const QString &deviceId);   // отдать СВОЙ список (название и значок, без текста команд)
    Q_INVOKABLE void requestCommands(const QString &deviceId);          // попросить список ТОГО устройства — придёт через remoteCommandsUpdated
    Q_INVOKABLE QVariantList remoteCommands(const QString &deviceId) const { return m_remote.value(deviceId); }   // последний полученный список
    Q_INVOKABLE void request(const QString &deviceId, const QString &commandId);   // попросить ТО устройство выполнить
    Q_INVOKABLE void confirmPending(const QString &requestId, bool run);           // ответ на локальное «точно выполнить?»

    void handleMessage(const QString &deviceId, const QJsonObject &payload) override;
    bool needsPermission(const QJsonObject &p) const override { return p.value(QStringLiteral("kind")).toString() != QLatin1String("list"); }

signals:
    void remoteCommandsUpdated(const QString &deviceId, const QVariantList &commands);
    void confirmationNeeded(const QString &requestId, const QString &name);   // показать диалог на 5 с
    void executed(const QString &commandId, bool ok);

private:
    void execute(const QString &commandId);

    CommandStore *m_store;
    QHash<QString, QVariantList> m_remote;     // списки команд других устройств (название, значок, id)
    struct Pending { QString commandId; };
    QHash<QString, Pending> m_pendingConfirm;
};

#endif // RUNCOMMANDPLUGIN_H
