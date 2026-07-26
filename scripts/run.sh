#!/usr/bin/env bash
# ============================================================================
#  Запуск Director 3D одной командой: ./scripts/run.sh
#
#  Скрипт делает всё, что нужно от «склонировал репозиторий» до «окно открыто»:
#  проверяет инструменты, находит или клонирует движок, настраивает сборку,
#  собирает и запускает.
#
#  Смысл не в экономии трёх команд, а в ДИАГНОСТИКЕ. Голый cmake при нехватке
#  системного пакета падает на сотой строке вывода сообщением про отсутствующий
#  заголовок, по которому непонятно, что именно доставить. Здесь каждая
#  проверка отвечает конкретной командой установки.
# ============================================================================
set -u

RED=$'\033[31m'; GREEN=$'\033[32m'; YELLOW=$'\033[33m'; DIM=$'\033[2m'; OFF=$'\033[0m'
say()  { printf '%s\n' "$*"; }
ok()   { printf '%s✓%s %s\n' "$GREEN" "$OFF" "$*"; }
warn() { printf '%s!%s %s\n' "$YELLOW" "$OFF" "$*"; }
die()  { printf '%s✗ %s%s\n' "$RED" "$*" "$OFF" >&2; exit 1; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || die "не удалось перейти в каталог проекта"

BUILD_DIR="${D3D_BUILD_DIR:-build}"
BUILD_TYPE="${D3D_BUILD_TYPE:-Release}"

say "${DIM}Director 3D — сборка и запуск${OFF}"
say "${DIM}Каталог проекта: $ROOT${OFF}"
say ""

# --- 1. Инструменты сборки --------------------------------------------------
# Определяем менеджер пакетов, чтобы советовать команду, которая реально
# сработает у человека, а не абстрактное «поставьте cmake».
if   command -v apt-get >/dev/null 2>&1; then INSTALL="sudo apt install"
elif command -v dnf     >/dev/null 2>&1; then INSTALL="sudo dnf install"
elif command -v pacman  >/dev/null 2>&1; then INSTALL="sudo pacman -S"
elif command -v brew    >/dev/null 2>&1; then INSTALL="brew install"
else INSTALL="установите пакет"
fi

need() {
    command -v "$1" >/dev/null 2>&1 && return 0
    die "не найден $1. Поставьте так: $INSTALL $2"
}
need cmake cmake
need git git
if   command -v c++ >/dev/null 2>&1; then :
elif command -v g++ >/dev/null 2>&1; then :
else die "не найден компилятор C++. Поставьте так: $INSTALL g++"
fi
ok "инструменты сборки на месте"

GENERATOR=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR=(-G Ninja)
else
    warn "ninja не найден — собираем через make (медленнее). Ускорить: $INSTALL ninja-build"
fi

# --- 2. Движок --------------------------------------------------------------
# Director 3D — приложение ПОВЕРХ движка, и без него собрать нечего. Порядок
# поиска тот же, что у CMakeLists: явный путь, соседний каталог, клонирование.
ENGINE="${SAGE_ENGINE_DIR:-}"
if [ -z "$ENGINE" ]; then
    for candidate in "$ROOT/../SAGE-Engine" "$ROOT/../sage-engine"; do
        if [ -f "$candidate/engine/CMakeLists.txt" ]; then
            ENGINE="$(cd "$candidate" && pwd)"
            break
        fi
    done
fi
if [ -z "$ENGINE" ]; then
    ENGINE="$ROOT/../SAGE-Engine"
    say "Движок рядом не найден — клонирую в $ENGINE"
    git clone --depth 1 https://github.com/AmckinatorStudios/SAGE-Engine.git "$ENGINE" \
        || die "не удалось склонировать движок. Если репозиторий приватный, склонируйте его рядом вручную."
fi
[ -f "$ENGINE/engine/CMakeLists.txt" ] || die "в $ENGINE нет движка (ожидался engine/CMakeLists.txt)"
ok "движок: $ENGINE"

# --- 3. Сборка --------------------------------------------------------------
say ""
say "${DIM}Настройка…${OFF}"
cmake -B "$BUILD_DIR" "${GENERATOR[@]}" \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DSAGE_ENGINE_DIR="$ENGINE" >/dev/null \
    || die "настройка не прошла. Полный вывод: cmake -B $BUILD_DIR -DSAGE_ENGINE_DIR=$ENGINE"

JOBS="$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) )"
say "${DIM}Сборка в $JOBS потоков (первый раз — несколько минут: качаются зависимости)…${OFF}"
cmake --build "$BUILD_DIR" -j"$JOBS" || die "сборка не прошла — вывод выше"
ok "собрано: $BUILD_DIR/Director3D"

# --- 4. Быстрая проверка ----------------------------------------------------
# Самотест не требует ни окна, ни видеокарты. Если он падает, окно открывать
# бессмысленно — проблема не в графике.
say ""
say "${DIM}Самопроверка ядра…${OFF}"
if "./$BUILD_DIR/Director3D" --self-test >/dev/null 2>&1; then
    ok "самопроверка пройдена"
else
    warn "самопроверка не прошла — запустите ./$BUILD_DIR/Director3D --self-test и посмотрите вывод"
fi

# --- 5. Запуск --------------------------------------------------------------
say ""
if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ] && [ "$(uname)" != "Darwin" ]; then
    warn "нет графического сеанса (DISPLAY пуст) — окно открыть некуда."
    say  "Собранный инструмент лежит здесь: $ROOT/$BUILD_DIR/Director3D"
    say  "Снять ролик без окна можно так:"
    say  "  xvfb-run -a ./$BUILD_DIR/Director3D --render out/demo.mp4 --showcase"
    exit 0
fi

say "${DIM}Запуск…${OFF}"
exec "./$BUILD_DIR/Director3D" "$@"
