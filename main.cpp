// NameLess Orbit — связь между своими устройствами в локальной сети: обнаружение, сопряжение, буфер обмена,
// уведомления, батарея, медиа, команды, демонстрация и управление экраном. Подробно — в README.md.
#ifndef APP_VERSION_STR
#define APP_VERSION_STR "0.2.1"
#endif

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QIcon>
#include <QWindow>

#include "core/Identity.h"
#include "core/TrustStore.h"
#include "core/PeerManager.h"
#include "core/SettingsManager.h"
#include "core/CommandStore.h"
#include "core/SingleInstance.h"
#include "core/ThemeManager.h"
#include "core/Localizer.h"
#include "core/OrbitDBus.h"
#include "platform/PlatformBridge.h"
#include "plugins/ClipboardPlugin.h"
#include "plugins/BatteryPlugin.h"
#include "plugins/FindDevicePlugin.h"
#include "plugins/MediaPlugin.h"
#include "plugins/RunCommandPlugin.h"
#include "plugins/NotificationsPlugin.h"
#include "plugins/SharePlugin.h"
#include "remote/RemoteDesktopController.h"
#include "remote/VirtualDisplay.h"
#include "remote/FrameView.h"
#include <QtQml/qqml.h>

namespace {
// иконка программы из PNG нужных размеров — тот же приём, что в drones/drokclient: SVG без плагина qsvg
// в сборке может не оказаться, набор растровых размеров работает везде одинаково
QIcon appIcon()
{
    QIcon icon;
    for (int s : {16, 24, 32, 48, 64, 128, 256})
        icon.addFile(QStringLiteral(":/qt/qml/NamelessOrbit/appicon/appicon-%1.png").arg(s), QSize(s, s));
    return icon;
}
}

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("NameLess Orbit"));
    QCoreApplication::setApplicationVersion(QStringLiteral(APP_VERSION_STR));
    QCoreApplication::setOrganizationName(QStringLiteral("glassrice"));
    QGuiApplication::setWindowIcon(appIcon());
    QGuiApplication::setDesktopFileName(QStringLiteral("nameless-orbit"));

    // демонстрация экрана (QScreenCapture) работает только с backend FFmpeg Qt Multimedia — задаём явно
    // на случай, если дистрибутив собрал Qt с другим backend по умолчанию; переменная читается при первом
    // обращении к Qt Multimedia, поэтому должна быть установлена до создания любых его объектов
    if (qEnvironmentVariableIsEmpty("QT_MEDIA_BACKEND"))
        qputenv("QT_MEDIA_BACKEND", "ffmpeg");

    const QString instanceName = QStringLiteral("nameless-orbit-") +
        qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME", QStringLiteral("user")));

    // поднимаем ядро до решения про единственный экземпляр: обработчик канала должен иметь к чему обращаться
    Identity identity;
    TrustStore trustStore;
    PeerManager peerManager(&identity, &trustStore);
    VirtualDisplay::cleanupStale();      // виртуальный монитор «второго экрана» мог остаться от прошлого запуска (Orbit упал)
    SettingsManager settings;
    CommandStore commandStore;
    ThemeManager theme;

    // OrbitDBus создаётся ниже, когда готовы плагины; до тех пор канал отвечает пустым статусом
    OrbitDBus *dbus = nullptr;

    SingleInstance instance(instanceName);
    instance.setHandler([&peerManager, &dbus](const QByteArray &cmd) -> QByteArray {
        if (cmd == "ping") return QByteArrayLiteral("{\"ok\":true}");
        if (cmd == "status" || cmd == "show" || cmd == "scan" || cmd == "enable" || cmd == "disable") {
            if (cmd == "scan") peerManager.refresh();
            if (cmd == "enable") peerManager.setDiscoveryActive(true);
            if (cmd == "disable") peerManager.setDiscoveryActive(false);
            if (!dbus) return QByteArrayLiteral("{}");
            if (cmd == "show") dbus->Show();       // повторный запуск поднимает окно уже работающего Orbit
            return dbus->statusJson();
        }
        return QByteArrayLiteral("{\"ok\":false,\"error\":\"unknown command\"}");
    });

    if (SingleInstance::ask(instanceName, QByteArrayLiteral("show"), nullptr, 800)) {
        qInfo() << "[main] NameLess Orbit уже запущен";
        return 0;
    }
    if (!instance.listen())
        qWarning() << "[main] не удалось занять канал единственного экземпляра — команды status/scan извне не дойдут";

    if (settings.discoveryOnStart()) peerManager.setDiscoveryActive(true);
    peerManager.setIntroProvider([&commandStore]() { return commandStore.commandsForQml(); });     // список быстрых команд для знакомства при сопряжении

    // разрешение «показ экрана» у устройства автоматически включает главный выключатель (показ и управление), а когда оно
    // не осталось ни у одного устройства — выключает: отдельно ходить в настройки не нужно
    QObject::connect(&trustStore, &TrustStore::pluginAllowedChanged, &app, [&settings, &trustStore](const QString &, const QString &plugin, bool allowed) {
        if (plugin != QLatin1String("remoteDesktop")) return;
        if (allowed) settings.setRemoteDesktopFeatureEnabled(true);
        else if (!trustStore.anyAllows(plugin)) settings.setRemoteDesktopFeatureEnabled(false);
    });

    // фон: закрытие окна не завершает Orbit (компьютер), служба переднего плана (Android)
    // постоянное уведомление службы Android всегда несёт текст о связи и обновляется при её изменении — пустой строки в шторке не бывает
    QObject::connect(&peerManager, &PeerManager::connectedChanged, &app, [&peerManager]() {
        QStringList names;
        for (const QVariant &v : peerManager.connectedDevicesForQml()) names << v.toMap().value(QStringLiteral("name")).toString();
        PlatformBridge::setBackgroundStatus(names.isEmpty() ? QStringLiteral("Нет связи — ищу устройства") : QStringLiteral("Подключено: ") + names.join(QStringLiteral(", ")));
    });
