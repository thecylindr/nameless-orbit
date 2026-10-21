#ifndef CLIPBOARDPLUGIN_H
#define CLIPBOARDPLUGIN_H

#include "PluginBase.h"

// Буфер обмена — по явному нажатию в обе стороны (не постоянная синхронизация, как у KDE Connect):
// пароли и прочее чувствительное копируют многие менеджеры паролей, и тихая отправка всего подряд
// на другое устройство — не то поведение, которого ожидают молча. Кнопка «Отправить» есть и получить можно
// только то, что отправили явно.
class ClipboardPlugin : public PluginBase
{
    Q_OBJECT

public:
    explicit ClipboardPlugin(PeerManager *manager, QObject *parent = nullptr);

    Q_INVOKABLE void sendClipboardTo(const QString &deviceId);
    void handleMessage(const QString &deviceId, const QJsonObject &payload) override;

signals:
    void textReceived(const QString &deviceId, const QString &fromName, const QString &text);
};

#endif // CLIPBOARDPLUGIN_H
