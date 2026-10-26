#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"

PKGNAME="shuttle-control"
PKGVER="0.1.0"

CACHE_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/shuttle-control-apk"

mkdir -p "$CACHE_ROOT"
WORK_ROOT="$(mktemp -d "$CACHE_ROOT/build.XXXXXX")"

trap 'rm -rf "$WORK_ROOT"' EXIT HUP INT TERM

APKBUILD_DIR="$WORK_ROOT/apkbuild"
SOURCE_ROOT="$WORK_ROOT/source"
SOURCE_DIR="$SOURCE_ROOT/$PKGNAME-$PKGVER"

mkdir -p "$APKBUILD_DIR" "$SOURCE_DIR/packaging"

if ! command -v abuild >/dev/null 2>&1; then
    echo "Ошибка: abuild не установлен." >&2
    echo "Установите пакет alpine-sdk." >&2
    exit 1
fi

# Копируем рецепт в рабочую директорию:
# abuild checksum не изменит файл в репозитории проекта.
cp "$PROJECT_ROOT/packaging/alpine/APKBUILD" \
    "$APKBUILD_DIR/APKBUILD"

# Минимальный набор исходников, необходимых для сборки.
cp "$PROJECT_ROOT/CMakeLists.txt" "$SOURCE_DIR/"
cp -R "$PROJECT_ROOT/src" "$PROJECT_ROOT/qml" "$SOURCE_DIR/"

cp "$PROJECT_ROOT/packaging/org.example.shuttle-control.desktop" \
    "$SOURCE_DIR/packaging/"

if [ -f "$PROJECT_ROOT/README.md" ]; then
    cp "$PROJECT_ROOT/README.md" "$SOURCE_DIR/"
fi

# Создаём архив с корневым каталогом,
# соответствующим переменной builddir в APKBUILD.
ARCHIVE="$APKBUILD_DIR/$PKGNAME-$PKGVER.tar.gz"

tar -czf "$ARCHIVE" \
    -C "$SOURCE_ROOT" \
    "$PKGNAME-$PKGVER"

(
    cd "$APKBUILD_DIR"

    echo "Расчёт контрольных сумм..."
    abuild checksum

    echo "Сборка Alpine-пакета..."
    abuild -r
)

echo
echo "Сборка завершена."
echo "Найти пакет можно командой:"
echo "  find \"$HOME/packages\" -type f -name '$PKGNAME-$PKGVER-r*.apk' -print"
