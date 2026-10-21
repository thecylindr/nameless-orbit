#!/usr/bin/env bash
# Выпуск Orbit на GitHub одной командой: контрольные суммы, подпись, релиз, проверка.
#   packaging/publish-release.sh --dry-run                       показать, что будет сделано, ничего не загружая
#   packaging/publish-release.sh                                 версия — из CMakeLists.txt, архивы — найдутся сами
#   packaging/publish-release.sh 0.2.4 --apk ~/Загрузки/Orbit.apk --push      версию можно написать просто числом
#
# Ключи:
#   --version X.Y.Z   версия (по умолчанию — из CMakeLists.txt)
#   --tag ИМЯ         тег релиза (по умолчанию vX.Y.Z; старые релизы без «v» — например --tag 0.2.3)
#   --dir ПАПКА       где лежат nameless-orbit-X-linux-x86_64.tar.gz, …AppImage, …bundle…; по умолчанию ищется сам:
#                     ~/drones/deploy/dist/orbit (make-bundle.sh, DEPLOY_DIR), затем dist/ (make-release.sh)
#   --apk ФАЙЛ        APK для телефона (подписанный, как из Qt Creator)
#   --sign            подписать SHA256SUMS своим ключом GPG → SHA256SUMS.asc (ключ: GPG_KEY=<id или почта>, иначе ключ по умолчанию)
#   --notes ФАЙЛ      описание релиза (по умолчанию — раздел этой версии из CHANGELOG.md)
#   --push            нет тега на GitHub — создать (если нужно) и отправить его в hub и flic
#   --draft           черновик: опубликовать потом кнопкой в браузере
#   --repo ВЛАДЕЛЕЦ/ИМЯ   репозиторий (по умолчанию thecylindr/nameless-orbit)
#   --dry-run         только показать план
# Уже выпущенный релиз: недостающие файлы (например, забытый SHA256SUMS) догружаются, существующие не подменяются —
# у тех, кто уже скачал, разошлась бы контрольная сумма. Нашли ошибку в файле — выпускайте следующую версию
set -euo pipefail

# путь к себе — до смены папки: запуск из packaging/ («./publish-release.sh») иначе ломал --help
SELF="$(readlink -f "$0")"
cd "$(dirname "$SELF")/.."
REPO="${REPO:-thecylindr/nameless-orbit}"
VER="" TAG="" DIR="" APK="" SIGN=0 NOTES="" PUSH=0 DRAFT=0 DRY=0

die() { printf '\n\033[31mОшибка:\033[0m %s\n' "$*" >&2; exit 1; }
say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
run() { if [ "$DRY" = 1 ]; then printf '   [план] %s\n' "$*"; else "$@"; fi; }

while [ $# -gt 0 ]; do
    case "$1" in
        --version) VER="${2:?}"; shift 2 ;;
        --tag) TAG="${2:?}"; shift 2 ;;
        --dir) DIR="${2:?}"; shift 2 ;;
        --apk) APK="${2:?}"; shift 2 ;;
        --sign) SIGN=1; shift ;;
        --notes) NOTES="${2:?}"; shift 2 ;;
        --push) PUSH=1; shift ;;
        --draft) DRAFT=1; shift ;;
        --repo) REPO="${2:?}"; shift 2 ;;
        --dry-run) DRY=1; shift ;;
        -h|--help) sed -n '2,24p' "$SELF" | sed 's/^# \{0,1\}//'; exit 0 ;;
        [0-9]*.[0-9]*.[0-9]*) VER="$1"; shift ;;          # версия просто числом: publish-release.sh 0.2.4
        *) die "неизвестный ключ $1 (см. --help)" ;;
    esac
done

