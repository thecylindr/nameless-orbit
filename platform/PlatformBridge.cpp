#include <QGuiApplication>
#include <QElapsedTimer>
#include "PlatformBridge.h"

#if defined(__ANDROID__)
#include <QJniObject>
#include <QJniEnvironment>
#include <QCoreApplication>
#elif defined(_WIN32)
#include <windows.h>
#elif defined(ORBIT_DESKTOP_LINUX)
#include <QDBusInterface>
#include <QDBusConnection>
#include <QVariantMap>
#endif

#include <QSysInfo>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>


namespace {
// Уведомление системы нужно, пока человек не в окне Orbit: в окне то же самое он уже видит (диалог, плашка внизу). Повтор одного и того же текста
// подряд (например, запрос пришёл дважды) не показывается
bool shouldPostNotification(const QString &title, const QString &body, bool force)
{
    static QString lastKey;
    static QElapsedTimer lastAt;
    const QString key = title + QLatin1Char('\n') + body;
    if (!force) {
        if (QGuiApplication::applicationState() == Qt::ApplicationActive) return false;
        if (key == lastKey && lastAt.isValid() && lastAt.elapsed() < 5000) return false;
    }
    lastKey = key;
    lastAt.start();
    return true;
}
}
namespace PlatformBridge {

#if !defined(__ANDROID__)
namespace {
// ~/Downloads/Orbit/<имя>; если такое имя занято — «имя (1).расш»; файл переносится, а не копируется
QString moveToDownloads(const QString &src, const QString &name)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + QStringLiteral("/Orbit");
    if (!QDir().mkpath(dir)) return QString();
    QString target = dir + QLatin1Char('/') + name;
    if (QFile::exists(target)) {
        const QFileInfo fi(name);
        for (int i = 1; i < 1000 && QFile::exists(target); ++i)
            target = dir + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral(" (%1)").arg(i) + (fi.suffix().isEmpty() ? QString() : QLatin1Char('.') + fi.suffix());
    }
    if (QFile::rename(src, target)) return target;
    if (QFile::copy(src, target)) { QFile::remove(src); return target; }      // другой раздел — копированием
    return QString();
}
}
#endif

#if defined(__ANDROID__)

namespace {
QJniObject context() { return QJniObject(QNativeInterface::QAndroidApplication::context()); }
QJniObject jstr(const char *s) { return QJniObject::fromString(QString::fromLatin1(s)); }
QJniObject systemService(const char *name)
{
    return context().callObjectMethod("getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", jstr(name).object<jstring>());
}
}

QString systemDeviceName()
{
    // Настройки → О телефоне → Название устройства (Settings.Global.device_name); чтение разрешения не требует
    const QJniObject resolver = context().callObjectMethod("getContentResolver", "()Landroid/content/ContentResolver;");
    if (resolver.isValid()) {
        const QJniObject n = QJniObject::callStaticObjectMethod("android/provider/Settings$Global", "getString",
            "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;", resolver.object(), jstr("device_name").object<jstring>());
        if (n.isValid() && !n.toString().trimmed().isEmpty()) return n.toString().trimmed();
    }
    return deviceModel();
}

QString deviceModel()
{
    const auto field = [](const char *name) {
        return QJniObject::getStaticObjectField<jstring>("android/os/Build", name).toString().trimmed();
    };
    QString maker = field("MANUFACTURER"), model = field("MODEL");
    if (!maker.isEmpty()) maker[0] = maker[0].toUpper();
    return model.startsWith(maker, Qt::CaseInsensitive) ? model : (maker + QLatin1Char(' ') + model).trimmed();
}

BatteryState battery()
{
    // липкое широковещательное сообщение BATTERY_CHANGED отдаёт текущее состояние без подписки
    QJniObject filter("android/content/IntentFilter", "(Ljava/lang/String;)V", jstr("android.intent.action.BATTERY_CHANGED").object<jstring>());
    QJniObject intent = context().callObjectMethod("registerReceiver",
        "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;", nullptr, filter.object());
    BatteryState s;
    if (!intent.isValid()) return s;
    const jint level = intent.callMethod<jint>("getIntExtra", "(Ljava/lang/String;I)I", jstr("level").object<jstring>(), jint(-1));
    const jint scale = intent.callMethod<jint>("getIntExtra", "(Ljava/lang/String;I)I", jstr("scale").object<jstring>(), jint(-1));
    const jint status = intent.callMethod<jint>("getIntExtra", "(Ljava/lang/String;I)I", jstr("status").object<jstring>(), jint(-1));
    if (level >= 0 && scale > 0) s.percent = level * 100 / scale;
    s.charging = (status == 2 || status == 5);      // BatteryManager.STATUS_CHARGING / STATUS_FULL
    return s;
}

