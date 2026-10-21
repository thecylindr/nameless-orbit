cmake_minimum_required(VERSION 3.16)

project(nameless-orbit VERSION 0.2.4 LANGUAGES CXX)
add_compile_definitions(APP_VERSION_STR="${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Qml Network Multimedia)
# настольный Linux: D-Bus (MPRIS, UPower, свой интерфейс для NameLess UI), /dev/uinput, QProcess.
# Android, Windows, macOS собираются без этого — Qt DBus для Android не существует, QProcess там недоступен
if(UNIX AND NOT APPLE AND NOT ANDROID)
    set(ORBIT_DESKTOP_LINUX ON)
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    add_compile_definitions(ORBIT_DESKTOP_LINUX)
endif()
qt_standard_project_setup(REQUIRES 6.8)

# Демонстрация экрана (QScreenCapture) работает только с backend FFmpeg Qt Multimedia (main.cpp задаёт
# QT_MEDIA_BACKEND=ffmpeg на запуске на случай, если система собрана с другим backend по умолчанию).
# На Wayland QScreenCapture сама показывает системный выбор экрана через XDG Desktop Portal (ScreenCast) и
# требует PipeWire 0.3 и запущенный бэкенд портала композитора (xdg-desktop-portal-hyprland для Hyprland).
# Без них демонстрация честно откажет с понятной ошибкой в UI, а не падает и не захватывает экран в обход

# =====================================================================
#  Исполняемый файл
# =====================================================================
qt_add_executable(nameless-orbit
    main.cpp
    # классы целиком в заголовке: без .cpp AUTOMOC их не видит, и линковка падает на vtable / staticMetaObject
    core/ThemeManager.h
    core/Localizer.cpp
    core/Localizer.h
    plugins/PluginBase.h
    core/CryptoUtils.cpp
    core/Identity.cpp
    core/TrustStore.cpp
    core/Peer.cpp
    core/Discovery.cpp
    core/PeerManager.cpp
    core/SettingsManager.cpp
    core/CommandStore.cpp
    core/OrbitDBus.cpp
    platform/PlatformBridge.cpp
    platform/AndroidCapture.cpp
    core/SingleInstance.cpp
    plugins/ClipboardPlugin.cpp
    plugins/BatteryPlugin.cpp
    plugins/FindDevicePlugin.cpp
    plugins/MediaPlugin.cpp
    plugins/RunCommandPlugin.cpp
    plugins/NotificationsPlugin.cpp
    plugins/SharePlugin.cpp
    remote/ScreenCaptureSession.cpp
    remote/FrameView.cpp
    remote/FrameView.h
    remote/InputInjector.cpp
    remote/HyprIpc.h
    remote/EdgeCrossing.cpp
    remote/KeyboardGrab.cpp
    remote/RemoteDesktopController.cpp
    remote/VideoDecoder.cpp
    remote/AudioStream.cpp
    remote/VirtualDisplay.cpp
)

if(WIN32)
    target_sources(nameless-orbit PRIVATE appicon/app.rc)
endif()

# =====================================================================
#  QML-модуль
# =====================================================================
qt_add_qml_module(nameless-orbit
    URI NamelessOrbit
    VERSION 1.0
    QML_FILES
        Main.qml
        ui/DeviceListPage.qml
        ui/DeviceCard.qml
        ui/PairingDialog.qml
        ui/PairingRequestBanner.qml
        ui/DeviceDetailPage.qml
        ui/RemoteScreenPage.qml
        ui/SecondScreenDialog.qml
        ui/SettingsPage.qml
        ui/CommandEditorDialog.qml
        ui/IncomingRequests.qml
        ui/common/GlassButton.qml
        ui/common/GlassSwitch.qml
        ui/common/GlassCard.qml
        ui/common/Toast.qml
        ui/common/ConsentBanner.qml
        ui/common/Icon.qml
        ui/common/GlassSpin.qml
    RESOURCES
        i18n/en.json
        appicon/appicon-16.png
        appicon/appicon-24.png
        appicon/appicon-32.png
        appicon/appicon-48.png
        appicon/appicon-64.png
        appicon/appicon-128.png
        appicon/appicon-256.png
)

target_link_libraries(nameless-orbit PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Qml
    Qt6::Network
    Qt6::Multimedia
)
if(ORBIT_DESKTOP_LINUX)
    target_link_libraries(nameless-orbit PRIVATE Qt6::DBus)
    # Разжатие видео H.264 на ПК (показ экрана телефона): libavcodec из системного ffmpeg. Необязательно: без неё сборка идёт, ПК просто
    # получает тайлы или JPEG, а в настройках пункт «Видео H.264» остаётся недоступным
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBAV IMPORTED_TARGET libavcodec libavutil)
        if(LIBAV_FOUND)
            target_link_libraries(nameless-orbit PRIVATE PkgConfig::LIBAV)
            target_compile_definitions(nameless-orbit PRIVATE ORBIT_HAVE_FFMPEG)
            message(STATUS "Видео H.264 на ПК: libavcodec ${LIBAV_libavcodec_VERSION}")
        endif()
    endif()
endif()

# Android: свой манифест (разрешения для сети, вибрации, уведомлений) на основе шаблона Qt
if(ANDROID)
    # код версии растёт вместе с версией проекта (0.2.4 → 204): Android ставит новый APK поверх старого только при большем коде
    math(EXPR ORBIT_VERSION_CODE "${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")
    set_target_properties(nameless-orbit PROPERTIES
        QT_ANDROID_VERSION_NAME ${PROJECT_VERSION}
        QT_ANDROID_VERSION_CODE ${ORBIT_VERSION_CODE}
        QT_ANDROID_PACKAGE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/android
        QT_ANDROID_MIN_SDK_VERSION 28
        QT_ANDROID_TARGET_SDK_VERSION 35
        QT_ANDROID_COMPILE_SDK_VERSION 36       # установлена android-36; android-37 в SDK лежит как «37.0» и не находится
        QT_ANDROID_PACKAGE_NAME org.glassrice.namelessorbit
        QT_ANDROID_APP_NAME Orbit)                # название под значком и в списке приложений
endif()

set_target_properties(nameless-orbit PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

install(TARGETS nameless-orbit
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib      # на Android цель собирается как модуль (.so), ему нужен свой каталог
)

# Самодостаточная папка (для deploy/package.sh: архив и AppImage): библиотеки Qt, плагины и модули QML кладутся рядом с программой.
# Скрипт упаковки готовится всегда, но срабатывает только когда установку просят о нём (cmake -DORBIT_BUNDLE_QT=ON -P …, как делает package.sh):
# обычная установка и пакет Arch используют системный Qt и ничего не копируют
if(UNIX AND NOT APPLE AND NOT ANDROID)
    set_target_properties(nameless-orbit PROPERTIES INSTALL_RPATH "\$ORIGIN/../lib")
    qt_generate_deploy_qml_app_script(
        TARGET nameless-orbit
        OUTPUT_SCRIPT orbit_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
        NO_TRANSLATIONS
    )
    install(CODE "if(ORBIT_BUNDLE_QT)\n    include(\"${orbit_deploy_script}\")\nendif()")
endif()

# .desktop и иконка для Linux (меню приложений, Wayland берёт значок по StartupWMClass/desktop-файлу)
if(UNIX AND NOT APPLE)
    install(FILES packaging/nameless-orbit.desktop DESTINATION share/applications)
    install(FILES appicon/appicon-256.png DESTINATION share/icons/hicolor/256x256/apps RENAME nameless-orbit.png)
endif()