#if defined(Q_OS_ANDROID)
    if (settings.keepRunning()) PlatformBridge::startBackgroundService();
    QObject::connect(&settings, &SettingsManager::keepRunningChanged, &app, [&settings]() {
        if (settings.keepRunning()) PlatformBridge::startBackgroundService(); else PlatformBridge::stopBackgroundService();
    });
#else
    app.setQuitOnLastWindowClosed(!settings.keepRunning());
    QObject::connect(&settings, &SettingsManager::keepRunningChanged, &app, [&settings, &app]() {
        app.setQuitOnLastWindowClosed(!settings.keepRunning());
    });
#endif
#if defined(Q_OS_ANDROID)
    // разрешение на уведомления просится при каждом выходе приложения на экран, пока оно не выдано (первый запрос мог прийти до окна)
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &app, [](Qt::ApplicationState st) {
        if (st == Qt::ApplicationActive && !PlatformBridge::notificationsAllowed()) PlatformBridge::requestNotificationPermission();
    });
#endif
    // события, которые нельзя пропустить, пока окно закрыто: запрос сопряжения и уведомления с других устройств
    QObject::connect(&peerManager, &PeerManager::pairingRequested, &app, [](const QString &, const QString &name, const QString &, const QString &code) {
        PlatformBridge::postNotification(QStringLiteral("Запрос на сопряжение"), name + QStringLiteral(" · код ") + code);
    });

    // плагины регистрируются в PeerManager под своим именем — так PeerManager узнаёт, какие возможности
    // этого устройства рассылать подключённым и куда отдавать входящие сообщения по каждому имени
    ClipboardPlugin clipboardPlugin(&peerManager);
    BatteryPlugin batteryPlugin(&peerManager);
    FindDevicePlugin findDevicePlugin(&peerManager);
    MediaPlugin mediaPlugin(&peerManager);
    RunCommandPlugin runCommandPlugin(&peerManager, &commandStore);
    NotificationsPlugin notificationsPlugin(&peerManager);
    SharePlugin sharePlugin(&peerManager);
    for (PluginBase *p : std::initializer_list<PluginBase *>{&clipboardPlugin, &batteryPlugin, &findDevicePlugin,
                                                              &mediaPlugin, &runCommandPlugin, &notificationsPlugin, &sharePlugin})
        peerManager.registerPlugin(p->name(), p);
    // низкорисковые плагины включены сразу; клиент сам решает через TrustStore, кому они видны
    clipboardPlugin.setEnabled(true);
    batteryPlugin.setEnabled(batteryPlugin.isAvailable());
    findDevicePlugin.setEnabled(true);
    mediaPlugin.setEnabled(true);
    runCommandPlugin.setEnabled(true);
    notificationsPlugin.setEnabled(true);
    sharePlugin.setEnabled(true);

    // D-Bus для NameLess UI и скриптов; нет шины или имя занято — Orbit работает без неё
    OrbitDBus orbitDBus(&peerManager, &batteryPlugin, &findDevicePlugin, &clipboardPlugin, &notificationsPlugin, &sharePlugin, &runCommandPlugin);
    orbitDBus.setRepliesGate([&settings]() { return settings.sendReplies(); });
    dbus = &orbitDBus;