void acquireMulticastLock()
{
    static QJniObject lock;
    if (lock.isValid()) return;
    QJniObject wifi = systemService("wifi");
    if (!wifi.isValid()) return;
    lock = wifi.callObjectMethod("createMulticastLock", "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;", jstr("orbit").object<jstring>());
    if (lock.isValid()) lock.callMethod<void>("acquire");
}

void vibrate(int ms)
{
    QJniObject v = systemService("vibrator");
    if (v.isValid()) v.callMethod<void>("vibrate", "(J)V", jlong(ms));
}

void cancelVibration()
{
    QJniObject v = systemService("vibrator");
    if (v.isValid()) v.callMethod<void>("cancel", "()V");
}

void setLoudVolume(bool on)
{
    static int previous = -1;
    QJniObject am = systemService("audio");
    if (!am.isValid()) return;
    constexpr jint kStreamMusic = 3;
    if (on) {
        if (previous < 0) previous = am.callMethod<jint>("getStreamVolume", "(I)I", kStreamMusic);
        // телефон на минимальной громкости всё равно должен быть слышен, но не на максимум: поднимаем только до 55 % шкалы
        const jint maxv = am.callMethod<jint>("getStreamMaxVolume", "(I)I", kStreamMusic);
        const jint target = jint(maxv * 0.55);
        if (previous < target) am.callMethod<void>("setStreamVolume", "(III)V", kStreamMusic, target, jint(0));
    } else if (previous >= 0) {
        am.callMethod<void>("setStreamVolume", "(III)V", kStreamMusic, jint(previous), jint(0));
        previous = -1;
    }
}

void mediaKey(const QString &action)
{
    // системные медиа-клавиши получает активный плеер (как кнопки на наушниках): KEYCODE_MEDIA_PLAY_PAUSE 85, NEXT 87, PREVIOUS 88
    const jint code = action == QLatin1String("next") ? 87 : (action == QLatin1String("previous") ? 88 : 85);
    QJniObject am = systemService("audio");
    if (!am.isValid()) return;
    for (jint act = 0; act <= 1; ++act) {                       // 0 — нажатие, 1 — отпускание
        QJniObject ev("android/view/KeyEvent", "(II)V", act, code);
        am.callMethod<void>("dispatchMediaKeyEvent", "(Landroid/view/KeyEvent;)V", ev.object());
    }
}

QByteArray mediaJson()
{
    const QJniObject r = QJniObject::callStaticObjectMethod("org/glassrice/namelessorbit/OrbitMedia", "read", "(Landroid/content/Context;)Ljava/lang/String;", context().object());
    return r.isValid() ? r.toString().toUtf8() : QByteArray();
}

bool mediaCommand(const QString &action)
{
    return QJniObject::callStaticMethod<jboolean>("org/glassrice/namelessorbit/OrbitMedia", "command", "(Landroid/content/Context;Ljava/lang/String;)Z",
                                                  context().object(), QJniObject::fromString(action).object<jstring>());
}

bool mediaAccess()
{
    return QJniObject::callStaticMethod<jboolean>("org/glassrice/namelessorbit/OrbitMedia", "hasAccess", "(Landroid/content/Context;)Z", context().object());
}

void openMediaAccess()
{
    QJniObject::callStaticMethod<void>("org/glassrice/namelessorbit/OrbitMedia", "openAccessSettings", "(Landroid/content/Context;)V", context().object());
}

int screenBrightness()
{
    return QJniObject::callStaticMethod<jint>("org/glassrice/namelessorbit/OrbitMedia", "brightness", "(Landroid/content/Context;)I", context().object());
}

bool runAction(const QString &action)
{
    if (action == QLatin1String("flashlight")) {
        static bool on = false;
        QJniObject cm = systemService("camera");
        if (!cm.isValid()) return false;
        on = !on;
        // основная камера («0») — у телефонов со вспышкой это она; без вспышки вызов тихо ничего не сделает
        cm.callMethod<void>("setTorchMode", "(Ljava/lang/String;Z)V", jstr("0").object<jstring>(), jboolean(on));
        return true;
    }
    return false;
}

bool musicActive()
{
    QJniObject am = systemService("audio");
    return am.isValid() && am.callMethod<jboolean>("isMusicActive", "()Z");
}

namespace {
constexpr const char *kService = "org/glassrice/namelessorbit/OrbitService";
}

QString publishDownload(const QString &srcPath, const QString &fileName)
{
    const QJniObject r = QJniObject::callStaticObjectMethod(kService, "publishDownload",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
        context().object(), QJniObject::fromString(srcPath).object<jstring>(), QJniObject::fromString(fileName).object<jstring>());
    return r.isValid() ? r.toString() : QString();
}

