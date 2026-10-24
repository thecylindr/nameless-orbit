#ifndef MEDIAPLUGIN_H
#define MEDIAPLUGIN_H

#include "PluginBase.h"
#include <QHash>
#include <QTimer>
#include <QVariantMap>

// Плеер другого устройства: что играет (название, исполнитель, альбом, приложение, обложка, позиция) и управление
// (пауза/пуск, вперёд/назад). ПК — MPRIS через DBus; Android — медиасессии системы (нужен доступ к уведомлениям,
// без него — только «играет ли звук»). Обложка — маленький JPEG, уходит только при смене трека (artKey)
class MediaPlugin : public PluginBase
{
    Q_OBJECT
    Q_PROPERTY(QString trackTitle READ trackTitle NOTIFY localTrackChanged)
    Q_PROPERTY(QString trackArtist READ trackArtist NOTIFY localTrackChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY localTrackChanged)
    Q_PROPERTY(bool access READ access NOTIFY localTrackChanged)          // Android: есть доступ к медиасессиям (показывать трек компьютеру)

public:
    explicit MediaPlugin(PeerManager *manager, QObject *parent = nullptr);

    QString trackTitle() const { return m_local.value(QStringLiteral("title")).toString(); }
    QString trackArtist() const { return m_local.value(QStringLiteral("artist")).toString(); }
    bool playing() const { return m_local.value(QStringLiteral("playing")).toBool(); }
    bool access() const;

    // команды НА удалённое устройство
    Q_INVOKABLE void sendCommand(const QString &deviceId, const QString &action);   // "playpause" | "next" | "previous"
    // что играет на устройстве: { title, artist, album, app, playing, pos, len (мс), art (base64 JPEG), artUrl, at (когда получено, мс) }
    Q_INVOKABLE QVariantMap remoteMedia(const QString &deviceId) const { return m_remote.value(deviceId); }
    Q_INVOKABLE void openAccessSettings() const;     // Android: «Доступ к уведомлениям» → Orbit

    void handleMessage(const QString &deviceId, const QJsonObject &payload) override;

public slots:
    void refreshLocal();

signals:
    void localTrackChanged();
    void remoteTrackChanged(const QString &deviceId, const QString &title, const QString &artist, bool playing);
    void remoteMediaChanged(const QString &deviceId);

private:
    void applyLocalCommand(const QString &action);
    void sendStateTo(const QString &deviceId, bool withArt);
    QJsonObject payload(bool withArt) const;

    QVariantMap m_local;                       // своё состояние в том же виде, что уходит по сети
    QString m_sentKey;                         // последнее разосланное «трек + состояние»
    qint64 m_sentAt = 0;
    QHash<QString, QVariantMap> m_remote;
    QTimer *m_pollTimer;
};

#endif // MEDIAPLUGIN_H
