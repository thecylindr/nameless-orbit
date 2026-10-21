package org.glassrice.namelessorbit;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.PixelFormat;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.util.DisplayMetrics;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;

// Демонстрация экрана телефона на другое устройство. Служба переднего плана (так требует система для захвата экрана):
// сжатые JPEG-кадры отдаются в C++ (RemoteDesktopController), который шлёт их по защищённому каналу. Качество и частота
// подстраиваются под сеть на стороне C++: сюда приходят готовые числа, а кадры пропускаются, пока очередь отправки полна
public class OrbitCaptureService extends Service {
    private static final String CHANNEL = "orbit_capture";
    private static final int NOTIFICATION_ID = 2;
    private static final int LONG_SIDE = 1280;

    private static OrbitCaptureService running;

    private MediaProjection projection;
    private VirtualDisplay display;
    private ImageReader reader;
    // onFrame (поток захвата) и shutdown (может прийти из главного потока: остановка службы) не должны идти одновременно: reader.close() и recycle()
    // освобождают память, из которой в этот момент копируются пиксели, — это и был краш SIGSEGV в copyPixelsFromBuffer
    private final Object frameLock = new Object();
    private volatile boolean closed = false;
    private HandlerThread thread;
    private long lastFrameMs = 0;
    private Bitmap padded, frame;
    private Canvas frameCanvas;
    private final android.graphics.Rect cropRect = new android.graphics.Rect();
    private final ByteArrayOutputStream jpegOut = new ByteArrayOutputStream(96 * 1024);
    private int w, h;
    private Handler handler;
    // ---- видео H.264: тот же захват экрана, но кадры идут в аппаратный кодер (MediaCodec) и уходят готовым потоком ----
    private MediaCodec enc;
    private Surface encSurface;
    private Thread encThread;
    private volatile boolean videoOn = false;
    private byte[] csd;            // SPS/PPS кодера: добавляются перед каждым ключевым кадром, как нужно зрителю
    private int lastBitrate = 0;

    // ---- вызывается из C++ (через OrbitActivity) ----
    public static native void nativeFrame(byte[] jpeg, int width, int height);
    public static native boolean nativeCanSend();
    public static native boolean nativeTiles();
    public static native void nativeRaw(ByteBuffer rgba, int w, int h, int rowStride);
    public static native boolean nativeVideo();                 // C++ просит видео вместо кадров целиком
    public static native int nativeBitrate();                   // желаемая скорость, кбит/с (лесенка качества)
    public static native boolean nativeKeyRequested();          // зритель потерял картинку — нужен ключевой кадр
    public static native void nativeVideoFrame(byte[] au, int w, int h, boolean key);
    public static native void nativeVideoFailed(String reason);
    public static native int nativeQuality();
    public static native int nativeFps();
    public static native void nativeStarted();
    public static native void nativeStopped();
    public static native void nativeDenied();

