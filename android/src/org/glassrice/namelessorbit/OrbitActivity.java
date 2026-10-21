package org.glassrice.namelessorbit;

import android.content.Context;
import android.content.Intent;
import android.media.projection.MediaProjectionManager;
import android.app.PictureInPictureParams;
import android.os.Build;
import android.os.Bundle;
import android.util.Rational;
import android.content.pm.ActivityInfo;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

import org.qtproject.qt.android.bindings.QtActivity;

// Окно приложения. Отличие от QtActivity одно: получает ответ системного окна «Начать демонстрацию экрана»
// (согласие даёт человек, в обход него экран телефона не захватить) и передаёт его службе захвата
public class OrbitActivity extends QtActivity {
    private static final int REQ_CAPTURE = 7421;
    private static OrbitActivity instance;

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        instance = this;
    }

    // Приложение закрыто (смахнули из недавних, «закрыть»): всё, что работало в фоне, останавливается вместе с ним. Иначе службы живут
    // в процессе без интерфейса Qt и обращаются в уже остановленную C++ часть — отсюда ошибка «приложение остановилось» в фоне
    @Override
    protected void onDestroy() {
        if (instance == this) instance = null;
        if (isFinishing()) {
            try { OrbitCaptureService.stop(this); } catch (Throwable ignored) { }
            try { OrbitVideoDecoder.stop(); } catch (Throwable ignored) { }
            try { OrbitService.keepAwake(this, false); } catch (Throwable ignored) { }
            try { OrbitService.stop(this); } catch (Throwable ignored) { }
        }
        super.onDestroy();
    }

    // ---- картинка в картинке: просмотр чужого экрана остаётся маленьким окном, когда человек уходит в другое приложение ----
    private static boolean pipEnabled = false;
    private static int pipW = 16, pipH = 9;

    // из C++: просмотр идёт (on) и каков размер кадра — по нему подбирается пропорция окошка
    public static void setPip(boolean on, int w, int h) {
        pipEnabled = on;
        if (w > 0 && h > 0) { pipW = w; pipH = h; }
    }

    // из C++ или кнопки «В окно»: свернуть в окошко прямо сейчас
    public static void enterPipNow() {
        final OrbitActivity a = instance;
        if (a == null) return;
        a.runOnUiThread(new Runnable() { @Override public void run() { a.tryEnterPip(); } });
    }

    private void tryEnterPip() {
        if (Build.VERSION.SDK_INT < 26 || isInPictureInPictureMode()) return;
        // система принимает пропорции от 1:2.39 до 2.39:1
        double r = Math.max(1.0 / 2.39, Math.min(2.39, (double) pipW / (double) pipH));
        PictureInPictureParams.Builder b = new PictureInPictureParams.Builder().setAspectRatio(new Rational((int) Math.round(r * 1000), 1000));
        try { enterPictureInPictureMode(b.build()); } catch (Exception ignored) { }
    }

    // человек нажал «Домой» или переключился на другое приложение
    @Override
    public void onUserLeaveHint() {
        super.onUserLeaveHint();
        if (pipEnabled) tryEnterPip();
    }

    // ---- полноэкранный просмотр: без строки состояния и жестовой панели, ориентация по выбору ----
    // on — скрыть системные панели; orientation (действует и без on): 0 — как раньше (по системе), 1 — альбомная, 2 — портретная, 3 — свободная (по датчику)
    public static void setViewerFullscreen(final boolean on, final int orientation) {
        final OrbitActivity a = instance;
        if (a == null) return;
        a.runOnUiThread(new Runnable() { @Override public void run() { a.applyFullscreen(on, orientation); } });
    }

    @SuppressWarnings("deprecation")   // SYSTEM_UI_FLAG_* нужны только Android 9–10 (minSdk 28), на 11+ работает WindowInsetsController
    private void applyFullscreen(boolean on, int orientation) {
        int req = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
        if (orientation == 1) req = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE;
        else if (orientation == 2) req = ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT;
        else if (orientation == 3) req = ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR;
        setRequestedOrientation(req);
        // картинка занимает весь экран вместе с вырезом камеры и областью под панелями (иначе вокруг остаются тёмные поля)
        try {
            android.view.WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode = on ? android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
                                              : android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_DEFAULT;
            getWindow().setAttributes(lp);
            if (Build.VERSION.SDK_INT >= 30) getWindow().setDecorFitsSystemWindows(!on);
        } catch (Throwable ignored) { }
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                if (on) {
                    c.hide(WindowInsets.Type.systemBars());
                    c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                } else {
                    c.show(WindowInsets.Type.systemBars());
                }
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(on
                ? (View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE)
                : View.SYSTEM_UI_FLAG_VISIBLE);
        }
    }

    // из C++: показать системный запрос на захват экрана
    public static void requestCapture() {
        final OrbitActivity a = instance;
        if (a == null) { OrbitCaptureService.nativeDenied(); return; }
        a.runOnUiThread(new Runnable() {
            @Override public void run() {
                MediaProjectionManager m = (MediaProjectionManager) a.getSystemService(Context.MEDIA_PROJECTION_SERVICE);
                a.startActivityForResult(m.createScreenCaptureIntent(), REQ_CAPTURE);
            }
        });
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == REQ_CAPTURE) {
            if (resultCode == RESULT_OK && data != null) OrbitCaptureService.begin(this, resultCode, data);
            else OrbitCaptureService.nativeDenied();
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }
}
