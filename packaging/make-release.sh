#!/usr/bin/env bash
# Собирает готовый архив для релиза и пакета nameless-orbit-bin в AUR.
#   packaging/make-release.sh            версия берётся из CMakeLists.txt
# Результат: dist/nameless-orbit-<версия>-linux-x86_64.tar.gz (внутри usr/bin, usr/share…) и его sha256 — вставьте в PKGBUILD.
set -euo pipefail

cd "$(dirname "$0")/.."
VER="$(grep -m1 -oP 'project\(nameless-orbit VERSION \K[0-9.]+' CMakeLists.txt)"
OUT="dist/nameless-orbit-$VER-linux-x86_64.tar.gz"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

echo "  -> собираю Orbit $VER (Release)"
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr >/dev/null
cmake --build build/release
DESTDIR="$STAGE" cmake --install build/release >/dev/null
install -Dm644 LICENSE "$STAGE/usr/share/licenses/nameless-orbit-bin/LICENSE"
strip --strip-unneeded "$STAGE/usr/bin/nameless-orbit" 2>/dev/null || true

mkdir -p dist
tar -C "$STAGE" -czf "$OUT" usr
echo "  -> готово: $OUT"
sha256sum "$OUT"
echo "  Дальше: загрузите архив в GitHub Release v$VER и вставьте sha256 в packaging/arch-bin/PKGBUILD"
