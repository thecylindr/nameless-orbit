#ifndef MEDIAPLUGIN_H
#define MEDIAPLUGIN_H

#include "PluginBase.h"
#include <QTimer>

// Управление плеером на другом устройстве: пауза/пуск, вперёд/назад, текущий трек — тот же набор, что
// у плеера в баре NameLess UI, но посланный по сети. На Linux источник/приёмник команд — MPRIS через DBus.
class MediaPlugin : public PluginBase
{
    Q_OBJECT
    Q_PROPERTY(QString trackTitle READ trackTitle NOTIFY localTrackChanged)
    Q_PROPERTY(QString trackArtist READ trackArtist NOTIFY localTrackChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY localTrackChanged)

public:
    explicit MediaPlugin(PeerManager *manager, QObject *parent = nullptr);

    QString trackTitle() const { return m_title; }
    QString trackArtist() const { return m_artist; }
    bool playing() const { return m_playing; }

    // команды НА удалённое устройство
    Q_INVOKABLE void sendCommand(const QString &deviceId, const QString &action);   // "playpause" | "next" | "previous"

    void handleMessage(const QString &deviceId, const QJsonObject &payload) override;

public slots:
    void refreshLocal();

signals:
    void localTrackChanged();
    void remoteTrackChanged(const QString &deviceId, const QString &title, const QString &artist, bool playing);

private:
    void applyLocalCommand(const QString &action);
    void sendStateTo(const QString &deviceId);

    QString m_title, m_artist;
    bool m_playing = false;
    QTimer *m_pollTimer;
};

#endif // MEDIAPLUGIN_H