#ifdef ORBIT_DESKTOP_LINUX
    if (!orbitDBus.registerOnBus())
        qWarning() << "[main] D-Bus недоступен — виджет NameLess UI не увидит Orbit";
#endif

    // создаётся ДО engine, чтобы пережить его: при выходе engine разбирает привязки QML, а им ещё нужны
    // объекты контекста (иначе в логе TypeError: Cannot read property ... of null)
    RemoteDesktopController remoteDesktop(&peerManager, &settings, nullptr);
    peerManager.registerPlugin(remoteDesktop.name(), &remoteDesktop);

    // экран показывающего устройства рисуется своим элементом сцены (см. remote/FrameView.h); регистрируется вручную в модуле
    // приложения — QML-файлы модуля видят его без отдельного import
    qmlRegisterType<FrameView>("NamelessOrbit", 1, 0, "FrameView");
    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("orbit"), remoteDesktop.createImageProvider());   // кадры демонстрации; engine владеет провайдером

    QQmlContext *ctx = engine.rootContext();
    ctx->setContextProperty(QStringLiteral("Theme"), &theme);
    ctx->setContextProperty(QStringLiteral("Identity"), &identity);
    ctx->setContextProperty(QStringLiteral("TrustStoreModel"), &trustStore);
    ctx->setContextProperty(QStringLiteral("PeerManagerModel"), &peerManager);
    ctx->setContextProperty(QStringLiteral("Settings"), &settings);
    ctx->setContextProperty(QStringLiteral("Commands"), &commandStore);
    ctx->setContextProperty(QStringLiteral("Clipboard"), &clipboardPlugin);
    ctx->setContextProperty(QStringLiteral("Battery"), &batteryPlugin);
    ctx->setContextProperty(QStringLiteral("FindDevice"), &findDevicePlugin);
    ctx->setContextProperty(QStringLiteral("Media"), &mediaPlugin);
    ctx->setContextProperty(QStringLiteral("RunCommand"), &runCommandPlugin);
    ctx->setContextProperty(QStringLiteral("Notifications"), &notificationsPlugin);
    ctx->setContextProperty(QStringLiteral("Share"), &sharePlugin);
    ctx->setContextProperty(QStringLiteral("RemoteDesktop"), &remoteDesktop);
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    ctx->setContextProperty(QStringLiteral("IsMobile"), true);      // телефон: свой экран и команды не отдаём, кнопка «Назад» ходит по страницам
#else
    ctx->setContextProperty(QStringLiteral("IsMobile"), false);
