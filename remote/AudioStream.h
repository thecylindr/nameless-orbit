#ifndef AUDIOSTREAM_H
#define AUDIOSTREAM_H

#include <QObject>
#include <QByteArray>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <memory>

class QAudioSink;
class QIODevice;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

// Звук между устройствами. Две независимые половины:
//   • отправка (только настольный Linux): звук ПК снимается через parec (PulseAudio / PipeWire-pulse) кусками по 20 мс, 48 кГц, стерео, 16 бит;
//     дальше либо уходит как есть («pcm», ≈1,5 Мбит/с, без задержки на кодер), либо сжимается в Opus через libavcodec («opus», 32–128 кбит/с);
//   • приём (все системы с Qt Multimedia): пришедшие куски разжимаются (Opus) и пишутся в QAudioSink; задержка — размер буфера приёмника,
//     лишнее при отставании выбрасывается, чтобы звук не уходил в хвост очереди.
// Сам класс ничего не знает о сети и разрешениях: пакеты отдаёт сигналом packet(), принимает через play()
class AudioStream : public QObject
{
    Q_OBJECT
public:
    explicit AudioStream(QObject *parent = nullptr);
    ~AudioStream() override;

    static bool captureAvailable();                 // можно ли снимать звук на этой системе (настольный Linux и parec в PATH)
    static bool opusAvailable();                    // собран ли Opus (libavcodec с libopus для кодера, любой опус-декодер — для приёма)
    static QStringList sources();                   // источники звука, которые можно отдать: «мониторы» выходов и входы (имена pactl)

    // отправка
    bool startCapture(const QString &source, const QString &codec, int opusKbps);       // source пуст — выход по умолчанию (@DEFAULT_MONITOR@)
    void stopCapture();
    bool capturing() const { return m_parec != nullptr; }
    QString codec() const { return m_codec; }

    // приём
    void setVolume(double v);                       // 0..1
    void setLatencyMs(int ms);
    bool play(const QByteArray &data, const QString &codec);
    void stopPlayback();
    bool playing() const { return m_sink != nullptr; }

signals:
    void packet(const QByteArray &data, const QString &codec);       // готовый кусок звука 20 мс — в сеть
    // отправка: громкость выхода, который снимается (доля по шкале системы: 1 — 100 %), и «без звука». Монитор выхода её не учитывает —
    // её отдельно повторяет приёмник, чтобы звук был таким же громким, как на этом компьютере
    void volumeChanged(double level, bool muted);
    void failed(const QString &reason);

private:
    void onCaptureData();
    bool openEncoder(int kbps);
    bool openDecoder();
    QByteArray encodeOpus(const QByteArray &pcm);
    QByteArray decodeOpus(const QByteArray &data);
    void closeCodecs();
    void ensureSink();

    void watchVolume(const QString &source);       // слежение за громкостью выхода: pactl subscribe — по событиям, без опроса
    void queryVolume();
    QProcess *m_parec = nullptr;
    QProcess *m_volWatch = nullptr;
    QString m_volSink;                             // выход, громкость которого повторяется (пусто — снимается вход, повторять нечего)
    double m_lastVol = -1;
    bool m_lastMute = false;
    QByteArray m_buf;
    QString m_codec;

    QAudioSink *m_sink = nullptr;
    QIODevice *m_io = nullptr;
    double m_volume = 0.8;
    int m_latencyMs = 120;

    AVCodecContext *m_enc = nullptr;
    AVCodecContext *m_dec = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_pkt = nullptr;
    qint64 m_encPts = 0;
};

#endif
