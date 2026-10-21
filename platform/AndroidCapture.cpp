#include "AndroidCapture.h"

#ifdef __ANDROID__
#include "../remote/RemoteDesktopController.h"
#include <QByteArray>
#include <QImage>
#include <QCoreApplication>
#include <QJniObject>
#include <QMetaObject>
#include <atomic>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <cstring>
#include <jni.h>

namespace {
std::atomic<RemoteDesktopController *> g_controller{nullptr};
std::atomic<int> g_quality{55};
std::atomic<int> g_fps{8};
std::atomic<bool> g_canSend{false};
std::atomic<bool> g_tiles{false};
std::atomic<bool> g_video{false};
std::atomic<int> g_bitrate{6000};
std::atomic<bool> g_keyReq{false};

// вызвать метод контроллера в потоке интерфейса
template <typename F>
void inUiThread(F f)
{
    RemoteDesktopController *c = g_controller.load();
    if (c) QMetaObject::invokeMethod(c, [c, f]() { f(c); }, Qt::QueuedConnection);
}
}

namespace AndroidCapture {

void attach(RemoteDesktopController *controller) { g_controller = controller; }

void setParams(int jpegQuality, int fps, bool canSend, bool tiles, bool video, int bitrateKbps)
{
    g_tiles = tiles;
    g_video = video;
    g_bitrate = bitrateKbps;
    g_quality = jpegQuality;
    g_fps = fps;
    g_canSend = canSend;
}

void requestKeyFrame() { g_keyReq = true; }

void request()
{
    QJniObject::callStaticMethod<void>("org/glassrice/namelessorbit/OrbitActivity", "requestCapture", "()V");
}

void stop()
{
    const QJniObject ctx(QNativeInterface::QAndroidApplication::context());
    QJniObject::callStaticMethod<void>("org/glassrice/namelessorbit/OrbitCaptureService", "stop", "(Landroid/content/Context;)V", ctx.object());
}

}