void keepAwake(bool on)
{
    QJniObject::callStaticMethod<void>(kService, "keepAwake", "(Landroid/content/Context;Z)V", context().object(), jboolean(on));
}

void setPictureInPicture(bool enabled, int w, int h)
{
    QJniObject::callStaticMethod<void>("org/glassrice/namelessorbit/OrbitActivity", "setPip", "(ZII)V", jboolean(enabled), jint(w), jint(h));
}

void enterPictureInPicture()
{
    QJniObject::callStaticMethod<void>("org/glassrice/namelessorbit/OrbitActivity", "enterPipNow", "()V");
}

void setViewerFullscreen(bool on, int orientation)
{
    QJniObject::callStaticMethod<void>("org/glassrice/namelessorbit/OrbitActivity", "setViewerFullscreen", "(ZI)V", jboolean(on), jint(orientation));
}

bool hasHardwareVideoDecoder()
{
    // не кэшируем «нет»: первый вызов мог прийти, пока Java-часть не готова, и вечное «нет» отнимало бы видео у человека. Ошибка вызова = «да»
    static bool known = false, has = false;
    if (known) return has;
    QJniEnvironment env;
    const bool r = QJniObject::callStaticMethod<jboolean>("org/glassrice/namelessorbit/OrbitVideoDecoder", "hasHardwareH264", "()Z");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
    if (r) { known = true; has = true; }
    return r;
}

bool feedVideo(const QByteArray &au, int width, int height, bool key, qint64 ptsUs)
{
    QJniEnvironment env;
    jbyteArray arr = env->NewByteArray(jsize(au.size()));
    if (!arr) return false;
    env->SetByteArrayRegion(arr, 0, jsize(au.size()), reinterpret_cast<const jbyte *>(au.constData()));
    const bool ok = QJniObject::callStaticMethod<jboolean>("org/glassrice/namelessorbit/OrbitVideoDecoder", "feed", "([BIIZJ)Z",
                                                           arr, jint(width), jint(height), jboolean(key), jlong(ptsUs));
    env->DeleteLocalRef(arr);
    return ok;
}

void stopVideo()
{
    QJniObject::callStaticMethod<void>("org/glassrice/namelessorbit/OrbitVideoDecoder", "stop", "()V");
}

void openBatterySettings()
{
    QJniObject::callStaticMethod<void>(kService, "openBatterySettings", "(Landroid/content/Context;)V", context().object());
}

bool notificationsAllowed()
{
    return QJniObject::callStaticMethod<jboolean>(kService, "notificationsEnabled", "(Landroid/content/Context;)Z", context().object());
}

void requestNotificationPermission()
{
    QJniObject::callStaticMethod<void>(kService, "requestNotificationPermission", "(Landroid/content/Context;)V", context().object());
}

void openNotificationSettings()
{
    QJniObject::callStaticMethod<void>(kService, "openNotificationSettings", "(Landroid/content/Context;)V", context().object());
}

void startBackgroundService(bool quietNotice)
{
    const QJniObject ctx = context();
    QJniObject::callStaticMethod<void>(kService, "requestNotificationPermission", "(Landroid/content/Context;)V", ctx.object());
    QJniObject::callStaticMethod<void>(kService, "start", "(Landroid/content/Context;Z)V", ctx.object(), jboolean(quietNotice));
}

void moveToBack()
{
    QJniObject::callStaticMethod<void>("org/glassrice/namelessorbit/OrbitActivity", "moveToBack", "()V");
}

void stopBackgroundService()
{
    QJniObject::callStaticMethod<void>(kService, "stop", "(Landroid/content/Context;)V", context().object());
}

void setBackgroundStatus(const QString &text)
{
    QJniObject::callStaticMethod<void>(kService, "setStatus", "(Landroid/content/Context;Ljava/lang/String;)V", context().object(), QJniObject::fromString(text).object<jstring>());
}

void postNotification(const QString &title, const QString &body, bool force)
{
    if (title.trimmed().isEmpty() && body.trimmed().isEmpty()) return;      // пустых уведомлений не бывает
    if (!shouldPostNotification(title, body, force)) return;
    QJniObject::callStaticMethod<void>(kService, "notifyEvent", "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)V",
                                       context().object(), QJniObject::fromString(title).object<jstring>(), QJniObject::fromString(body).object<jstring>());
}

#elif defined(_WIN32)

QString deviceModel() { return QSysInfo::prettyProductName(); }
QString systemDeviceName() { return QSysInfo::machineHostName(); }

