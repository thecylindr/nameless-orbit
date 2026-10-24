package org.glassrice.namelessorbit;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Environment;
import android.os.IBinder;
import android.os.PowerManager;
import android.provider.MediaStore;
import android.provider.Settings;
import android.net.Uri;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;

// Фоновая работа Orbit на Android: служба переднего плана с постоянным уведомлением не даёт системе усыпить процесс,
// пока приложение свёрнуто, и держит Wi-Fi активным, чтобы связь с компьютером не рвалась при выключенном экране.
// Сама связь живёт в Qt-части приложения; служба только удерживает процесс. Статические методы вызываются из C++ (PlatformBridge)
public class OrbitService extends Service {
    private static final String CH_SERVICE = "orbit_service";
    // канал скрытого уведомления: создаётся сразу выключенным (IMPORTANCE_NONE) — служба переднего плана работает, а в шторке и строке
    // состояния ничего нет. Важность канала приложение поднять потом не может, поэтому «видно / не видно» — два разных канала
    private static final String CH_HIDDEN = "orbit_service_hidden";
    private static boolean quiet = true;          // уведомление о работе в фоне скрыто (настройка Orbit, по умолчанию)
    private static final String CH_EVENTS = "orbit_events";
    private static final int ID_SERVICE = 1;
    private static int nextEventId = 100;

    private WifiManager.WifiLock wifiLock;
    private static String status = "Ищу устройства…";       // текст постоянного уведомления: пустым оно не бывает
    private static boolean alive = false;

    private static Notification buildOngoing(Context c) {
        return new Notification.Builder(c, quiet ? CH_HIDDEN : CH_SERVICE)
            .setContentTitle("NameLess Orbit")
            .setContentText(status)
            .setSmallIcon(smallIcon(c))
            .setContentIntent(openAppIntent(c))
            .setOngoing(true)
            .setVisibility(Notification.VISIBILITY_SECRET)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            .build();
    }

    // из C++: связь изменилась — строка уведомления обновляется (без звука и без нового значка)
    public static void setStatus(Context c, String text) {
        if (text == null || text.trim().isEmpty()) return;
        status = text;
        if (!alive) return;
        try { ((NotificationManager) c.getSystemService(Context.NOTIFICATION_SERVICE)).notify(ID_SERVICE, buildOngoing(c)); } catch (RuntimeException ignored) { }
    }

    @Override public IBinder onBind(Intent intent) { return null; }

    @SuppressWarnings("deprecation")   // WIFI_MODE_FULL_HIGH_PERF нужен только Android 9 (minSdk 28)
    @Override public void onCreate() {
        super.onCreate();
        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        // тихий канал для постоянного уведомления службы
        nm.createNotificationChannel(new NotificationChannel(CH_SERVICE, "Фоновая работа", NotificationManager.IMPORTANCE_MIN));
        nm.createNotificationChannel(new NotificationChannel(CH_HIDDEN, "Фоновая работа (скрыто)", NotificationManager.IMPORTANCE_NONE));

        Notification n = buildOngoing(this);
        // Android 12+ не даёт службе стать «переднего плана», если её запустила система из фона (перезапуск после убийства процесса, спящий режим):
        // раньше исключение в onCreate убивало приложение, и система запускала службу снова — вылеты шли по кругу. Теперь служба просто
        // не стартует; при следующем выходе приложения на экран C++ запустит её уже из переднего плана
        try {
            if (Build.VERSION.SDK_INT >= 29)
                startForeground(ID_SERVICE, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
            else
                startForeground(ID_SERVICE, n);
        } catch (RuntimeException e) {
            android.util.Log.w("OrbitService", "фоновая служба не запущена: " + e);
            stopSelf();
            return;
        }
        alive = true;

        // Wi-Fi не засыпает вместе с экраном
        WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wm != null) {
            wifiLock = wm.createWifiLock(Build.VERSION.SDK_INT >= 29 ? WifiManager.WIFI_MODE_FULL_LOW_LATENCY : WifiManager.WIFI_MODE_FULL_HIGH_PERF, "orbit");
            wifiLock.setReferenceCounted(false);
            wifiLock.acquire();
        }
    }