extern "C" {

JNIEXPORT void JNICALL Java_org_glassrice_namelessorbit_OrbitCaptureService_nativeFrame(JNIEnv *env, jclass, jbyteArray data, jint w, jint h)
{
    const jsize n = env->GetArrayLength(data);
    QByteArray jpeg(int(n), Qt::Uninitialized);
    env->GetByteArrayRegion(data, 0, n, reinterpret_cast<jbyte *>(jpeg.data()));
    inUiThread([jpeg, w, h](RemoteDesktopController *c) { c->onExternalFrame(jpeg, int(w), int(h)); });
}

// Кадр от аппаратного декодера: плоскости YUV 4:2:0 копируются как есть в QVideoFrame (NV12 или I420) — цвета переводит видеочип при отрисовке
// (VideoOutput), а не процессор. Копия — 3 МБ на 1080p вместо 8 МБ RGBA, преобразования на процессоре нет совсем
JNIEXPORT void JNICALL Java_org_glassrice_namelessorbit_OrbitVideoDecoder_nativeFrame(JNIEnv *env, jclass, jobject yb, jobject ub, jobject vb,
                                                                                    jint w, jint h, jint yRow, jint uvRow, jint uvPixel, jlong ptsUs, jint colorStd)
{
    const uchar *Y = static_cast<const uchar *>(env->GetDirectBufferAddress(yb));
    const uchar *U = static_cast<const uchar *>(env->GetDirectBufferAddress(ub));
    const uchar *V = static_cast<const uchar *>(env->GetDirectBufferAddress(vb));
    if (!Y || !U || !V || w <= 0 || h <= 0) return;
    const qint64 yCap = env->GetDirectBufferCapacity(yb), uCap = env->GetDirectBufferCapacity(ub), vCap = env->GetDirectBufferCapacity(vb);
    const bool nv12 = uvPixel == 2 && V == U + 1;              // U и V чередуются (UVUV…) — самый частый случай у аппаратных декодеров
    QVideoFrameFormat fmt(QSize(w, h), nv12 ? QVideoFrameFormat::Format_NV12 : QVideoFrameFormat::Format_YUV420P);
    fmt.setColorSpace(colorStd == 1 ? QVideoFrameFormat::ColorSpace_BT709 : QVideoFrameFormat::ColorSpace_BT601);
    fmt.setColorRange(QVideoFrameFormat::ColorRange_Video);
    QVideoFrame frame(fmt);
    if (!frame.map(QVideoFrame::WriteOnly)) return;
    // строка копируется не длиннее, чем есть в буфере: последняя строка цветовой плоскости у декодера бывает на байт короче
    auto copyRows = [](uchar *dst, int dstStride, const uchar *src, int srcStride, qint64 cap, int rows, int bytes) {
        for (int r = 0; r < rows; ++r) {
            const qint64 off = qint64(r) * srcStride;
            const int n = int(qBound(qint64(0), cap - off, qint64(bytes)));
            if (n > 0) std::memcpy(dst + qint64(r) * dstStride, src + off, size_t(n));
            if (n < bytes && n > 0) std::memset(dst + qint64(r) * dstStride + n, dst[qint64(r) * dstStride + n - 1], size_t(bytes - n));
        }
    };
    const int ch = (h + 1) / 2, cw = (w + 1) / 2;
    copyRows(frame.bits(0), frame.bytesPerLine(0), Y, yRow, yCap, h, w);
    if (nv12) {
        copyRows(frame.bits(1), frame.bytesPerLine(1), U, uvRow, uCap, ch, cw * 2);
    } else if (uvPixel == 1) {
        copyRows(frame.bits(1), frame.bytesPerLine(1), U, uvRow, uCap, ch, cw);
        copyRows(frame.bits(2), frame.bytesPerLine(2), V, uvRow, vCap, ch, cw);
    } else {
        // редкий шаг (не 1 и не 2): разбор по одному отсчёту
        uchar *du = frame.bits(1), *dv = frame.bits(2);
        for (int r = 0; r < ch; ++r)
            for (int x = 0; x < cw; ++x) {
                const qint64 o = qint64(r) * uvRow + qint64(x) * uvPixel;
                du[r * frame.bytesPerLine(1) + x] = o < uCap ? U[o] : 128;
                dv[r * frame.bytesPerLine(2) + x] = o < vCap ? V[o] : 128;
            }
    }
    frame.unmap();
    inUiThread([frame, ptsUs](RemoteDesktopController *c) { c->onVideoFrame(frame, qint64(ptsUs)); });
}
JNIEXPORT void JNICALL Java_org_glassrice_namelessorbit_OrbitVideoDecoder_nativeFailed(JNIEnv *env, jclass, jstring reason)
{
    QString why;
    if (reason) {
        const char *chars = env->GetStringUTFChars(reason, nullptr);
        if (chars) { why = QString::fromUtf8(chars).left(200); env->ReleaseStringUTFChars(reason, chars); }
    }
    inUiThread([why](RemoteDesktopController *c) { c->onVideoFailed(why); });
}

JNIEXPORT jboolean JNICALL Java_org_glassrice_namelessorbit_OrbitCaptureService_nativeCanSend(JNIEnv *, jclass) { return g_canSend.load() ? JNI_TRUE : JNI_FALSE; }
JNIEXPORT jboolean JNICALL Java_org_glassrice_namelessorbit_OrbitCaptureService_nativeTiles(JNIEnv *, jclass) { return g_tiles.load() ? JNI_TRUE : JNI_FALSE; }
JNIEXPORT jboolean JNICALL Java_org_glassrice_namelessorbit_OrbitCaptureService_nativeVideo(JNIEnv *, jclass) { return g_video.load() ? JNI_TRUE : JNI_FALSE; }
JNIEXPORT jint JNICALL Java_org_glassrice_namelessorbit_OrbitCaptureService_nativeBitrate(JNIEnv *, jclass) { return g_bitrate.load(); }
JNIEXPORT jboolean JNICALL Java_org_glassrice_namelessorbit_OrbitCaptureService_nativeKeyRequested(JNIEnv *, jclass) { return g_keyReq.exchange(false) ? JNI_TRUE : JNI_FALSE; }

// Готовый кадр H.264 от аппаратного кодера телефона (с SPS/PPS у ключевых) — на отправку зрителю
JNIEXPORT void JNICALL Java_org_glassrice_namelessorbit_OrbitCaptureService_nativeVideoFrame(JNIEnv *env, jclass, jbyteArray data, jint w, jint h, jboolean key)
{
    const jsize n = env->GetArrayLength(data);
    QByteArray au(int(n), Qt::Uninitialized);
    env->GetByteArrayRegion(data, 0, n, reinterpret_cast<jbyte *>(au.data()));
    inUiThread([au, w, h, key](RemoteDesktopController *c) { c->onExternalVideo(au, int(w), int(h), key == JNI_TRUE); });
}
JNIEXPORT void JNICALL Java_org_glassrice_namelessorbit_OrbitCaptureService_nativeVideoFailed(JNIEnv *env, jclass, jstring reason)
{
    QString why;
    if (reason) { const char *s = env->GetStringUTFChars(reason, nullptr); if (s) { why = QString::fromUtf8(s).left(200); env->ReleaseStringUTFChars(reason, s); } }
    inUiThread([why](RemoteDesktopController *c) { c->onExternalVideoFailed(why); });
}

// Сырой кадр экрана телефона (RGBA, шаг строки может быть больше ширины): копия уходит в общий движок плиток
JNIEXPORT void JNICALL Java_org_glassrice_namelessorbit_OrbitCaptureService_nativeRaw(JNIEnv *env, jclass, jobject buffer, jint w, jint h, jint rowStride)
{
    const uchar *data = static_cast<const uchar *>(env->GetDirectBufferAddress(buffer));
    if (!data || w <= 0 || h <= 0 || rowStride < w * 4) return;
    const QImage img = QImage(data, w, h, rowStride, QImage::Format_RGBX8888).copy();
    inUiThread([img](RemoteDesktopController *c) { c->onExternalImage(img); });
}
JNIEXPORT jint JNICALL Java_org_glassrice_namelessorbit_OrbitCaptureService_nativeQuality(JNIEnv *, jclass) { return g_quality.load(); }
JNIEXPORT jint JNICALL Java_org_glassrice_namelessorbit_OrbitCaptureService_nativeFps(JNIEnv *, jclass) { return g_fps.load(); }

JNIEXPORT void JNICALL Java_org_glassrice_namelessorbit_OrbitCaptureService_nativeStarted(JNIEnv *, jclass)
{
    inUiThread([](RemoteDesktopController *c) { c->onExternalState(1); });
}
JNIEXPORT void JNICALL Java_org_glassrice_namelessorbit_OrbitCaptureService_nativeStopped(JNIEnv *, jclass)
{
    inUiThread([](RemoteDesktopController *c) { c->onExternalState(2); });
}
JNIEXPORT void JNICALL Java_org_glassrice_namelessorbit_OrbitCaptureService_nativeDenied(JNIEnv *, jclass)
{
    inUiThread([](RemoteDesktopController *c) { c->onExternalState(3); });
}

}

#else

namespace AndroidCapture {
void attach(RemoteDesktopController *) {}
void setParams(int, int, bool, bool, bool, int) {}
void requestKeyFrame() {}
void request() {}
void stop() {}
}

#endif