BatteryState battery()
{
    BatteryState s;
    SYSTEM_POWER_STATUS p;
    if (!GetSystemPowerStatus(&p) || p.BatteryLifePercent == 255 || (p.BatteryFlag & 128)) return s;   // 128 — батареи нет
    s.percent = p.BatteryLifePercent;
    s.charging = p.ACLineStatus == 1;
    return s;
}
void acquireMulticastLock() {}
void vibrate(int) {}
void cancelVibration() {}
void setLoudVolume(bool) {}
void startBackgroundService(bool) {}
void moveToBack() {}
void stopBackgroundService() {}
void setBackgroundStatus(const QString &) {}
void postNotification(const QString &, const QString &, bool) {}
void mediaKey(const QString &) {}
bool musicActive() { return false; }
QByteArray mediaJson() { return {}; }
bool mediaCommand(const QString &) { return false; }
bool mediaAccess() { return false; }
void openMediaAccess() {}
int screenBrightness() { return -1; }
bool runAction(const QString &) { return false; }
bool notificationsAllowed() { return true; }
void requestNotificationPermission() {}
void openNotificationSettings() {}
void keepAwake(bool) {}
void setPictureInPicture(bool, int, int) {}
void enterPictureInPicture() {}
void setViewerFullscreen(bool, int) {}
bool hasHardwareVideoDecoder() { return false; }
bool feedVideo(const QByteArray &, int, int, bool, qint64) { return false; }
void stopVideo() {}
void openBatterySettings() {}
QString publishDownload(const QString &srcPath, const QString &fileName) { return moveToDownloads(srcPath, fileName); }

#else

QString systemDeviceName() { return QSysInfo::machineHostName(); }

// Linux: изготовитель и модель из DMI (например «Machenike L15 Pro»); нет данных — название системы
QString deviceModel()
{
    const auto dmi = [](const char *name) {
        QFile f(QStringLiteral("/sys/class/dmi/id/") + QLatin1String(name));
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed() : QString();
    };
    const QString vendor = dmi("sys_vendor"), product = dmi("product_name");
    const bool junk = product.isEmpty() || product.contains(QLatin1String("To Be Filled"), Qt::CaseInsensitive)
                      || product.contains(QLatin1String("System Product"), Qt::CaseInsensitive) || product == QLatin1String("Default string");
    if (junk) return QSysInfo::prettyProductName();
    return product.startsWith(vendor, Qt::CaseInsensitive) || vendor.isEmpty() ? product : vendor + QLatin1Char(' ') + product;
}

BatteryState battery() { return {}; }
void acquireMulticastLock() {}
void vibrate(int) {}
void cancelVibration() {}
void setLoudVolume(bool) {}
void startBackgroundService(bool) {}
void moveToBack() {}
void stopBackgroundService() {}
void setBackgroundStatus(const QString &) {}
void mediaKey(const QString &) {}
bool musicActive() { return false; }
QByteArray mediaJson() { return {}; }
bool mediaCommand(const QString &) { return false; }
bool mediaAccess() { return false; }
void openMediaAccess() {}
int screenBrightness() { return -1; }
bool runAction(const QString &) { return false; }
bool notificationsAllowed() { return true; }
void requestNotificationPermission() {}
void openNotificationSettings() {}
void keepAwake(bool) {}
void setPictureInPicture(bool, int, int) {}
void enterPictureInPicture() {}
void setViewerFullscreen(bool, int) {}
bool hasHardwareVideoDecoder() { return false; }
bool feedVideo(const QByteArray &, int, int, bool, qint64) { return false; }
void stopVideo() {}
void openBatterySettings() {}
QString publishDownload(const QString &srcPath, const QString &fileName) { return moveToDownloads(srcPath, fileName); }

#ifdef ORBIT_DESKTOP_LINUX
// обычное уведомление рабочего стола: его принимает и показывает демон уведомлений (в сборке — NameLess UI)
void postNotification(const QString &title, const QString &body, bool force)
{
    if (title.trimmed().isEmpty() && body.trimmed().isEmpty()) return;
    if (!shouldPostNotification(title, body, force)) return;
    QDBusInterface n(QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("/org/freedesktop/Notifications"),
                     QStringLiteral("org.freedesktop.Notifications"), QDBusConnection::sessionBus());
    if (!n.isValid()) return;
    n.call(QDBus::NoBlock, QStringLiteral("Notify"), QStringLiteral("NameLess Orbit"), uint(0), QStringLiteral("nameless-orbit"),
           title, body, QStringList(), QVariantMap(), -1);
}
#else
void postNotification(const QString &, const QString &, bool) {}
#endif

#endif

}
