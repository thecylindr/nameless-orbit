package org.glassrice.namelessorbit;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.media.MediaMetadata;
import android.media.session.MediaController;
import android.media.session.MediaSessionManager;
import android.media.session.PlaybackState;
import android.provider.Settings;
import android.service.notification.NotificationListenerService;
import android.util.Base64;
import java.io.ByteArrayOutputStream;
import java.util.List;
import org.json.JSONObject;

// Что играет на телефоне — для компьютера (Orbit → NameLess UI). Название, исполнитель, альбом, обложка, позиция трека
// доступны только через медиасессии системы, а их Android отдаёт приложению с доступом к уведомлениям
// (Настройки → «Доступ к уведомлениям» → Orbit). Без доступа — read() отдаёт { access: false }, и остаётся «играет / нет».
// Команды плееру идут прямо в его сессию (точнее медиа-клавиш: попадают в нужный плеер, а не в последний активный)
public class OrbitMedia {
    // служба нужна только для разрешения: сами уведомления Orbit не читает
    public static class Listener extends NotificationListenerService { }

    private static ComponentName component(Context c) { return new ComponentName(c, Listener.class); }

    public static boolean hasAccess(Context c) {
        String list = Settings.Secure.getString(c.getContentResolver(), "enabled_notification_listeners");
        return list != null && list.contains(c.getPackageName());
    }

    public static void openAccessSettings(Context c) {
        Intent i = new Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        c.startActivity(i);
    }

    // играющий плеер, иначе первый с сессией
    private static MediaController current(Context c) {
        if (!hasAccess(c)) return null;
        try {
            MediaSessionManager m = (MediaSessionManager) c.getSystemService(Context.MEDIA_SESSION_SERVICE);
            List<MediaController> list = m.getActiveSessions(component(c));
            MediaController first = null;
            for (MediaController mc : list) {
                PlaybackState st = mc.getPlaybackState();
                if (st != null && st.getState() == PlaybackState.STATE_PLAYING) return mc;
                if (first == null) first = mc;
            }
            return first;
        } catch (SecurityException e) {
            return null;
        }
    }

    private static String lastArtKey = "";
    private static String lastArt = "";

    // JSON: { access, title, artist, album, app, playing, pos, len, art (JPEG base64, только при смене трека), artKey }
    public static String read(Context c) {
        JSONObject o = new JSONObject();
        try {
            o.put("access", hasAccess(c));
            MediaController mc = current(c);
            if (mc == null) return o.toString();
            MediaMetadata md = mc.getMetadata();
            PlaybackState st = mc.getPlaybackState();
            String title = md != null ? md.getString(MediaMetadata.METADATA_KEY_TITLE) : null;
            String artist = md != null ? md.getString(MediaMetadata.METADATA_KEY_ARTIST) : null;
            if (artist == null && md != null) artist = md.getString(MediaMetadata.METADATA_KEY_ALBUM_ARTIST);
            o.put("title", title != null ? title : "");
            o.put("artist", artist != null ? artist : "");
            o.put("album", md != null && md.getString(MediaMetadata.METADATA_KEY_ALBUM) != null ? md.getString(MediaMetadata.METADATA_KEY_ALBUM) : "");
            o.put("len", md != null ? md.getLong(MediaMetadata.METADATA_KEY_DURATION) : 0);
            o.put("playing", st != null && st.getState() == PlaybackState.STATE_PLAYING);
            o.put("pos", st != null ? st.getPosition() : 0);
            try {
                PackageManager pm = c.getPackageManager();
                ApplicationInfo ai = pm.getApplicationInfo(mc.getPackageName(), 0);
                o.put("app", pm.getApplicationLabel(ai).toString());
            } catch (Exception e) { o.put("app", mc.getPackageName()); }
            // обложка: маленький JPEG, кодируется заново только при смене трека
            String key = mc.getPackageName() + "|" + title + "|" + artist;
            if (!key.equals(lastArtKey)) {
                lastArtKey = key; lastArt = "";
                Bitmap b = null;
                if (md != null) {
                    b = md.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART);
                    if (b == null) b = md.getBitmap(MediaMetadata.METADATA_KEY_ART);
                    if (b == null) b = md.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON);
                }
                if (b != null) {
                    Bitmap s = Bitmap.createScaledBitmap(b, 128, 128 * b.getHeight() / Math.max(1, b.getWidth()), true);
                    ByteArrayOutputStream out = new ByteArrayOutputStream();
                    s.compress(Bitmap.CompressFormat.JPEG, 78, out);
                    lastArt = Base64.encodeToString(out.toByteArray(), Base64.NO_WRAP);
                }
            }
            o.put("artKey", lastArtKey);
            o.put("art", lastArt);
        } catch (Exception ignored) { }
        return o.toString();
    }

    // команда плееру: playpause | next | previous; false — сессии нет (тогда C++ шлёт медиа-клавишу)
    public static boolean command(Context c, String action) {
        MediaController mc = current(c);
        if (mc == null) return false;
        MediaController.TransportControls t = mc.getTransportControls();
        if ("next".equals(action)) t.skipToNext();
        else if ("previous".equals(action)) t.skipToPrevious();
        else {
            PlaybackState st = mc.getPlaybackState();
            if (st != null && st.getState() == PlaybackState.STATE_PLAYING) t.pause(); else t.play();
        }
        return true;
    }

    // яркость экрана, 0..100 (системная настройка 0..255); -1 — недоступна
    public static int brightness(Context c) {
        try {
            return Math.round(Settings.System.getInt(c.getContentResolver(), Settings.System.SCREEN_BRIGHTNESS) * 100f / 255f);
        } catch (Exception e) {
            return -1;
        }
    }
}
