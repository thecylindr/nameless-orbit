#ifndef PEERMANAGER_H
#define PEERMANAGER_H

#include <QObject>
#include <QTcpServer>
#include <QHash>
#include <QVariantList>
#include <QJsonObject>
#include <QSet>
#include <QTimer>
#include <functional>
#include "Discovery.h"
#include "Identity.h"
#include "TrustStore.h"     // полные типы нужны moc для свойств identity / trustStore и плагинам, читающим имя устройства

class Peer;
class PluginBase;

// Центр притяжения всего Orbit: слушает входящие подключения, поднимает исходящие к найденным устройствам,
// решает вопрос доверия (через TrustStore) и раздаёт сообщения плагинам. Единственный объект, с которым
// напрямую говорит QML по части соединений — остальные классы ядра видны ему, но не наружу.
//
// Состояния одного deviceId: не виден | виден в сети (Discovery) | идёт рукопожатие | ждёт решения о
// сопряжении (не в TrustStore) | подключён и доверен (в TrustStore, ключ совпал) | ключ не совпал (тревога).
class PeerManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool discoveryActive READ discoveryActive WRITE setDiscoveryActive NOTIFY discoveryActiveChanged)
    Q_PROPERTY(bool listening READ listening NOTIFY listeningChanged)
    Q_PROPERTY(QVariantList nearbyDevices READ nearbyDevices NOTIFY nearbyChanged)
    Q_PROPERTY(QVariantList connectedDevices READ connectedDevicesForQml NOTIFY connectedChanged)
    Q_PROPERTY(QVariantList pairingRequests READ pairingRequestsForQml NOTIFY pairingRequestsChanged)
    Q_PROPERTY(Identity *identity READ identity CONSTANT)
    Q_PROPERTY(TrustStore *trustStore READ trustStore CONSTANT)

public:
    explicit PeerManager(Identity *identity, TrustStore *trustStore, QObject *parent = nullptr);

    bool discoveryActive() const;
    void setDiscoveryActive(bool on);
    bool listening() const { return m_server.isListening(); }

    QVariantList nearbyDevices() const;          // объединение Discovery + известных по TrustStore (включая офлайн)
    QVariantList connectedDevicesForQml() const;
    QVariantList pairingRequestsForQml() const;

    Identity *identity() const { return m_identity; }
    TrustStore *trustStore() const { return m_trustStore; }

    // для плагинов: найти активного доверенного пира по deviceId (nullptr — не подключён)
    Peer *trustedPeer(const QString &deviceId) const;
    void registerPlugin(const QString &name, PluginBase *plugin);
    void sendPlugin(const QString &deviceId, const QString &plugin, const QJsonObject &payload);
    void broadcastPlugin(const QString &plugin, const QJsonObject &payload, const QString &requiredCapability);

public:
    // список быстрых команд этого устройства (название, значок) для знакомства: показывается другой стороне при сопряжении
    void setIntroProvider(std::function<QVariantList()> provider) { m_introProvider = std::move(provider); }

public slots:
    // решение при сопряжении: разрешить ли этому устройству запускать мои быстрые команды (по умолчанию да, как и показано в окне)
    void setPairingCommandsAllowed(const QString &deviceId, bool allowed);
    void connectToDevice(const QString &deviceId);                 // по адресу из Discovery
    void connectByAddress(const QString &host, int port);          // ручной ввод IP (сопряжение не в одной сети)
    void acceptPairing(const QString &deviceId);
    void rejectPairing(const QString &deviceId);
    void forgetDevice(const QString &deviceId);
    void disconnectDevice(const QString &deviceId);
    void refresh();                                                 // перечитать список рядом и попросить маяк сейчас

public:
    // сколько байт для устройства ещё не ушло в сеть — плагины с потоком данных (экран, файлы) по нему не копят очередь
    qint64 backlog(const QString &deviceId) const;
    bool isConnected(const QString &deviceId) const { return m_peers.contains(deviceId); }

signals:
    void discoveryActiveChanged();
    void listeningChanged();
    void nearbyChanged();
    void connectedChanged();
    void pairingRequestsChanged();
    void pairingRequested(const QString &deviceId, const QString &name, const QString &kind, const QString &code);
    void pairingDeclined(const QString &deviceId, const QString &name);       // другая сторона отказала (или не ответила вовремя)
    void deviceConnected(const QString &deviceId, const QString &name);
    void deviceDisconnected(const QString &deviceId);
    void keyMismatch(const QString &deviceId, const QString &name, const QString &oldFingerprint, const QString &newFingerprint);
    // сообщение плагину — кидается конкретному зарегистрированному PluginBase через registerPlugin
    void pluginMessage(const QString &deviceId, const QString &plugin, const QJsonObject &payload);

private slots:
    void onNewConnection();

private:
    // Сопряжение — только по согласию ОБЕИХ сторон: каждая нажимает «Сопрячь» после сверки кода, решение уходит собеседнику
    // сообщением «pair». Пока не согласились оба, доверие не записывается и плагины не работают
    struct PendingPairing { Peer *peer; QString name, kind, code; bool outgoing; bool localAccepted = false; bool remoteAccepted = false; qint64 startedMs = 0;
                            bool allowCommands = true; QVariantList remoteCommands; };

    void wireUpPeer(Peer *peer);
    void onPeerHandshakeDone(Peer *peer);
    void onPeerFailed(Peer *peer, const QString &reason);
    void onPeerMessage(Peer *peer, const QJsonObject &msg);
    void onPeerDisconnected(Peer *peer);
    void sendCapabilities(Peer *peer);
    void finalizeConnected(Peer *peer);
    void completePairing(const QString &deviceId);
    void autoReconnect();                         // доверенные устройства подключаются сами, когда появляются
    void dial(const QString &deviceId, const QHostAddress &addr, quint16 port);
    bool prefers(Peer *p) const;                  // какое из двух встречных подключений оставить (одинаково у обеих сторон)
    QStringList enabledCapabilities() const;

    Identity *m_identity;
    TrustStore *m_trustStore;
    Discovery m_discovery;
    QTcpServer m_server;

    QHash<QString, DiscoveredDevice> m_nearby;          // живой список от маяка (не обязательно подключены)
    QHash<QString, Peer *> m_peers;                     // подключённые и доверенные (connected)
    QHash<QString, qint64> m_blockedUntil;              // устройства, чей запрос на сопряжение отклонён: повторные запросы временно не принимаются
    QList<qint64> m_pairStarts;                         // времена последних запросов на сопряжение (общий предел в минуту)
    QHash<QString, PendingPairing> m_pending;           // рукопожатие прошло, ждём решения пользователя
    QHash<QString, QStringList> m_remoteCapabilities;   // что умеет каждый подключённый пир (для показа в UI)
    QHash<QString, PluginBase *> m_plugins;             // имя плагина -> обработчик

    std::function<QVariantList()> m_introProvider;
    QTimer m_maintainTimer;                             // раз в 5 секунд: просроченные запросы, повторные подключения
    QHash<Peer *, QString> m_dialing;                   // исходящие попытки: пир -> deviceId, к которому идём
    QHash<QString, qint64> m_nextAttemptMs;             // когда можно пробовать снова
    QHash<QString, int> m_attempts;                     // подряд неудачных попыток (задержка растёт: 6, 12, 24… до 60 с)
    QHash<QString, qint64> m_wantedSinceMs;             // с какого момента устройство «должно быть на связи», но нет
};

#endif // PEERMANAGER_H
