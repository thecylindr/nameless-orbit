#ifndef PLATFORMBRIDGE_H
#define PLATFORMBRIDGE_H

// Тонкий слой над системой там, где у Qt нет своего API: заряд батареи (Android, Windows), блокировка приёма
// широковещательных пакетов Wi-Fi на Android (без неё обнаружение устройств засыпает), вибрация для «Найти устройство».
// На остальных системах функции безопасно ничего не делают — вызывающий код не проверяет платформу сам
#include <QString>
#include <QByteArray>

namespace PlatformBridge {

struct BatteryState
{
    int percent = -1;        // -1 — батареи нет или значение неизвестно
    bool charging = false;
};

// модель устройства для показа в списках: «Xiaomi 14», «Machenike L15», «Arch Linux»
QString deviceModel();

// имя устройства, как его видит пользователь: Android — «Название устройства» из настроек телефона, иначе имя узла в сети
QString systemDeviceName();

BatteryState battery();
void acquireMulticastLock();
void vibrate(int ms);
void cancelVibration();

// «Найти устройство»: на время сигнала громкость мультимедиа поднимается до максимума (Android), потом возвращается прежней
void setLoudVolume(bool on);

// плеер этого устройства без MPRIS (Android): нажатие медиа-клавиши («playpause» | «next» | «previous») и «играет ли сейчас звук»
void mediaKey(const QString &action);
bool musicActive();

// встроенные действия телефона для «Запуска команд» (на Android оболочки нет): «flashlight» — фонарик вкл/выкл. true — выполнено
bool runAction(const QString &action);

// фоновая работа (Android: служба переднего плана; на остальных ничего) и системное уведомление о событии
// (Android: канал «События Orbit»; настольный Linux: org.freedesktop.Notifications — то есть центр уведомлений NameLess UI)
// принятый файл → «Загрузки»: Android — Download/Orbit через MediaStore, остальные — ~/Downloads/Orbit. Возвращает, куда
// сохранено (для показа), пустая строка — не вышло. Исходный временный файл при успехе удаляется
QString publishDownload(const QString &srcPath, const QString &fileName);

void startBackgroundService();
void stopBackgroundService();
void setBackgroundStatus(const QString &text);      // Android: строка постоянного уведомления службы («Подключено: …», «Нет связи …»); пустого уведомления не бывает
// force — показать даже когда окно приложения на виду (пробное уведомление); иначе уведомление не дублирует то, что человек и так видит в окне,
// и одинаковый текст подряд (несколько секунд) показывается один раз
void postNotification(const QString &title, const QString &body, bool force = false);
// Android 13+: разрешение на уведомления. allowed — включены ли они сейчас; request — показать системный запрос (один раз);
// openSettings — окно настроек уведомлений приложения, если человек их запретил
bool notificationsAllowed();
void requestNotificationPermission();
void openNotificationSettings();

// Фон для просмотра и показа экрана (Android): keepAwake — держать процессор и сеть, пока идёт поток; picture-in-picture —
// просмотр в маленьком окне поверх других приложений (включается само, когда человек уходит из приложения во время просмотра);
// openBatterySettings — окно «не ограничивать батарею» для Orbit
void keepAwake(bool on);
void setPictureInPicture(bool enabled, int frameWidth, int frameHeight);
void enterPictureInPicture();
// полноэкранный просмотр (Android): скрыть системные панели и задать ориентацию — 0 по системе, 1 альбомная, 2 портретная, 3 по датчику
void setViewerFullscreen(bool on, int orientation);
// Видео H.264 от другого устройства (Android — аппаратный декодер MediaCodec): feedVideo отдаёт один кадр (Annex B) в разжатие; false — кадр не принят
// (декодер занят, или он ждёт ключевой кадр). Готовые картинки приходят в RemoteDesktopController::onVideoImage. stopVideo — освободить декодер
// hasHardwareVideoDecoder — есть ли аппаратный декодер H.264 (на ПК false: там видео пока не разжимается)
bool hasHardwareVideoDecoder();
bool feedVideo(const QByteArray &au, int width, int height, bool key, qint64 ptsUs);
void stopVideo();
void openBatterySettings();

}

#endif // PLATFORMBRIDGE_H