    // не START_STICKY: систему не нужно просить перезапускать службу из фона — это запрещено правилами Android 12+ и вело к вылетам по кругу
    @Override public int onStartCommand(Intent intent, int flags, int startId) { return START_NOT_STICKY; }

    // карточку приложения смахнули: служба не должна держать процесс без интерфейса
    @Override public void onTaskRemoved(Intent rootIntent) { stopSelf(); super.onTaskRemoved(rootIntent); }

    @Override public void onDestroy() {
        alive = false;
        keepSync(this, false);
        if (wifiLock != null && wifiLock.isHeld()) wifiLock.release();
        super.onDestroy();
    }

    // значок в строке состояния: свой (res/drawable/ic_stat_orbit.xml), запасной — системный; нулевой id роняет приложение
    // ошибкой «Bad notification for startForeground»
    private static int smallIcon(Context c) {
        int id = c.getResources().getIdentifier("ic_stat_orbit", "drawable", c.getPackageName());
        return id != 0 ? id : android.R.drawable.stat_notify_sync;
    }

    private static PendingIntent openAppIntent(Context c) {
        Intent i = c.getPackageManager().getLaunchIntentForPackage(c.getPackageName());
        return PendingIntent.getActivity(c, 0, i, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    // ---------- вызывается из C++ ----------
    // quiet — уведомление о работе в фоне не показывается (выключенный канал)
    public static void start(Context c, boolean quietNotice) {
        quiet = quietNotice;
        Intent i = new Intent(c, OrbitService.class);
        try {
            if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i); else c.startService(i);
        } catch (RuntimeException e) {       // приложение в фоне — запуск запрещён системой; повторится, когда оно выйдет на экран
            android.util.Log.w("OrbitService", "запуск службы запрещён системой: " + e);
        }
    }

    public static void stop(Context c) { c.stopService(new Intent(c, OrbitService.class)); }

    // уведомление о событии: запрос сопряжения, уведомление с другого устройства
    public static void notifyEvent(Context c, String title, String body) {
        // пустое уведомление в шторке бессмысленно: пустой заголовок заменяется названием приложения, пустой текст — заголовком
        if (title == null || title.trim().isEmpty()) title = "NameLess Orbit";
        if (body == null || body.trim().isEmpty()) body = title;
        NotificationManager nm = (NotificationManager) c.getSystemService(Context.NOTIFICATION_SERVICE);
        nm.createNotificationChannel(new NotificationChannel(CH_EVENTS, "События Orbit", NotificationManager.IMPORTANCE_HIGH));
        // обычное уведомление в шторке: остаётся там, пока его не смахнут; группируется по приложению, касание открывает Orbit
        Notification n = new Notification.Builder(c, CH_EVENTS)
            .setCategory(Notification.CATEGORY_MESSAGE)
            .setWhen(System.currentTimeMillis())
            .setShowWhen(true)
            .setGroup("orbit-events")
            .setContentTitle(title)
            .setContentText(body)
            .setStyle(new Notification.BigTextStyle().bigText(body))
            .setSmallIcon(smallIcon(c))
            .setContentIntent(openAppIntent(c))
            .setAutoCancel(true)
            .build();
        nm.notify(nextEventId++, n);
    }

    // пока идёт просмотр или показ экрана: процессор не засыпает (частичная блокировка, кадры идут и при выключенном экране)
    private static PowerManager.WakeLock streamLock;
    public static synchronized void keepAwake(Context c, boolean on) {
        if (on) {
            if (streamLock == null) {
                PowerManager pm = (PowerManager) c.getSystemService(Context.POWER_SERVICE);
                streamLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "orbit:stream");
                streamLock.setReferenceCounted(false);
            }
            if (!streamLock.isHeld()) streamLock.acquire(6 * 60 * 60 * 1000L);       // на всякий случай — не дольше шести часов
        } else if (streamLock != null && streamLock.isHeld()) {
            streamLock.release();
        }
    }

