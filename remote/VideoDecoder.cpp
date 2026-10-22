#include "VideoDecoder.h"
#include <QVideoFrameFormat>
#include <QMetaObject>
#include <cstring>

#ifdef ORBIT_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#endif

VideoDecoder::VideoDecoder(QObject *parent) : QObject(parent) {}
VideoDecoder::~VideoDecoder() { stop(); }

bool VideoDecoder::available()
{
#ifdef ORBIT_HAVE_FFMPEG
    return avcodec_find_decoder(AV_CODEC_ID_H264) != nullptr;
#else
    return false;
#endif
}

bool VideoDecoder::feed(const QByteArray &au, bool key, qint64 ptsUs)
{
#ifdef ORBIT_HAVE_FFMPEG
    if (au.size() < 5) return false;
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_started) {
        if (!key) return false;                          // начинать можно только с ключевого кадра (в нём SPS/PPS)
        m_quit = false;
        m_reported = false;
        m_started = true;
        m_thread = std::thread([this]() { run(); });
    }
    if (m_queue.size() >= 6) {
        m_queue.clear();
        m_waitKey = true;
    }
    if (m_waitKey && !key) return false;
    if (key) m_waitKey = false;
    m_queue.push_back({au, ptsUs, key});
    m_cv.notify_one();
    return true;
#else
    Q_UNUSED(au) Q_UNUSED(key) Q_UNUSED(ptsUs)
    return false;
#endif
}

void VideoDecoder::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = true;
        ++m_generation;
        m_waitKey = true;
        m_queue.clear();
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_started = false;
}

void VideoDecoder::run()
{
#ifdef ORBIT_HAVE_FFMPEG
    const quint64 generation = m_generation.load();
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    auto fail = [this](const QString &why) {
        if (m_reported) return;
        m_reported = true;
        QMetaObject::invokeMethod(this, [this, why]() { emit failed(why); }, Qt::QueuedConnection);
    };
    if (!ctx || !pkt || !frame) { fail(QStringLiteral("не удалось создать декодер H.264")); }
    else {
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;                // без накопления кадров на выходе
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        ctx->thread_count = 2;
        ctx->thread_type = FF_THREAD_SLICE;                   // кадровые потоки добавили бы задержку в несколько кадров
        if (avcodec_open2(ctx, codec, nullptr) < 0) fail(QStringLiteral("libavcodec не открыл декодер H.264"));
        else {
            for (;;) {
                Unit u;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cv.wait(lock, [this]() { return m_quit.load() || !m_queue.empty(); });
                    if (m_quit) break;
                    u = std::move(m_queue.front());
                    m_queue.pop_front();
                }
                if (u.key) avcodec_flush_buffers(ctx); // IDR also clears references after queue loss or encoder resize.
                if (av_new_packet(pkt, int(u.data.size())) < 0) continue;
                std::memcpy(pkt->data, u.data.constData(), size_t(u.data.size()));
                pkt->pts = u.pts;
                const int sent = avcodec_send_packet(ctx, pkt);
                av_packet_unref(pkt);
                if (sent < 0 && sent != AVERROR(EAGAIN)) continue;     // битый кадр — пропускается, следующий ключевой всё исправит
                while (avcodec_receive_frame(ctx, frame) == 0) {
                    const bool nv12 = frame->format == AV_PIX_FMT_NV12;
                    const bool i420 = frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P;
                    if (!nv12 && !i420) { fail(QStringLiteral("непонятный формат пикселей кадра")); av_frame_unref(frame); break; }
                    const int w = frame->width, h = frame->height;
                    QVideoFrameFormat fmt(QSize(w, h), nv12 ? QVideoFrameFormat::Format_NV12 : QVideoFrameFormat::Format_YUV420P);
                    // цвета: BT.601 помечен только поток x264 у нас; всё остальное (видеокарта с явной матрицей, MediaCodec) — BT.709
                    const bool bt709 = frame->colorspace == AVCOL_SPC_BT709 || frame->colorspace == AVCOL_SPC_UNSPECIFIED;      // без метки — BT.709 (так кодирует видеокарта: матрица задана явно)
                    fmt.setColorSpace(bt709 ? QVideoFrameFormat::ColorSpace_BT709 : QVideoFrameFormat::ColorSpace_BT601);
                    // диапазон — всегда «видео» (16–235): кодеры Orbit отдают именно его, а метка «полный» в потоке от wf-recorder неверна — по ней чёрное светлело и контраст рос
                    fmt.setColorRange(QVideoFrameFormat::ColorRange_Video);
                    QVideoFrame out(fmt);
                    if (out.map(QVideoFrame::WriteOnly)) {
                        auto copy = [](uchar *dst, int dstStride, const uint8_t *src, int srcStride, int rows, int bytes) {
                            for (int r = 0; r < rows; ++r) std::memcpy(dst + size_t(r) * dstStride, src + size_t(r) * srcStride, size_t(bytes));
                        };
                        const int ch = (h + 1) / 2, cw = (w + 1) / 2;
                        copy(out.bits(0), out.bytesPerLine(0), frame->data[0], frame->linesize[0], h, w);
                        if (nv12) copy(out.bits(1), out.bytesPerLine(1), frame->data[1], frame->linesize[1], ch, cw * 2);
                        else { copy(out.bits(1), out.bytesPerLine(1), frame->data[1], frame->linesize[1], ch, cw);
                               copy(out.bits(2), out.bytesPerLine(2), frame->data[2], frame->linesize[2], ch, cw); }
                        out.unmap();
                        const qint64 pts = frame->pts;
                        QMetaObject::invokeMethod(this, [this, out, pts, generation]() { if (m_generation == generation) emit frameDecoded(out, pts); }, Qt::QueuedConnection);
                    }
                    av_frame_unref(frame);
                }
            }
        }
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
#endif
}
