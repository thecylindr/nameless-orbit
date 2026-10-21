#ifndef SHAREPLUGIN_H
#define SHAREPLUGIN_H

#include "PluginBase.h"
#include <QHash>
#include <QTimer>
#include <QVariantList>
#include <QFile>
#include <memory>

// Передача файлов между устройствами (телефон ↔ компьютер, компьютер ↔ компьютер) по тому же защищённому каналу.
//   отправитель: offer {id, name, size} → ждёт «accept» получателя → шлёт куски по 32 КБ, не больше окна из 8 неподтверждённых
//   получатель:  показывает запрос с именем и размером; принять может только человек → ack на каждый кусок → done → «saved»
// Файл принимается во временный файл и появляется в «Загрузках» (Android — Download/Orbit) только после того, как пришёл целиком
// и размер совпал. Имя очищается от путей; перезаписывать чужие файлы нельзя — при совпадении имени добавляется «(1)»
class SharePlugin : public PluginBase
{
    Q_OBJECT
    // список передач для показа: id, deviceId, deviceName, name, size, done, outgoing, state (offered|active|done|failed|declined|cancelled), path
    Q_PROPERTY(QVariantList transfers READ transfers NOTIFY transfersChanged)

public:
    explicit SharePlugin(PeerManager *manager, QObject *parent = nullptr);

    QVariantList transfers() const;

    // файл — локальный путь или URL (file://, content:// на Android)
    Q_INVOKABLE void sendFile(const QString &deviceId, const QString &pathOrUrl);
    Q_INVOKABLE void accept(const QString &id);
    Q_INVOKABLE void decline(const QString &id);
    Q_INVOKABLE void cancel(const QString &id);
    Q_INVOKABLE void clearFinished();

    void handleMessage(const QString &deviceId, const QJsonObject &payload) override;
    bool needsPermission(const QJsonObject &p) const override { return p.value(QStringLiteral("kind")).toString() == QLatin1String("offer"); }

signals:
    void transfersChanged();
    void offerReceived(const QString &id, const QString &deviceName, const QString &fileName, qint64 size);   // нужен ответ человека
    void fileReceived(const QString &deviceName, const QString &fileName, const QString &savedAs);
    void fileSent(const QString &deviceName, const QString &fileName);
    void transferFailed(const QString &fileName, const QString &reason);

private:
    struct Transfer
    {
        QString id, deviceId, deviceName, name, state, path, partPath;
        qint64 size = 0, done = 0;
        bool outgoing = false;
        std::unique_ptr<QFile> file;
        int sentSeq = 0, ackedSeq = 0, expectSeq = 0;
        bool eofSent = false;
    };

    void pump(Transfer &t);
    void pumpAll();
    void finish(Transfer &t, const QString &state, const QString &reason = QString());
    void sendTo(const Transfer &t, const QJsonObject &payload);
    void changed();
    static QString cleanName(const QString &name);
    static QString uniqueName(const QString &dir, const QString &name);

    QHash<QString, std::shared_ptr<Transfer>> m_transfers;
    QStringList m_order;                 // порядок показа: новые в конце
    QTimer m_pumpTimer;                  // подталкивает отправку, пока сеть не успевает
    QTimer m_notifyTimer;                // склеивает частые обновления прогресса для интерфейса
    bool m_dirty = false;
};

#endif // SHAREPLUGIN_H
