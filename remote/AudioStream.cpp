#include "AudioStream.h"
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QStandardPaths>
#include <QIODevice>
#include <QRegularExpression>
#include <QTimer>
#include <cstring>

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <signal.h>
#include <sys/prctl.h>
#endif

#ifdef ORBIT_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}
#endif

namespace {
constexpr int RATE = 48000;
constexpr int CHANNELS = 2;
constexpr int FRAME_SAMPLES = 960;                           // 20 мс при 48 кГц
constexpr int FRAME_BYTES = FRAME_SAMPLES * CHANNELS * 2;    // 16 бит
}

AudioStream::AudioStream(QObject *parent) : QObject(parent) {}
AudioStream::~AudioStream() { stopCapture(); stopPlayback(); closeCodecs(); }

bool AudioStream::captureAvailable()
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    return !QStandardPaths::findExecutable(QStringLiteral("parec")).isEmpty();
#else
    return false;
#endif
}

bool AudioStream::opusAvailable()
{
#ifdef ORBIT_HAVE_FFMPEG
    return avcodec_find_decoder(AV_CODEC_ID_OPUS) != nullptr;       // приём; кодер libopus проверяется при старте отправки, при отсутствии отправка идёт как pcm
#else
    return false;
#endif
}

QStringList AudioStream::sources()
{
    QStringList out;
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    QProcess p;
    p.setStandardInputFile(QProcess::nullDevice());
    p.start(QStringLiteral("pactl"), {QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("sources")});
    if (!p.waitForFinished(1500)) { p.kill(); return out; }
    const QList<QByteArray> lines = p.readAllStandardOutput().split('\n');
    for (const QByteArray &l : lines) {
        const QList<QByteArray> f = l.split('\t');
        if (f.size() >= 2 && !f.at(1).isEmpty()) out << QString::fromUtf8(f.at(1));
    }
#endif
    return out;
}

// ---------- отправка ----------

bool AudioStream::startCapture(const QString &source, const QString &codec, int opusKbps)
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    stopCapture();
    if (!captureAvailable()) { emit failed(QStringLiteral("на этом компьютере нет parec (пакет pulseaudio-utils или pipewire-pulse)")); return false; }
    m_codec = QStringLiteral("pcm");
    if (codec == QLatin1String("opus")) {
        if (openEncoder(opusKbps)) m_codec = QStringLiteral("opus");
        // кодера Opus нет — звук идёт без сжатия, получатель разберёт по метке в пакете
    }
    m_parec = new QProcess(this);
    m_parec->setStandardInputFile(QProcess::nullDevice());
    m_parec->setProcessChannelMode(QProcess::SeparateChannels);
    m_parec->setChildProcessModifier([]() { prctl(PR_SET_PDEATHSIG, SIGTERM); });          // ПК закрылся — захват звука не остаётся сиротой
    connect(m_parec, &QProcess::readyReadStandardOutput, this, &AudioStream::onCaptureData);
    connect(m_parec, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) emit failed(QStringLiteral("parec не запустился"));
    });
    connect(m_parec, &QProcess::finished, this, [this]() {
        if (m_parec) emit failed(QStringLiteral("захват звука остановился"));
    });
    m_buf.clear();
    m_encPts = 0;
    m_parec->start(QStringLiteral("parec"), {QStringLiteral("--device=") + (source.isEmpty() ? QStringLiteral("@DEFAULT_MONITOR@") : source),
                                              QStringLiteral("--format=s16le"), QStringLiteral("--rate=48000"), QStringLiteral("--channels=2"),
                                              QStringLiteral("--latency-msec=20")});
    watchVolume(source);
    return true;
#else
    Q_UNUSED(source) Q_UNUSED(codec) Q_UNUSED(opusKbps)
    emit failed(QStringLiteral("эта система не умеет отдавать звук"));
    return false;
#endif
}