#endif
    ctx->setContextProperty(QStringLiteral("AppVersion"), QStringLiteral(APP_VERSION_STR));

    // --background: без окна, только связь (запускает NameLess UI, когда нужны устройства); окно — по Show() из D-Bus
    ctx->setContextProperty(QStringLiteral("StartHidden"), app.arguments().contains(QStringLiteral("--background")));

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    // язык интерфейса: русский — исходные тексты, английский — словарь (core/Localizer); переключается на лету
    DictTranslator enTranslator;
    const bool enLoaded = enTranslator.loadJson(QStringLiteral(":/qt/qml/NamelessOrbit/i18n/en.json"));
    const auto applyLanguage = [&]() {
        QCoreApplication::removeTranslator(&enTranslator);
        if (enLoaded && settings.language() == QLatin1String("en")) QCoreApplication::installTranslator(&enTranslator);
        engine.retranslate();
    };
    QObject::connect(&settings, &SettingsManager::languageChanged, &app, applyLanguage);
    if (enLoaded && settings.language() == QLatin1String("en")) QCoreApplication::installTranslator(&enTranslator);
    engine.loadFromModule("NamelessOrbit", "Main");
    if (engine.rootObjects().isEmpty()) return -1;

    // «Показать» из D-Bus и повторного запуска: развернуть и вынести окно вперёд
    QObject::connect(&notificationsPlugin, &NotificationsPlugin::notificationReceived, &app,
                     [&settings](const QString &, const QString &from, const QString &title, const QString &body, const QString &app) {
        if (app == QLatin1String("Claude") && !settings.receiveReplies()) return;      // человек выключил сообщения об ответе Claude
        PlatformBridge::postNotification(from + QStringLiteral(": ") + title, body);
    });
    // запросы, на которые должен ответить человек, приходят в диалог окна: если окно закрыто (работа в фоне), оно выходит вперёд само
    const auto bringToFront = [&engine]() {
        if (auto *w = qobject_cast<QWindow *>(engine.rootObjects().first())) {
            if (w->isVisible() && w->windowStates() != Qt::WindowMinimized) return;
            w->setWindowStates(Qt::WindowNoState);
            w->show(); w->raise(); w->requestActivate();
        }
    };
    QObject::connect(&remoteDesktop, &RemoteDesktopController::shareRequested, &app, bringToFront);
    QObject::connect(&remoteDesktop, &RemoteDesktopController::shareOffered, &app, bringToFront);
    QObject::connect(&remoteDesktop, &RemoteDesktopController::controlRequested, &app, bringToFront);
    QObject::connect(&remoteDesktop, &RemoteDesktopController::secondScreenOffered, &app, bringToFront);
    QObject::connect(&remoteDesktop, &RemoteDesktopController::secondScreenRequested, &app, bringToFront);      // иначе при работе в фоне окно с вопросом не появлялось
    QObject::connect(&remoteDesktop, &RemoteDesktopController::sourceChoiceNeeded, &app, bringToFront);
    QObject::connect(&remoteDesktop, &RemoteDesktopController::secondScreenChanged, &app, [&]() { if (remoteDesktop.secondScreen()) bringToFront(); });      // второй экран занимает экран устройства сразу
    QObject::connect(&sharePlugin, &SharePlugin::offerReceived, &app, bringToFront);
    QObject::connect(&runCommandPlugin, &RunCommandPlugin::confirmationNeeded, &app, bringToFront);
    // «Посмотреть экран» / «Показать экран» из Nameless UI: запрос уходит устройству, окно Orbit выходит вперёд и открывает просмотр
    QObject::connect(&orbitDBus, &OrbitDBus::screenRequested, &app, [&](const QString &id, bool view) {
        if (view) { remoteDesktop.requestShare(id); emit orbitDBus.showRequested(); }
        else remoteDesktop.offerShare(id);
    });
    QObject::connect(&orbitDBus, &OrbitDBus::quitRequested, &app, &QCoreApplication::quit);
    QObject::connect(&orbitDBus, &OrbitDBus::showRequested, &app, [&engine]() {
        if (auto *w = qobject_cast<QWindow *>(engine.rootObjects().first())) {
            w->setWindowStates(Qt::WindowNoState);
            w->show(); w->raise(); w->requestActivate();
        }
    });

    return app.exec();
}
