#!/usr/bin/env bash
# Вариант Б: самодостаточная сборка для ЛЮБОГО Linux (Qt и библиотеки внутри): папка, .tar.gz и .AppImage.
#   packaging/make-bundle.sh
# Сам настраивает и собирает проект (Release), затем упаковывает его скриптом deploy/package.sh.
# Где лежит package.sh: переменная DEPLOY_DIR (по умолчанию ~/drones/deploy). Результат — DEPLOY_DIR/dist/orbit/.
# Вариант А (для Arch / AUR, системный Qt) — packaging/make-release.sh.
set -euo pipefail

cd "$(dirname "$0")/.."
DEPLOY_DIR="${DEPLOY_DIR:-$HOME/drones/deploy}"
[[ -x "$DEPLOY_DIR/package.sh" ]] || { echo "Не нашёл $DEPLOY_DIR/package.sh. Укажите папку: DEPLOY_DIR=/путь/к/deploy packaging/make-bundle.sh" >&2; exit 1; }

echo "  -> собираю Orbit (Release)"
cmake -S . -B build/bundle -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/bundle

echo "  -> упаковываю (Qt, плагины, glibc — внутрь пакета)"
"$DEPLOY_DIR/package.sh" "$PWD/build/bundle"
echo "  Готово: $DEPLOY_DIR/dist/orbit/ — папка, .tar.gz и .AppImage"