// источник — монитор выхода (по умолчанию или «<выход>.monitor»): следим за громкостью этого выхода
void AudioStream::watchVolume(const QString &source)
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    m_volSink = source.isEmpty() || source == QLatin1String("@DEFAULT_MONITOR@") ? QStringLiteral("@DEFAULT_SINK@")
              : (source.endsWith(QLatin1String(".monitor")) ? source.chopped(8) : QString());
    m_lastVol = -1;
    if (m_volSink.isEmpty() || QStandardPaths::findExecutable(QStringLiteral("pactl")).isEmpty()) return;
    m_volWatch = new QProcess(this);
    m_volWatch->setChildProcessModifier([]() { prctl(PR_SET_PDEATHSIG, SIGTERM); });
    connect(m_volWatch, &QProcess::readyReadStandardOutput, this, [this]() {
        // «Event 'change' on sink #57» и смена выхода по умолчанию («on server») — перечитать громкость; пачка событий — один запрос
        bool relevant = false;
        while (m_volWatch && m_volWatch->canReadLine()) {
            const QByteArray line = m_volWatch->readLine();
            relevant = relevant || line.contains(" sink ") || line.contains(" server");
        }
        if (relevant) QTimer::singleShot(40, this, &AudioStream::queryVolume);
    });
    m_volWatch->start(QStringLiteral("pactl"), {QStringLiteral("subscribe")});
    queryVolume();
#else
    Q_UNUSED(source)
#endif
}

void AudioStream::queryVolume()
{
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    if (m_volSink.isEmpty() || !m_parec) return;
    auto *p = new QProcess(this);
    connect(p, &QProcess::finished, this, [this, p]() {
        const QString out = QString::fromUtf8(p->readAllStandardOutput());
        p->deleteLater();
        // «Volume: front-left: 29491 /  45% / -20,81 dB, …» и «Mute: no»: громкость — среднее по каналам
        static const QRegularExpression pctRe(QStringLiteral("(\\d+)%"));
        int sum = 0, n = 0;
        for (auto it = pctRe.globalMatch(out.section(QLatin1Char('\n'), 0, 0)); it.hasNext();) { sum += it.next().captured(1).toInt(); ++n; }
        if (n == 0) return;
        const double level = sum / (100.0 * n);
        const bool muted = out.contains(QLatin1String("Mute: yes"));
        if (qAbs(level - m_lastVol) < 0.005 && muted == m_lastMute) return;
        m_lastVol = level; m_lastMute = muted;
        emit volumeChanged(level, muted);
    });
    p->start(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("LC_ALL=C pactl get-sink-volume \"$1\"; LC_ALL=C pactl get-sink-mute \"$1\""),
                                    QStringLiteral("sh"), m_volSink});
#endif
}

void AudioStream::stopCapture()
{
    if (m_volWatch) { m_volWatch->disconnect(this); m_volWatch->kill(); m_volWatch->deleteLater(); m_volWatch = nullptr; }
    if (m_parec) {
        QProcess *p = m_parec;
        m_parec = nullptr;                      // сначала сбросить: сигнал finished от остановки не считается сбоем
        p->disconnect(this);
        p->kill();
        p->deleteLater();
    }
    m_buf.clear();
    if (m_enc) closeCodecs();
}

void AudioStream::onCaptureData()
{
    if (!m_parec) return;
    m_buf += m_parec->readAllStandardOutput();
    // отстали (процесс завис, сеть): копится больше четверти секунды — старое выбрасывается, звук идёт «сейчас», а не из прошлого
    if (m_buf.size() > FRAME_BYTES * 12) m_buf.remove(0, m_buf.size() - FRAME_BYTES * 2);
    while (m_buf.size() >= FRAME_BYTES) {
        const QByteArray pcm = m_buf.left(FRAME_BYTES);
        m_buf.remove(0, FRAME_BYTES);
        if (m_codec == QLatin1String("opus")) {
            const QByteArray enc = encodeOpus(pcm);
            if (!enc.isEmpty()) emit packet(enc, QStringLiteral("opus"));
        } else {
            emit packet(pcm, QStringLiteral("pcm"));
        }
    }
}

