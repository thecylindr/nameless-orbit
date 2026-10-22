# Orbit 0.2.5

Изменения и причины перечислены в [CHANGELOG.md](../CHANGELOG.md).

## Уведомления Codex

Настройки → «Ответы помощников · Claude и Codex» → включите отправку сообщений и «Сообщать о завершении ответа Codex». На принимающем устройстве оставьте включённым приём ответов помощников и разрешение уведомлений. Получатели должны быть сопряжены и подключены к Orbit.

На Linux переключатель подключает официальный [Codex notify](https://developers.openai.com/codex/config-advanced/#notifications): `agent-turn-complete`. Перезапустите уже открытые сессии Codex после настройки. Передаётся до 600 символов `last-assistant-message`. Orbit должен работать в фоне; hook не запускает новый экземпляр и не открывает окно.

Настройка сохраняет копию `config.toml.orbit-backup`, не читает `auth.json` и не перезаписывает существующий `notify`. При конфликте добавьте вызов `nameless-orbit --codex-notify '<JSON-событие>'` в свой обработчик. Отключение переключателя прекращает пересылку, оставляя hook настроенным. Если переместили бинарный файл, поправьте путь в `notify` вручную.

На Windows/macOS командный обработчик доступен в исходниках, но автоматическая установка hook реализована для Linux.

## Проверки

- Linux Release собран системным Qt 6.12.0.
- `cmake -S . -B build/Desktop_Release -DORBIT_BUILD_TESTS=ON`, затем сборка и `ctest --test-dir build/Desktop_Release --output-on-failure`: границы фрагментированных H.264 access units и восстановление настоящего libavcodec-декодера после перегрузки/перезапуска.
- `python3 -m unittest discover -s tests -p 'test_*.py' -v`: сохранение TOML и существующего hook, обработка некорректного события без GUI. Проверка доставки через Unix socket пропускается в среде, запрещающей локальные сокеты.
- Android ARM64: native library собирается Qt 6.11.1/NDK 27.2; изменённый Java-декодер отдельно компилируется против Android API 36.
- APK в текущей ограниченной среде не упакован: Gradle не может создать FileLockContentionHandler (`Could not determine a usable wildcard IP`). Повторите `cmake --build build/android_v8a_NDK_27_2_Release --target apk_all` в обычном окружении сборки.

Сравнение FPS, задержки и качества на настоящих телефонах и втором компьютере здесь не выполнялось. Изменения не являются подтверждённым исправлением внутреннего падения Quickshell в `QScreen::handle()`; устранено лишнее удаление действующего монитора повторным экземпляром Orbit.

## Артефакты

`dist/nameless-orbit-0.2.5-linux-x86_64.tar.gz` содержит `usr/bin` и desktop-файлы; это сборка с системными библиотеками Qt, не AppImage. `dist/nameless-orbit-0.2.5.patch` содержит изменения исходников. Автоматической установки или публикации релиза нет.