[ -n "$VER" ] || VER="$(grep -m1 -oP 'project\(nameless-orbit VERSION \K[0-9.]+' CMakeLists.txt)"
[[ "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "версия должна быть вида 0.2.4, а не «$VER»"
TAG="${TAG:-v$VER}"
# папка с архивами: сборка make-bundle.sh (для install.sh на любом Linux) или make-release.sh
if [ -z "$DIR" ]; then
    for d in "${DEPLOY_DIR:-$HOME/drones/deploy}/dist/orbit" dist; do
        [ -f "$d/nameless-orbit-$VER-linux-x86_64.tar.gz" ] && { DIR="$d"; break; }
    done
    [ -n "$DIR" ] || die "не нашёл nameless-orbit-$VER-linux-x86_64.tar.gz ни в ${DEPLOY_DIR:-$HOME/drones/deploy}/dist/orbit, ни в dist/ — соберите: packaging/make-bundle.sh (или укажите папку: --dir …)"
fi
DIR="${DIR%/}"
[ -d "$DIR" ] || die "нет папки $DIR"

# ---- что выкладывается ----------------------------------------------------------------------------------------------
say "Orbit $VER → релиз $TAG в $REPO (файлы из $DIR)"
FILES=()
for f in "nameless-orbit-$VER-linux-x86_64.tar.gz" "nameless-orbit-$VER-bundle-linux-x86_64.tar.gz" "nameless-orbit-$VER-linux-x86_64.AppImage"; do
    [ -f "$DIR/$f" ] && FILES+=("$DIR/$f")
done
# без этого архива install.sh orbit не найдёт, что ставить
[ -f "$DIR/nameless-orbit-$VER-linux-x86_64.tar.gz" ] || die "нет $DIR/nameless-orbit-$VER-linux-x86_64.tar.gz — соберите его: packaging/make-bundle.sh"
if [ -n "$APK" ]; then
    [ -f "$APK" ] || die "нет APK: $APK"
    [[ "$APK" != *unsigned* ]] || die "APK неподписанный ($APK) — телефон его не поставит; соберите с «Подписать пакет» в Qt Creator"
    FILES+=("$APK")
fi
for f in "${FILES[@]}"; do printf '   %-60s %s\n' "$(basename "$f")" "$(du -h "$f" | cut -f1)"; done

command -v gh >/dev/null || die "нет gh: sudo pacman -S github-cli, затем gh auth login"
gh auth status >/dev/null 2>&1 || die "gh не вошёл в GitHub: gh auth login"

# ---- контрольные суммы и подпись --------------------------------------------------------------------------------------
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
for f in "${FILES[@]}"; do (cd "$(dirname "$f")" && sha256sum "$(basename "$f")"); done > "$STAGE/SHA256SUMS"
say "SHA256SUMS"
sed 's/^/   /' "$STAGE/SHA256SUMS"
EXTRA=("$STAGE/SHA256SUMS")
if [ "$SIGN" = 1 ]; then
    command -v gpg >/dev/null || die "нет gpg"
    gpg --list-secret-keys ${GPG_KEY:+"$GPG_KEY"} >/dev/null 2>&1 || die "нет секретного ключа GPG${GPG_KEY:+ «$GPG_KEY»}: создайте — gpg --full-generate-key (см. мануал 09)"
    say "Подпись SHA256SUMS ключом ${GPG_KEY:-по умолчанию}"
    run gpg --armor --detach-sign ${GPG_KEY:+--local-user "$GPG_KEY"} --output "$STAGE/SHA256SUMS.asc" "$STAGE/SHA256SUMS"
    EXTRA+=("$STAGE/SHA256SUMS.asc")
fi

# ---- тег ---------------------------------------------------------------------------------------------------------------
if ! git ls-remote --exit-code --tags hub "refs/tags/$TAG" >/dev/null 2>&1; then
    [ "$PUSH" = 1 ] || die "тега $TAG нет на GitHub. Отправьте его: git tag $TAG && git push hub $TAG (или добавьте --push)"
    say "Тег $TAG → GitHub и GitFlic"
    git rev-parse -q --verify "refs/tags/$TAG" >/dev/null || run git tag "$TAG"
    run git push hub "$TAG"
    git remote | grep -qx flic && run git push flic "$TAG" || true
fi

# ---- описание из CHANGELOG ----------------------------------------------------------------------------------------------
if [ -z "$NOTES" ]; then
    NOTES="$STAGE/notes.md"
    awk -v v="$VER" '$0 ~ "^## \\[" v "\\]" { on = 1; next } on && /^## \[/ { exit } on' CHANGELOG.md > "$NOTES"
    [ -s "$NOTES" ] || echo "Orbit $VER" > "$NOTES"
fi

# ---- релиз ---------------------------------------------------------------------------------------------------------------
if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
    # уже есть: только недостающие файлы
    HAVE="$(gh release view "$TAG" --repo "$REPO" --json assets -q '.assets[].name')"
    MISSING=()
    for f in "${FILES[@]}" "${EXTRA[@]}"; do grep -qx "$(basename "$f")" <<<"$HAVE" || MISSING+=("$f"); done
    if [ ${#MISSING[@]} -eq 0 ]; then say "Релиз $TAG уже содержит все файлы"
    else
        say "Релиз $TAG есть — догружаю: $(for f in "${MISSING[@]}"; do printf '%s ' "$(basename "$f")"; done)"
        run gh release upload "$TAG" "${MISSING[@]}" --repo "$REPO"
    fi
else
    say "Создаю релиз $TAG$( [ "$DRAFT" = 1 ] && echo ' (черновик)')"
    run gh release create "$TAG" "${FILES[@]}" "${EXTRA[@]}" --repo "$REPO" --title "Orbit $VER" --notes-file "$NOTES" $( [ "$DRAFT" = 1 ] && echo --draft )
fi

# ---- проверка: суммы GitHub совпадают с SHA256SUMS ---------------------------------------------------------------------
[ "$DRY" = 1 ] && { say "План показан, ничего не загружено"; exit 0; }
say "Проверка"
gh release view "$TAG" --repo "$REPO" --json assets -q '.assets[] | "\(.digest | sub("sha256:"; ""))  \(.name)"' > "$STAGE/remote"
grep -qx '.*  SHA256SUMS' "$STAGE/remote" || die "в релизе нет SHA256SUMS — install.sh orbit у пользователей не сработает"
BAD=0
while read -r sum name; do
    remote="$(awk -v n="$name" '$2 == n { print $1 }' "$STAGE/remote")"
    if [ "$remote" = "$sum" ]; then printf '   \033[32mсовпадает\033[0m  %s\n' "$name"
    else printf '   \033[31mНЕ совпадает\033[0m  %s (в релизе другой файл)\n' "$name"; BAD=1; fi
done < "$STAGE/SHA256SUMS"
[ "$BAD" = 0 ] || die "суммы не сошлись: в релизе лежат другие файлы, чем в $DIR — выпустите следующую версию"
say "Готово: https://github.com/$REPO/releases/tag/$TAG"
echo "Проверка установки на чистом месте:  tmp=\$(mktemp -d); HOME=\$tmp ~/newdesign/actually/nameless-ui/install.sh orbit"