// ---------- приём ----------

void AudioStream::setVolume(double v)
{
    m_volume = qBound(0.0, v, 1.0);
    if (m_sink) m_sink->setVolume(m_volume);
}

void AudioStream::setLatencyMs(int ms)
{
    const int n = qBound(40, ms, 600);
    if (n == m_latencyMs) return;
    m_latencyMs = n;
    if (m_sink) stopPlayback();                 // буфер приёмника задаётся при создании: новый размер — со следующим куском
}

void AudioStream::ensureSink()
{
    if (m_sink) return;
    QAudioFormat fmt;
    fmt.setSampleRate(RATE);
    fmt.setChannelCount(CHANNELS);
    fmt.setSampleFormat(QAudioFormat::Int16);
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull() || !dev.isFormatSupported(fmt)) { emit failed(QStringLiteral("на этом устройстве нет вывода звука 48 кГц")); return; }
    m_sink = new QAudioSink(dev, fmt, this);
    m_sink->setBufferSize(RATE * CHANNELS * 2 * m_latencyMs / 1000);
    m_sink->setVolume(m_volume);
    m_io = m_sink->start();
    if (!m_io) { delete m_sink; m_sink = nullptr; emit failed(QStringLiteral("не удалось открыть вывод звука")); }
}

bool AudioStream::play(const QByteArray &data, const QString &codec)
{
    QByteArray pcm = data;
    if (codec == QLatin1String("opus")) {
        pcm = decodeOpus(data);
        if (pcm.isEmpty()) return false;
    }
    if (pcm.isEmpty()) return false;
    ensureSink();
    if (!m_sink || !m_io) return false;
    // в буфере уже почти полная задержка — кусок выбрасывается, звук догоняет, а не отстаёт всё сильнее
    const qsizetype queued = m_sink->bufferSize() - m_sink->bytesFree();
    if (queued > m_sink->bufferSize() * 9 / 10) return false;
    m_io->write(pcm);
    return true;
}

void AudioStream::stopPlayback()
{
    if (m_sink) {
        m_sink->stop();
        m_sink->deleteLater();
        m_sink = nullptr;
        m_io = nullptr;
    }
    if (m_dec) closeCodecs();
}

// ---------- Opus ----------

void AudioStream::closeCodecs()
{
#ifdef ORBIT_HAVE_FFMPEG
    if (m_enc) avcodec_free_context(&m_enc);
    if (m_dec) avcodec_free_context(&m_dec);
    if (m_frame) av_frame_free(&m_frame);
    if (m_pkt) av_packet_free(&m_pkt);
#endif
}

bool AudioStream::openEncoder(int kbps)
{
#ifdef ORBIT_HAVE_FFMPEG
    if (m_enc) return true;
    const AVCodec *c = avcodec_find_encoder_by_name("libopus");
    if (!c) return false;
    m_enc = avcodec_alloc_context3(c);
    if (!m_enc) return false;
    m_enc->sample_rate = RATE;
    av_channel_layout_default(&m_enc->ch_layout, CHANNELS);
    m_enc->sample_fmt = AV_SAMPLE_FMT_S16;
    m_enc->bit_rate = qBound(16, kbps, 256) * 1000;
    m_enc->time_base = AVRational{1, RATE};
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "application", "lowdelay", 0);
    av_dict_set(&opts, "frame_duration", "20", 0);
    const int r = avcodec_open2(m_enc, c, &opts);
    av_dict_free(&opts);
    if (r < 0) { avcodec_free_context(&m_enc); return false; }
    if (!m_frame) m_frame = av_frame_alloc();
    if (!m_pkt) m_pkt = av_packet_alloc();
    m_frame->nb_samples = FRAME_SAMPLES;
    m_frame->format = AV_SAMPLE_FMT_S16;
    av_channel_layout_default(&m_frame->ch_layout, CHANNELS);
    m_frame->sample_rate = RATE;
    if (av_frame_get_buffer(m_frame, 0) < 0) { closeCodecs(); return false; }
    return true;
