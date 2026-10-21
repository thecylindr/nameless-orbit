package org.glassrice.namelessorbit;

import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Build;
import android.util.Log;

import java.nio.ByteBuffer;

// Аппаратный разбор видео H.264 от другого устройства (показ экрана компьютера на телефон): кадры Annex B приходят из C++ в feed(),
// MediaCodec разжимает их видеочипом прямо в поверхность ImageReader, готовая RGBA-картинка уходит обратно в C++ (nativeFrame).
// Это на порядок быстрее и легче процессора, чем JPEG на каждый кадр. Любой сбой — nativeFailed(): отправитель возвращается к JPEG
public class OrbitVideoDecoder {
    private static final String TAG = "OrbitVideo";
    private static final Object lock = new Object();

    private static MediaCodec codec;
    private static Thread outThread;
    private static volatile boolean running = false;
    private static int curW = 0, curH = 0;
    private static volatile long lastOutputMs = 0;
    private static long startedMs = 0;
    private static boolean failedReported = false;
    private static String lastError = "";
    private static int fed = 0;

    // плоскости YUV 4:2:0 прямо из декодера (valid только на время вызова): перевод в RGBA делает C++ в нескольких потоках
    private static native void nativeFrame(ByteBuffer y, ByteBuffer u, ByteBuffer v, int w, int h, int yRow, int uvRow, int uvPixel, long ptsUs, int colorStandard);
    private static native void nativeFailed(String reason);

    // Один кадр в разжатие. false — не принят: декодер занят, ещё не получил ключевой кадр или не запустился
    public static boolean feed(byte[] au, int w, int h, boolean key, long ptsUs) {
        synchronized (lock) {
            if (codec == null || w != curW || h != curH) {
                if (!key) return false;                  // начинать можно только с ключевого кадра (в нём SPS/PPS)
                if (!start(w, h)) { fail(lastError); return false; }
            }
            // декодер молчит уже давно после того, как ему отдали кадры — аппаратный путь на этом телефоне не работает
            final long now = System.currentTimeMillis();
            if (lastOutputMs == 0 && startedMs != 0 && now - startedMs > 3000) { fail("декодер не выдал ни одного кадра за 3 с (подано кадров: " + fed + ")"); return false; }
            try {
                int idx = codec.dequeueInputBuffer(4000);
                if (idx < 0) return false;
                ByteBuffer in = codec.getInputBuffer(idx);
                if (in == null || in.capacity() < au.length) { codec.queueInputBuffer(idx, 0, 0, ptsUs, 0); return false; }
                in.clear();
                in.put(au);
                codec.queueInputBuffer(idx, 0, au.length, ptsUs, 0);
                ++fed;
                return true;
            } catch (Exception e) {
                Log.w(TAG, "feed: " + e);
                stopLocked();
                return false;
            }
        }
    }

    // есть ли в телефоне декодер H.264, который не программный. Проверка нарочно мягкая: любой не «только программный» декодер подходит,
    // а сбой самой проверки считается «есть» — если декодер на деле не заработает, показ сам вернётся к тайлам (см. fail), а вот ложное «нет»
    // отнимает у человека видео совсем
    public static boolean hasHardwareH264() {
        try {
            boolean any = false;
            for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
                if (info.isEncoder()) continue;
                boolean avc = false;
                for (String t : info.getSupportedTypes()) if (t.equalsIgnoreCase("video/avc")) avc = true;
                if (!avc) continue;
                any = true;
                final String name = info.getName().toLowerCase();
                boolean software;
                if (Build.VERSION.SDK_INT >= 29) software = info.isSoftwareOnly() && !info.isHardwareAccelerated();
                else software = name.startsWith("omx.google.") || name.startsWith("c2.android.");
                if (!software) { Log.i(TAG, "декодер H.264: " + info.getName()); return true; }
            }
            Log.w(TAG, "аппаратного декодера H.264 не найдено (всего декодеров avc: " + (any ? "есть, но программные" : "нет") + ")");
            return false;
        } catch (Throwable t) {
            Log.w(TAG, "проверка декодера не удалась, считаем что есть: " + t);
            return true;
        }
    }

    public static void stop() {
        synchronized (lock) { stopLocked(); failedReported = false; }      // новый просмотр — сбой снова можно сообщить
    }

    // причина уходит в C++ и показывается человеку (и отправителю): почему показ остался на JPEG
    private static void fail(String reason) {
        Log.w(TAG, "сбой видео: " + reason);
        stopLocked();
        if (!failedReported) { failedReported = true; try { nativeFailed(reason); } catch (Throwable ignored) { } }
    }

    private static boolean start(int w, int h) {
        stopLocked();
        try {
            MediaFormat fmt = MediaFormat.createVideoFormat("video/avc", w, h);
            fmt.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 2 * 1024 * 1024);
            if (Build.VERSION.SDK_INT >= 30) fmt.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);       // без накопления кадров в декодере
            fmt.setInteger(MediaFormat.KEY_PRIORITY, 0);                                           // реальное время
            codec = MediaCodec.createDecoderByType("video/avc");
            // без поверхности: кадры читаются как Image YUV 4:2:0 в любом внутреннем формате чипа (фирменные форматы ImageReader отвергает)
            fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
            codec.configure(fmt, null, null, 0);
            codec.start();

            curW = w; curH = h;
            lastOutputMs = 0;
            startedMs = System.currentTimeMillis();
            running = true;
            final MediaCodec c = codec;
            outThread = new Thread(new Runnable() {
                @Override public void run() {
                    MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
                    int colorStd = 0;          // 1 — BT.709, 2/4 — BT.601 (MediaFormat.COLOR_STANDARD_*), 0 — неизвестно
                    while (running && c == codec) {
                        try {
                            int i = c.dequeueOutputBuffer(info, 20000);
                            if (i == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                                try { MediaFormat of = c.getOutputFormat(); if (of.containsKey(MediaFormat.KEY_COLOR_STANDARD)) colorStd = of.getInteger(MediaFormat.KEY_COLOR_STANDARD); } catch (Throwable ignored) { }
                            }
                            if (i >= 0) {
                                Image img = null;
                                try {
                                    img = c.getOutputImage(i);
                                    if (img != null) {
                                        Image.Plane[] p = img.getPlanes();
                                        lastOutputMs = System.currentTimeMillis();
                                        nativeFrame(p[0].getBuffer(), p[1].getBuffer(), p[2].getBuffer(), img.getWidth(), img.getHeight(),
                                                    p[0].getRowStride(), p[1].getRowStride(), p[1].getPixelStride(), info.presentationTimeUs, colorStd);
                                    }
                                } catch (Throwable t) {
                                    Log.w(TAG, "image: " + t);
                                } finally {
                                    if (img != null) img.close();
                                    c.releaseOutputBuffer(i, false);
                                }
                            }
                        } catch (Throwable t) {
                            break;       // кодек остановлен
                        }
                    }
                }
            }, "orbit-video-out");
            outThread.start();
            return true;
        } catch (Throwable t) {
            Log.w(TAG, "start: " + t);
            lastError = "не запустился: " + t;
            stopLocked();
            return false;
        }
    }

    private static void stopLocked() {
        running = false;
        curW = curH = 0;
        startedMs = 0;
        lastOutputMs = 0;
        fed = 0;
        if (outThread != null) { try { outThread.join(200); } catch (InterruptedException ignored) { } outThread = null; }
        if (codec != null) {
            try { codec.stop(); } catch (Throwable ignored) { }
            try { codec.release(); } catch (Throwable ignored) { }
            codec = null;
        }
    }
}