    // синхронизация в фоне: пока есть связь с устройством, процессор не засыпает при выключенном экране — иначе Orbit пропускал
    // проверки связи, компьютер считал телефон пропавшим, и буфер обмена, уведомления и файлы не доходили до включения экрана
    private static PowerManager.WakeLock syncLock;
    public static synchronized void keepSync(Context c, boolean on) {
        if (on) {
            if (syncLock == null) {
                PowerManager pm = (PowerManager) c.getSystemService(Context.POWER_SERVICE);
                syncLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "orbit:sync");
                syncLock.setReferenceCounted(false);
            }
            if (!syncLock.isHeld()) syncLock.acquire(12 * 60 * 60 * 1000L);       // страховка — не дольше 12 часов подряд
        } else if (syncLock != null && syncLock.isHeld()) {
            syncLock.release();
        }
    }

    // работает ли Orbit без ограничений батареи (оптимизация отключена)
    public static boolean batteryUnrestricted(Context c) {
        PowerManager pm = (PowerManager) c.getSystemService(Context.POWER_SERVICE);
        return pm != null && pm.isIgnoringBatteryOptimizations(c.getPackageName());
    }

    // системный вопрос «Разрешить работу в фоне?» сразу для Orbit; не открылся (прошивка без него) — общий список приложений
    @android.annotation.SuppressLint("BatteryLife")
    public static void requestBatteryUnrestricted(Context c) {
        if (batteryUnrestricted(c)) return;
        try {
            Intent i = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:" + c.getPackageName()));
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            c.startActivity(i);
        } catch (RuntimeException e) {
            openBatterySettings(c);
        }
    }

    // окно «не ограничивать батарею»: без него некоторые прошивки усыпляют приложение в фоне вопреки службе
    public static void openBatterySettings(Context c) {
        Intent i = new Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        c.startActivity(i);
    }

    // принятый файл в «Загрузки»/Orbit: Android 10+ — через MediaStore (разрешение на хранилище не нужно), ниже — в папку приложения
    public static String publishDownload(Context c, String src, String name) {
        try {
            File in = new File(src);
            if (Build.VERSION.SDK_INT >= 29) {
                ContentValues v = new ContentValues();
                v.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
                v.put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/Orbit");
                Uri uri = c.getContentResolver().insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v);
                if (uri == null) return "";
                try (InputStream is = new FileInputStream(in); OutputStream os = c.getContentResolver().openOutputStream(uri)) {
                    byte[] buf = new byte[65536];
                    int n;
                    while ((n = is.read(buf)) > 0) os.write(buf, 0, n);
                }
                in.delete();
                return "Download/Orbit/" + name;
            }
            File dir = new File(c.getExternalFilesDir(null), "Orbit");
            dir.mkdirs();
            File out = new File(dir, name);
            try (InputStream is = new FileInputStream(in); OutputStream os = new java.io.FileOutputStream(out)) {
                byte[] buf = new byte[65536];
                int n;
                while ((n = is.read(buf)) > 0) os.write(buf, 0, n);
            }
            in.delete();
            return out.getAbsolutePath();
        } catch (Exception e) {
            return "";
        }
    }

    // включены ли уведомления приложения (разрешение Android 13+ и переключатель в настройках системы)
    public static boolean notificationsEnabled(Context c) {
        return ((NotificationManager) c.getSystemService(Context.NOTIFICATION_SERVICE)).areNotificationsEnabled();
    }

    // настройки уведомлений Orbit в системе: если человек запретил их, вернуть можно только там
    public static void openNotificationSettings(Context c) {
        Intent i = new Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS);
        i.putExtra(Settings.EXTRA_APP_PACKAGE, c.getPackageName());
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        c.startActivity(i);
    }

    // Android 13+: без разрешения уведомления (в том числе постоянное) не показываются
    public static void requestNotificationPermission(Context c) {
        if (Build.VERSION.SDK_INT < 33 || !(c instanceof Activity)) return;
        if (c.checkSelfPermission("android.permission.POST_NOTIFICATIONS") == PackageManager.PERMISSION_GRANTED) return;
        ((Activity) c).requestPermissions(new String[] { "android.permission.POST_NOTIFICATIONS" }, 1);
    }
}