    public static void begin(Context c, int resultCode, Intent data) {
        Intent i = new Intent(c, OrbitCaptureService.class);
        i.putExtra("code", resultCode);
        i.putExtra("data", data);
        if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i); else c.startService(i);
    }

    public static void stop(Context c) { c.stopService(new Intent(c, OrbitCaptureService.class)); }

    @Override public IBinder onBind(Intent intent) { return null; }

    @SuppressWarnings("deprecation")      // getParcelableExtra(String): типизированный вариант есть только с Android 13, а minSdk — 28
    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null || running != null) return START_NOT_STICKY;
        running = this;
        closed = false;

        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Показ экрана", NotificationManager.IMPORTANCE_LOW));
        Intent launch = getPackageManager().getLaunchIntentForPackage(getPackageName());
        Notification n = new Notification.Builder(this, CHANNEL)
            .setContentTitle("Орбита показывает экран телефона")
            .setContentText("Нажмите, чтобы открыть Orbit и остановить показ")
            .setSmallIcon(smallIcon())
            .setContentIntent(PendingIntent.getActivity(this, 1, launch, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT))
            .setOngoing(true)
            .build();
        // тип «захват экрана» обязан быть объявлен до получения MediaProjection (Android 14+)
        if (Build.VERSION.SDK_INT >= 29) startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION);
        else startForeground(NOTIFICATION_ID, n);

        try {
            MediaProjectionManager mpm = (MediaProjectionManager) getSystemService(Context.MEDIA_PROJECTION_SERVICE);
            projection = mpm.getMediaProjection(intent.getIntExtra("code", 0), (Intent) intent.getParcelableExtra("data"));
            if (projection == null) throw new IllegalStateException("нет разрешения");

            DisplayMetrics dm = getResources().getDisplayMetrics();
            // размер кадра — по длинной стороне экрана (у планшета в альбомной ориентации ширина больше высоты); чётные числа нужны кодеку
            float scale = Math.min(1f, (float) LONG_SIDE / Math.max(dm.widthPixels, dm.heightPixels));
            w = (Math.round(dm.widthPixels * scale) / 2) * 2;
            h = (Math.round(dm.heightPixels * scale) / 2) * 2;
            cropRect.set(0, 0, w, h);
            thread = new HandlerThread("orbit-capture");
            thread.start();
            handler = new Handler(thread.getLooper());

            projection.registerCallback(new MediaProjection.Callback() {
                @Override public void onStop() { shutdown(true); }       // человек остановил показ системной кнопкой
            }, handler);

            reader = ImageReader.newInstance(w, h, PixelFormat.RGBA_8888, 2);
            display = projection.createVirtualDisplay("orbit", w, h, dm.densityDpi, DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                                                      reader.getSurface(), null, handler);
            reader.setOnImageAvailableListener(new ImageReader.OnImageAvailableListener() {
                @Override public void onImageAvailable(ImageReader r) { onFrame(r); }
            }, handler);
            handler.postDelayed(modeTick, 300);
            nativeStarted();
        } catch (Exception e) {
            shutdown(false);
            nativeDenied();
        }
        return START_NOT_STICKY;
    }

    // Раз в 0,4 с: включить или выключить видео по просьбе C++, подстроить скорость, выдать ключевой кадр по просьбе зрителя
    private final Runnable modeTick = new Runnable() {
        @Override public void run() {
            synchronized (frameLock) {
                if (closed) return;
                try {
                    final boolean want = nativeVideo();
                    if (want && !videoOn) startVideo();
                    else if (!want && videoOn) stopVideo();
                    if (videoOn && enc != null) {
                        final int br = nativeBitrate();
                        if (br > 0 && Math.abs(br - lastBitrate) * 6 > lastBitrate) {
                            Bundle b = new Bundle();
                            b.putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, br * 1000);
                            enc.setParameters(b);
                            lastBitrate = br;
                        }
                        if (nativeKeyRequested()) {
                            Bundle b = new Bundle();
                            b.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
                            enc.setParameters(b);
                        }
                    }
                } catch (Throwable t) {
                    Log.w("OrbitVideo", "тик видео: " + t);
                }
                if (handler != null) handler.postDelayed(this, 400);
            }
        }
    };

    private void startVideo() {
        try {
            MediaFormat f = MediaFormat.createVideoFormat("video/avc", w, h);
            f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            int br = nativeBitrate();
            if (br <= 0) br = 6000;
            f.setInteger(MediaFormat.KEY_BIT_RATE, br * 1000);
            f.setInteger(MediaFormat.KEY_FRAME_RATE, 30);
            f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);                  // ключевой кадр раз в секунду: с него зритель начинает
            f.setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 200000);      // экран замер — кодер повторяет кадр, чтобы поток не молчал
            f.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR);
            f.setInteger(MediaFormat.KEY_PRIORITY, 0);
            if (Build.VERSION.SDK_INT >= 30) f.setInteger(MediaFormat.KEY_LATENCY, 1);
            f.setFloat("max-fps-to-encoder", Math.max(15, nativeFps()));         // экран 120 Гц не нужно гнать в сеть на полной частоте
            enc = MediaCodec.createEncoderByType("video/avc");
            enc.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            encSurface = enc.createInputSurface();
            enc.start();
            display.setSurface(encSurface);
            lastBitrate = br;
            csd = null;
            videoOn = true;
            final MediaCodec c = enc;
            encThread = new Thread(new Runnable() {
                @Override public void run() {
                    MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
                    while (videoOn && c == enc && !closed) {
                        try {
                            int i = c.dequeueOutputBuffer(info, 50000);
                            if (i < 0) continue;
                            ByteBuffer ob = c.getOutputBuffer(i);
                            if (ob != null && info.size > 0) {
                                byte[] d = new byte[info.size];
                                ob.position(info.offset);
                                ob.get(d);
                                if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) csd = d;
                                else {
                                    final boolean key = (info.flags & MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0;
                                    if (key && csd != null) { byte[] k = new byte[csd.length + d.length]; System.arraycopy(csd, 0, k, 0, csd.length); System.arraycopy(d, 0, k, csd.length, d.length); d = k; }
                                    nativeVideoFrame(d, w, h, key);
                                }
                            }
                            c.releaseOutputBuffer(i, false);
                        } catch (Throwable t) { break; }
                    }
                }
            }, "orbit-video-enc");
            encThread.start();
        } catch (Throwable t) {
            Log.w("OrbitVideo", "кодер не запустился: " + t);
            stopVideo();
            nativeVideoFailed("кодер H.264 телефона не запустился: " + t);
        }
    }

    // вернуть захват на кадры целиком и освободить кодер (вызывается под frameLock)
    private void stopVideo() {
        videoOn = false;
        if (encThread != null) { try { encThread.join(300); } catch (InterruptedException ignored) { } encThread = null; }
        try { if (display != null && reader != null) display.setSurface(reader.getSurface()); } catch (Throwable ignored) { }
        if (enc != null) { try { enc.stop(); } catch (Throwable ignored) { } try { enc.release(); } catch (Throwable ignored) { } enc = null; }
        if (encSurface != null) { try { encSurface.release(); } catch (Throwable ignored) { } encSurface = null; }
    }

    private void onFrame(ImageReader r) {
        synchronized (frameLock) {
            if (closed) return;
            onFrameLocked(r);
        }
    }

    private void onFrameLocked(ImageReader r) {
        Image image = null;
        try {
            image = r.acquireLatestImage();
            if (image == null || videoOn) return;
            long now = System.currentTimeMillis();
            int fps = Math.max(1, nativeFps());
            if (now - lastFrameMs < 1000L / fps || !nativeCanSend()) return;      // слишком рано или сеть не успевает — кадр не сжимаем
            lastFrameMs = now;

            Image.Plane plane = image.getPlanes()[0];
            ByteBuffer buffer = plane.getBuffer();
            // «Тайлы Orbit»: кадр уходит в C++ сырым (RGBA), плитки и сжатие делает общий движок Orbit — тот же, что на ПК
            if (nativeTiles()) { nativeRaw(buffer, w, h, plane.getRowStride()); return; }
            int pixelStride = plane.getPixelStride();
            int rowPadding = plane.getRowStride() - pixelStride * w;
            int paddedW = w + rowPadding / pixelStride;
            // Растровые картинки и буфер создаются один раз и переиспользуются: раньше на каждый кадр выделялось по ~6 МБ, и сборщик мусора
            // останавливал показ примерно раз в секунду — «лаг каждую секунду»
            if (padded == null || padded.getWidth() != paddedW) {
                if (padded != null) padded.recycle();
                padded = Bitmap.createBitmap(paddedW, h, Bitmap.Config.ARGB_8888);
            }
            padded.copyPixelsFromBuffer(buffer);
            Bitmap toCompress = padded;
            if (paddedW != w) {                                    // лишние пиксели строки обрезаются рисованием в готовую картинку
                if (frame == null) { frame = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888); frameCanvas = new Canvas(frame); }
                frameCanvas.drawBitmap(padded, cropRect, cropRect, null);
                toCompress = frame;
            }
            jpegOut.reset();
            toCompress.compress(Bitmap.CompressFormat.JPEG, Math.max(20, Math.min(90, nativeQuality())), jpegOut);
            nativeFrame(jpegOut.toByteArray(), w, h);
        } catch (Exception ignored) {
        } finally {
            if (image != null) image.close();
        }
    }

    @SuppressWarnings("deprecation")      // stopForeground(boolean) — единственный вариант на Android 9–11
    private void shutdown(boolean notifyNative) {
        synchronized (frameLock) {
            closed = true;
            shutdownLocked();
        }
        if (notifyNative) nativeStopped();
    }

    @SuppressWarnings("deprecation")
    private void shutdownLocked() {
        if (videoOn || enc != null) stopVideo();
        handler = null;
        if (display != null) { display.release(); display = null; }
        if (reader != null) { reader.setOnImageAvailableListener(null, null); reader.close(); reader = null; }
        if (projection != null) { projection.stop(); projection = null; }
        if (thread != null) { thread.quitSafely(); thread = null; }
        if (padded != null) { padded.recycle(); padded = null; }
        if (frame != null) { frame.recycle(); frame = null; frameCanvas = null; }
        running = null;
        stopForeground(true);
        stopSelf();
    }

    // карточку приложения смахнули: показ экрана останавливается без обращений к C++ (его уже нет)
    @Override
    public void onTaskRemoved(Intent rootIntent) {
        if (running == this) shutdown(false);
        stopSelf();
        super.onTaskRemoved(rootIntent);
    }

    @Override
    public void onDestroy() {
        if (running == this) shutdown(false);
        super.onDestroy();
    }

    private int smallIcon() {
        int id = getResources().getIdentifier("ic_stat_orbit", "drawable", getPackageName());
        return id != 0 ? id : android.R.drawable.stat_notify_sync;
    }
}