#else
    Q_UNUSED(kbps)
    return false;
#endif
}

QByteArray AudioStream::encodeOpus(const QByteArray &pcm)
{
#ifdef ORBIT_HAVE_FFMPEG
    if (!m_enc || pcm.size() != FRAME_BYTES) return {};
    if (av_frame_make_writable(m_frame) < 0) return {};
    std::memcpy(m_frame->data[0], pcm.constData(), size_t(FRAME_BYTES));
    m_frame->pts = m_encPts;
    m_encPts += FRAME_SAMPLES;
    if (avcodec_send_frame(m_enc, m_frame) < 0) return {};
    QByteArray out;
    while (avcodec_receive_packet(m_enc, m_pkt) == 0) {
        out = QByteArray(reinterpret_cast<const char *>(m_pkt->data), m_pkt->size);
        av_packet_unref(m_pkt);
    }
    return out;
#else
    Q_UNUSED(pcm)
    return {};
#endif
}

bool AudioStream::openDecoder()
{
#ifdef ORBIT_HAVE_FFMPEG
    if (m_dec) return true;
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!c) return false;
    m_dec = avcodec_alloc_context3(c);
    if (!m_dec) return false;
    m_dec->sample_rate = RATE;
    av_channel_layout_default(&m_dec->ch_layout, CHANNELS);
    if (avcodec_open2(m_dec, c, nullptr) < 0) { avcodec_free_context(&m_dec); return false; }
    if (!m_pkt) m_pkt = av_packet_alloc();
    if (!m_frame) m_frame = av_frame_alloc();
    return true;
#else
    return false;
#endif
}

QByteArray AudioStream::decodeOpus(const QByteArray &data)
{
#ifdef ORBIT_HAVE_FFMPEG
    if (!openDecoder() || data.isEmpty()) { emit failed(QStringLiteral("на этом устройстве нет декодера Opus")); return {}; }
    av_packet_unref(m_pkt);
    if (av_new_packet(m_pkt, data.size()) < 0) return {};
    std::memcpy(m_pkt->data, data.constData(), size_t(data.size()));
    if (avcodec_send_packet(m_dec, m_pkt) < 0) return {};
    QByteArray out;
    while (avcodec_receive_frame(m_dec, m_frame) == 0) {
        const int n = m_frame->nb_samples;
        const int ch = qMin(CHANNELS, m_frame->ch_layout.nb_channels);
        QByteArray part(n * CHANNELS * 2, 0);
        qint16 *dst = reinterpret_cast<qint16 *>(part.data());
        auto clip = [](float f) { return qint16(qBound(-1.0f, f, 1.0f) * 32767.0f); };
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < CHANNELS; ++c) {
                const int src = qMin(c, ch - 1);
                qint16 v = 0;
                switch (m_frame->format) {
                case AV_SAMPLE_FMT_S16:  v = reinterpret_cast<const qint16 *>(m_frame->data[0])[i * m_frame->ch_layout.nb_channels + src]; break;
                case AV_SAMPLE_FMT_S16P: v = reinterpret_cast<const qint16 *>(m_frame->data[src])[i]; break;
                case AV_SAMPLE_FMT_FLT:  v = clip(reinterpret_cast<const float *>(m_frame->data[0])[i * m_frame->ch_layout.nb_channels + src]); break;
                case AV_SAMPLE_FMT_FLTP: v = clip(reinterpret_cast<const float *>(m_frame->data[src])[i]); break;
                default: break;
                }
                dst[i * CHANNELS + c] = v;
            }
        }
        out += part;
        av_frame_unref(m_frame);
    }
    return out;
#else
    Q_UNUSED(data)
    return {};
#endif
}
