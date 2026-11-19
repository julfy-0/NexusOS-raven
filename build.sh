#!/usr/bin/env bash
# NexusOS-P6P build — Google Pixel 6 Pro (raven, gs101, aarch64)
#
# Собирает kernel Image -> boot.img (header v4, только ядро; DTB и cmdline берутся
# из штатного vendor_boot). Источник правды по сборке — Makefile.pixel6pro,
# этот скрипт только красивый и безопасный фронтенд вокруг него.
# Сборка x86_64 ISO для ПК: ./build_pc.sh

set -u -o pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

# ─────────────────────────────────────────────────────────────────────────────
#  Конфигурация
# ─────────────────────────────────────────────────────────────────────────────
MK="Makefile.pixel6pro"
B="build/pixel6pro"
IMG="$B/boot.img"
LOG_FILE="build_p6p.log"
RAMOOPS_OUT="ramoops_p6p.txt"
CROSS="${CROSS:-aarch64-linux-gnu-}"
EXTRA_CFLAGS="${EXTRA:-}"
CLEAN=1; DRY=0; DO_BL=0; DO_BOOT=0; DO_LOG=0
FORCE_COLOR=0; FORCE_ASCII=0; NOCOLOR_OPT=0
JOBS="${NEXUS_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 1)}"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || JOBS=1

version() {
    local f=include/nexus/nexus_version.h
    [[ -f $f ]] && sed -n 's/^#define NEXUS_VERSION_DISPLAY "\(.*\)"/\1/p' "$f" | head -1
}
VERSION_STR="$(version)"; : "${VERSION_STR:=dev}"

# ─────────────────────────────────────────────────────────────────────────────
#  Аргументы (разбираем до оформления, чтобы --help и --no-color работали)
# ─────────────────────────────────────────────────────────────────────────────
usage_plain() {
cat <<USAGE
NexusOS-P6P build (Pixel 6 Pro, aarch64)

Usage: ./build.sh [options]

Build:
  --no-clean         keep $B (incremental build)
  --clean-only       clean and exit
  --dry-run          show make commands, compile nothing, delete nothing
  --cross PREFIX     cross-compiler prefix (default: $CROSS)
  --cflags "FLAGS"   extra compiler flags (same as EXTRA=...)
  --pktcnt           build with -DTOUCH_PKTCNT=1 (touch SPI PACKET_CNT experiment)
  -j N               make jobs (default: $JOBS)

Phone (needs adb/fastboot; in WSL use FASTBOOT=fastboot.exe ADB=adb.exe):
  --bootloader       adb reboot bootloader
  --boot             wait for fastboot, then: fastboot boot $IMG
  --log              fetch ramoops after you boot back into Android (needs root)

Look & feel:
  --color            force colors and the live progress bar (even when piped)
  --no-color         plain text (also honors NO_COLOR)
  --ascii            ASCII-only symbols (for terminals without UTF-8)
  -h, --help         this help

Environment: CROSS, EXTRA, NEXUS_BUILD_JOBS, FASTBOOT, ADB, NO_COLOR
Full cycle:  ./build.sh --bootloader --boot --log
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-clean)   CLEAN=0 ;;
        --clean-only) CLEAN=2 ;;
        --dry-run)    DRY=1 ;;
        --cross)      [[ $# -ge 2 ]] || { echo "--cross needs a value" >&2; exit 2; }; CROSS="$2"; shift ;;
        --cflags)     [[ $# -ge 2 ]] || { echo "--cflags needs a value" >&2; exit 2; }; EXTRA_CFLAGS="$EXTRA_CFLAGS $2"; shift ;;
        --pktcnt)     EXTRA_CFLAGS="$EXTRA_CFLAGS -DTOUCH_PKTCNT=1" ;;
        -j)           [[ $# -ge 2 && "$2" =~ ^[1-9][0-9]*$ ]] || { echo "-j needs a number" >&2; exit 2; }; JOBS="$2"; shift ;;
        --bootloader) DO_BL=1 ;;
        --boot)       DO_BOOT=1 ;;
        --log)        DO_LOG=1 ;;
        --color)      FORCE_COLOR=1 ;;
        --no-color)   NOCOLOR_OPT=1 ;;
        --ascii)      FORCE_ASCII=1 ;;
        -h|--help)    usage_plain; exit 0 ;;
        *)            usage_plain >&2; echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

# ─────────────────────────────────────────────────────────────────────────────
#  Возможности терминала
# ─────────────────────────────────────────────────────────────────────────────
COLS="$(tput cols 2>/dev/null || true)"
[[ "$COLS" =~ ^[0-9]+$ ]] || COLS="${COLUMNS:-80}"
[[ "$COLS" =~ ^[0-9]+$ ]] || COLS=80
(( COLS > 100 )) && COLS=100
(( COLS < 40 )) && COLS=40

IS_TTY=0; [[ -t 1 ]] && IS_TTY=1
USE_COLOR=$IS_TTY
[[ -n "${NO_COLOR:-}" || "${TERM:-}" == "dumb" ]] && USE_COLOR=0
(( FORCE_COLOR )) && USE_COLOR=1
(( NOCOLOR_OPT )) && USE_COLOR=0
LIVE=$USE_COLOR                       # живой прогресс-бар только на цветном терминале

UTF=1
case "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" in *[Uu][Tt][Ff]-8*|*[Uu][Tt][Ff]8*) ;; *) UTF=0 ;; esac
(( FORCE_ASCII )) && UTF=0

TRUECOLOR=0
case "${COLORTERM:-}" in *truecolor*|*24bit*) TRUECOLOR=1 ;; esac

# Цвет по RGB: truecolor или ближайший из 256-цветной палитры.
fg() {
    (( USE_COLOR )) || return 0
    if (( TRUECOLOR )); then printf '\e[38;2;%d;%d;%dm' "$1" "$2" "$3"
    else printf '\e[38;5;%dm' $(( 16 + 36 * ($1 * 5 / 255) + 6 * ($2 * 5 / 255) + ($3 * 5 / 255) ))
    fi
}
# Палитра в тон темы NexusOS (cyan -> blue -> indigo).
if (( USE_COLOR )); then
    RST=$'\e[0m'; BOLD=$'\e[1m'; DIM=$'\e[2m'
    C_ACC="$(fg 0 208 255)";  C_BLUE="$(fg 56 189 248)"; C_VIO="$(fg 129 140 248)"
    C_OK="$(fg 34 197 94)";   C_ERR="$(fg 239 68 68)";   C_WARN="$(fg 250 204 21)"
    C_GRY="$(fg 148 163 184)"; C_WHT="$(fg 248 250 252)"
else
    RST=; BOLD=; DIM=; C_ACC=; C_BLUE=; C_VIO=; C_OK=; C_ERR=; C_WARN=; C_GRY=; C_WHT=
fi

if (( UTF )); then
    S_OK="✔"; S_FAIL="✖"; S_WARN="▲"; S_ARR="▸"; S_DOT="·"; S_BAR="┃"; S_RULE="─"
    BAR_F="━"; BAR_E="─"; BAR_H="╸"; BLK_F="█"; BLK_E="░"
    SPIN=(⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏)
else
    S_OK="OK"; S_FAIL="XX"; S_WARN="!!"; S_ARR=">"; S_DOT="-"; S_BAR="|"; S_RULE="-"
    BAR_F="#"; BAR_E="-"; BAR_H=">"; BLK_F="#"; BLK_E="."
    SPIN=('|' '/' '-' '\')
fi

hide_cursor() { (( LIVE )) && printf '\e[?25l'; }
show_cursor() { (( LIVE )) && printf '\e[?25h'; }
clear_line()  { (( LIVE )) && printf '\r\e[K'; }
repeat()      { local s="" i; for ((i = 0; i < $2; i++)); do s+="$1"; done; printf '%s' "$s"; }

on_exit() { show_cursor; }
on_int()  { trap - INT TERM; show_cursor; pkill -P $$ 2>/dev/null; printf '\n  %s%s interrupted%s\n' "$C_WARN" "$S_WARN" "$RST"; exit 130; }
trap on_exit EXIT
trap on_int INT TERM

# ─────────────────────────────────────────────────────────────────────────────
#  Утилиты оформления
# ─────────────────────────────────────────────────────────────────────────────
now_ms() {
    if [[ -n "${EPOCHREALTIME:-}" ]]; then local t="${EPOCHREALTIME/[.,]/}"; echo $(( t / 1000 ))
    else echo $(( $(date +%s) * 1000 )); fi
}
fmt_ms() {
    local ms=$1
    if (( ms < 1000 )); then printf '%dms' "$ms"
    elif (( ms < 60000 )); then printf '%d.%ds' $(( ms / 1000 )) $(( ms % 1000 / 100 ))
    else printf '%dm %02ds' $(( ms / 60000 )) $(( ms % 60000 / 1000 )); fi
}
hsize() {   # байты -> человекочитаемо
    awk -v b="$1" 'BEGIN{ if (b<1024) printf "%d B", b; else if (b<1048576) printf "%.1f KiB", b/1024; else printf "%.2f MiB", b/1048576 }'
}
# Текст с горизонтальным градиентом: gtext "строка" r1 g1 b1 r2 g2 b2
gtext() {
    local s="$1" n=${#1} i r g b
    if (( ! USE_COLOR )); then printf '%s' "$s"; return; fi
    for ((i = 0; i < n; i++)); do
        local d=$(( n > 1 ? n - 1 : 1 ))
        r=$(( $2 + ($5 - $2) * i / d )); g=$(( $3 + ($6 - $3) * i / d )); b=$(( $4 + ($7 - $4) * i / d ))
        printf '%s%s' "$(fg $r $g $b)" "${s:i:1}"
    done
    printf '%s' "$RST"
}
section() {   # ── Заголовок ─────────────
    local t="$1" w=$(( COLS - 6 - ${#1} ))
    (( w < 4 )) && w=4
    printf '\n  %s%s%s %s%s%s %s%s%s\n' "$C_GRY" "$(repeat "$S_RULE" 2)" "$RST" "$BOLD$C_WHT" "$t" "$RST" "$C_GRY" "$(repeat "$S_RULE" "$w")" "$RST"
}
kv() { printf '  %s%s%s %s%-9s%s %s\n' "$C_VIO" "$S_BAR" "$RST" "$C_GRY" "$1" "$RST" "$2"; }

banner() {
    local n0=("███╗   ██╗" "████╗  ██║" "██╔██╗ ██║" "██║╚██╗██║" "██║ ╚████║" "╚═╝  ╚═══╝")
    local e0=("███████╗" "██╔════╝" "█████╗  " "██╔══╝  " "███████╗" "╚══════╝")
    local x0=("██╗  ██╗" "╚██╗██╔╝" " ╚███╔╝ " " ██╔██╗ " "██╔╝ ██╗" "╚═╝  ╚═╝")
    local u0=("██╗   ██╗" "██║   ██║" "██║   ██║" "██║   ██║" "╚██████╔╝" " ╚═════╝ ")
    local s0=("███████╗" "██╔════╝" "███████╗" "╚════██║" "███████║" "╚══════╝")
    local o0=(" ██████╗ " "██╔═══██╗" "██║   ██║" "██║   ██║" "╚██████╔╝" " ╚═════╝ ")
    echo
    if (( UTF && COLS >= 66 )); then
        local i line
        for i in 0 1 2 3 4 5; do
            line="${n0[i]}${e0[i]}${x0[i]}${u0[i]}${s0[i]}${o0[i]}${s0[i]}"
            printf '  '; gtext "$line" 0 208 255 129 140 248; printf '\n'
        done
    else
        printf '  %s%sN E X U S O S%s\n' "$BOLD" "$C_ACC" "$RST"
    fi
    printf '\n  %sPixel 6 Pro%s %s%s%s raven %s%s%s gs101 %s%s%s aarch64          %sv%s%s\n' \
        "$BOLD$C_WHT" "$RST" "$C_GRY" "$S_DOT" "$RST" "$C_GRY" "$S_DOT" "$RST" "$C_GRY" "$S_DOT" "$RST" "$C_VIO" "$VERSION_STR" "$RST"
}

# ── строки стадий ────────────────────────────────────────────────────────────
STAGE_TOTAL=$(( 5 + DO_BL + 2 * DO_BOOT + DO_LOG ))
STAGE_N=0
declare -A STAGE_MS
stage_tag() { printf '%s[%d/%d]%s' "$DIM" "$STAGE_N" "$STAGE_TOTAL" "$RST"; }
stage_done() {  # symbol color label detail ms
    [[ -n "${5:-}" ]] && STAGE_MS["$3"]="$5"
    clear_line
    printf '  %s%s%s %s %s%-11s%s %s' "$2" "$1" "$RST" "$(stage_tag)" "$BOLD" "$3" "$RST" "$4"
    [[ -n "${5:-}" ]] && printf '  %s%s %s%s' "$DIM" "$S_DOT" "$(fmt_ms "$5")" "$RST"
    printf '\n'
}
stage_ok()   { stage_done "$S_OK"   "$C_OK"   "$1" "$2" "${3:-}"; }
stage_fail() { stage_done "$S_FAIL" "$C_ERR"  "$1" "$2" "${3:-}"; }
stage_warn() { stage_done "$S_WARN" "$C_WARN" "$1" "$2" "${3:-}"; }
stage_next() { STAGE_N=$(( STAGE_N + 1 )); }
die() { show_cursor; printf '\n  %s%s %s%s\n\n' "$C_ERR$BOLD" "$S_FAIL" "$*" "$RST" >&2; exit 1; }

# Прогресс-бар с градиентом cyan -> indigo
draw_bar() {   # pct width
    local pct=$1 w=$2 i filled out="" r g b
    filled=$(( pct * w / 100 ))
    for ((i = 0; i < w; i++)); do
        if (( i < filled )); then
            r=$(( 0 + 129 * i / w )); g=$(( 208 - 68 * i / w )); b=$(( 255 - 7 * i / w ))
            out+="$(fg $r $g $b)${BAR_F}"
        elif (( i == filled && pct < 100 )); then out+="${C_VIO}${BAR_H}"
        else out+="${DIM}${C_GRY}${BAR_E}"; fi
    done
    printf '%s%s' "$out" "$RST"
}

# Спиннер на время ожидания: spin_until "текст" таймаут_сек команда...
spin_until() {
    local label="$1" tmo="$2"; shift 2
    local i=0 ticks=$(( tmo * 10 ))
    hide_cursor
    while (( i < ticks )); do
        if (( i % 10 == 0 )) && "$@" >/dev/null 2>&1; then clear_line; show_cursor; return 0; fi
        if (( LIVE )); then
            printf '\r\e[K  %s%s%s %s %s%-11s%s %s%s%s' "$C_ACC" "${SPIN[i % ${#SPIN[@]}]}" "$RST" "$(stage_tag)" "$BOLD" "${STAGE_LABEL:-wait}" "$RST" "$DIM" "$label ($(( i / 10 ))s/${tmo}s)" "$RST"
        fi
        sleep 0.1; i=$(( i + 1 ))
    done
    clear_line; show_cursor; return 1
}

# ─────────────────────────────────────────────────────────────────────────────
#  Старт: баннер и конфигурация
# ─────────────────────────────────────────────────────────────────────────────
banner

section "Configuration"
kv "target"   "boot.img v4 ${S_DOT} kernel-only ${S_DOT} DTB/cmdline from vendor_boot"
kv "makefile" "$MK"
kv "compiler" "${CROSS}gcc"
kv "jobs"     "$JOBS"
[[ -n "${EXTRA_CFLAGS// }" ]] && kv "flags" "${EXTRA_CFLAGS# }"
kv "mode"     "$( ((DRY)) && echo 'dry-run' || { ((CLEAN == 1)) && echo 'clean build' || echo 'incremental'; })$( ((DO_BL)) && echo " ${S_DOT} bootloader" )$( ((DO_BOOT)) && echo " ${S_DOT} fastboot boot" )$( ((DO_LOG)) && echo " ${S_DOT} ramoops log" )"
if git rev-parse --short HEAD >/dev/null 2>&1; then
    kv "commit" "$(git rev-parse --short HEAD)$(git diff --quiet 2>/dev/null || echo ' (dirty)')"
fi
kv "date"     "$(date '+%Y-%m-%d %H:%M:%S')"

section "Build"
T_ALL=$(now_ms)

# ── 1. Toolchain ─────────────────────────────────────────────────────────────
stage_next; t0=$(now_ms)
tools=(make python3)
(( DRY )) || tools+=("${CROSS}gcc" "${CROSS}ld" "${CROSS}objcopy")
missing=()
for t in "${tools[@]}"; do command -v "$t" >/dev/null 2>&1 || missing+=("$t"); done
for f in "$MK" tools/mkbootimg_pixel.py kernel/arch/aarch64/linker.ld; do [[ -f $f ]] || missing+=("file:$f"); done
if (( ${#missing[@]} )); then
    stage_fail "Toolchain" "missing: ${missing[*]}"
    printf '\n  %sInstall the cross-compiler:%s\n' "$C_GRY" "$RST"
    printf '    %ssudo apt install make python3 gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu%s\n' "$C_WHT" "$RST"
    printf '    %s./build.sh --cross aarch64-none-elf-%s   %s# other prefix%s\n\n' "$C_WHT" "$RST" "$DIM" "$RST"
    exit 1
fi
if (( DRY )); then gccv="dry-run (compiler not required)"
else gccv="$("${CROSS}gcc" --version 2>/dev/null | head -1)"; fi
stage_ok "Toolchain" "$gccv" "$(( $(now_ms) - t0 ))"

# ── 2. Clean ─────────────────────────────────────────────────────────────────
stage_next; t0=$(now_ms)
if (( DRY )); then
    stage_ok "Clean" "skipped (dry-run)" "$(( $(now_ms) - t0 ))"
elif (( CLEAN )); then
    make -f "$MK" clean >/dev/null 2>&1 || true
    rm -rf "$B"
    stage_ok "Clean" "$B" "$(( $(now_ms) - t0 ))"
    if (( CLEAN == 2 )); then printf '\n  %s%s clean complete%s\n\n' "$C_OK" "$S_OK" "$RST"; exit 0; fi
else
    stage_ok "Clean" "skipped (incremental)" "$(( $(now_ms) - t0 ))"
fi

# ── 3. Compile ───────────────────────────────────────────────────────────────
stage_next; t0=$(now_ms)
MAKE_ARGS=(-f "$MK" "-j$JOBS" "CROSS=$CROSS")
[[ -n "${EXTRA_CFLAGS// }" ]] && MAKE_ARGS+=("EXTRA=$EXTRA_CFLAGS")

if (( DRY )); then
    printf '\n'
    make -n "${MAKE_ARGS[@]}"
    printf '\n  %s%s dry-run complete%s\n\n' "$C_OK" "$S_OK" "$RST"
    exit 0
fi

# Сколько шагов предстоит (для процентов): компиляции, линковка, objcopy, mkbootimg.
TOTAL=$(make -n "${MAKE_ARGS[@]}" 2>/dev/null | grep -Ec '^[^ ]*(gcc|ld|objcopy|python3?) ' || true)
[[ "$TOTAL" =~ ^[0-9]+$ ]] || TOTAL=0

DONE=0; MAKE_RC=1; WARN_LIST=(); ERR_LIST=(); CUR="starting"; SPIN_I=0
: > "$LOG_FILE"
BARW=$(( COLS - 58 )); (( BARW < 10 )) && BARW=10; (( BARW > 32 )) && BARW=32
hide_cursor
draw_live() {
    (( LIVE )) || return 0
    local pct=100 name="$CUR"
    (( TOTAL > 0 )) && pct=$(( DONE * 100 / TOTAL ))
    (( pct > 100 )) && pct=100
    local room=$(( COLS - BARW - 34 ))
    (( room < 8 )) && room=8
    (( ${#name} > room )) && name="…${name: -$(( room - 1 ))}"
    (( UTF )) || name="${name: -$room}"
    printf '\r\e[K  %s%s%s %s %s%-11s%s %s %s%3d%%%s %s%d/%d%s  %s%s%s' \
        "$C_ACC" "${SPIN[SPIN_I % ${#SPIN[@]}]}" "$RST" "$(stage_tag)" "$BOLD" "Compile" "$RST" \
        "$(draw_bar "$pct" "$BARW")" "$C_WHT" "$pct" "$RST" "$DIM" "$DONE" "$TOTAL" "$RST" "$C_GRY" "$name" "$RST"
    SPIN_I=$(( SPIN_I + 1 ))
}

exec 3< <(make "${MAKE_ARGS[@]}" 2>&1; echo "@@RC:$?")
draw_live
while IFS= read -r -u3 line; do
    printf '%s\n' "$line" >> "$LOG_FILE"
    case "$line" in
        @@RC:*)   MAKE_RC="${line#@@RC:}"; break ;;
        mkdir\ *) ;;
        *warning:*) WARN_LIST+=("$line") ;;
        *error:*|*Error\ [0-9]*|*undefined\ reference*|*No\ such\ file*) ERR_LIST+=("$line") ;;
        *)
            tok="${line%% *}"
            case "$tok" in
                *gcc)     rest="${line#* -c }"; src="${rest%% *}"; CUR="${src##*/}"; DONE=$(( DONE + 1 )) ;;
                *ld)      CUR="link kernel.elf"; DONE=$(( DONE + 1 )) ;;
                *objcopy) CUR="objcopy Image"; DONE=$(( DONE + 1 )) ;;
                python3|python) CUR="mkbootimg"; DONE=$(( DONE + 1 )) ;;
            esac
            draw_live ;;
    esac
done
exec 3<&-
show_cursor
T_COMPILE=$(( $(now_ms) - t0 ))

if [[ "$MAKE_RC" != "0" ]]; then
    stage_fail "Compile" "failed after $DONE/$TOTAL steps (make exit $MAKE_RC)" "$T_COMPILE"
    section "Errors"
    if (( ${#ERR_LIST[@]} )); then
        for l in "${ERR_LIST[@]:0:12}"; do printf '  %s%s%s %s\n' "$C_ERR" "$S_BAR" "$RST" "$l"; done
    else
        tail -n 15 "$LOG_FILE" | while IFS= read -r l; do printf '  %s%s%s %s\n' "$C_ERR" "$S_BAR" "$RST" "$l"; done
    fi
    printf '\n  %sFull log:%s %s\n\n' "$C_GRY" "$RST" "$LOG_FILE"
    exit 1
fi
if (( ${#WARN_LIST[@]} )); then
    stage_warn "Compile" "${DONE} steps ${S_DOT} ${#WARN_LIST[@]} warning(s)" "$T_COMPILE"
else
    stage_ok "Compile" "${DONE} steps ${S_DOT} no warnings" "$T_COMPILE"
fi

# ── 4. Link & image ──────────────────────────────────────────────────────────
stage_next; t0=$(now_ms)
[[ -f "$IMG" ]] || { stage_fail "Image" "not created: $IMG"; exit 1; }
stage_ok "Image" "$IMG" "$(( $(now_ms) - t0 ))"

# ── 5. Verify ────────────────────────────────────────────────────────────────
stage_next; t0=$(now_ms)
problems=()
[[ "$(head -c 8 "$IMG")" == "ANDROID!" ]] || problems+=("no ANDROID! signature")
IMG_SIZE=$(stat -c %s "$IMG" 2>/dev/null || wc -c < "$IMG")
KIMG_SIZE=$(stat -c %s "$B/Image" 2>/dev/null || wc -c < "$B/Image" 2>/dev/null || echo 0)
(( IMG_SIZE % 4096 == 0 )) || problems+=("size not a multiple of 4096")
(( KIMG_SIZE > 0 )) || problems+=("empty kernel Image")
SHA="$(sha256sum "$IMG" 2>/dev/null | cut -d' ' -f1)"
if (( ${#problems[@]} )); then
    stage_fail "Verify" "${problems[*]}" "$(( $(now_ms) - t0 ))"; exit 1
fi
stage_ok "Verify" "ANDROID! header ${S_DOT} page-aligned ${S_DOT} sha256 ${SHA:0:12}…" "$(( $(now_ms) - t0 ))"

# ── Предупреждения ───────────────────────────────────────────────────────────
if (( ${#WARN_LIST[@]} )); then
    section "Warnings (${#WARN_LIST[@]})"
    for l in "${WARN_LIST[@]:0:8}"; do
        printf '  %s%s%s %s\n' "$C_WARN" "$S_BAR" "$RST" "${l:0:$(( COLS - 6 ))}"
    done
    (( ${#WARN_LIST[@]} > 8 )) && printf '  %s%s … and %d more in %s%s\n' "$DIM" "$S_BAR" $(( ${#WARN_LIST[@]} - 8 )) "$LOG_FILE" "$RST"
fi

# ── Память ядра (text / data / bss) ──────────────────────────────────────────
if command -v "${CROSS}size" >/dev/null 2>&1 && [[ -f "$B/kernel.elf" ]]; then
    read -r s_text s_data s_bss < <("${CROSS}size" "$B/kernel.elf" 2>/dev/null | awk 'NR==2{print $1, $2, $3}')
    if [[ "${s_text:-}" =~ ^[0-9]+$ ]]; then
        section "Kernel memory"
        tot=$(( s_text + s_data + s_bss )); (( tot == 0 )) && tot=1
        for row in "text:$s_text:$C_ACC" "data:$s_data:$C_BLUE" "bss:$s_bss:$C_VIO"; do
            IFS=: read -r nm val col <<< "$row"
            w=$(( 24 * val / tot )); (( val > 0 && w == 0 )) && w=1
            printf '  %s%s%s %s%-5s%s %s%s%s%s%s%s %s%10s%s\n' "$C_VIO" "$S_BAR" "$RST" "$C_GRY" "$nm" "$RST" \
                "$col" "$(repeat "$BLK_F" "$w")" "$RST" "$DIM" "$(repeat "$BLK_E" $(( 24 - w )))" "$RST" "$C_WHT" "$(hsize "$val")" "$RST"
        done
    fi
fi

# ── Самые тяжёлые объекты ────────────────────────────────────────────────────
mapfile -t objs < <(ls -S "$B"/*.o 2>/dev/null | head -5)
if (( ${#objs[@]} )); then
    section "Largest objects"
    maxsz=$(stat -c %s "${objs[0]}" 2>/dev/null || echo 1); (( maxsz == 0 )) && maxsz=1
    for o in "${objs[@]}"; do
        sz=$(stat -c %s "$o" 2>/dev/null || echo 0); nm="${o##*/}"
        w=$(( 24 * sz / maxsz )); (( sz > 0 && w == 0 )) && w=1
        printf '  %s%s%s %s%-22s%s %s%s%s%s%s%s %s%10s%s\n' "$C_VIO" "$S_BAR" "$RST" "$C_GRY" "${nm:0:22}" "$RST" \
            "$C_BLUE" "$(repeat "$BLK_F" "$w")" "$RST" "$DIM" "$(repeat "$BLK_E" $(( 24 - w )))" "$RST" "$C_WHT" "$(hsize "$sz")" "$RST"
    done
fi

# ─────────────────────────────────────────────────────────────────────────────
#  Телефон
# ─────────────────────────────────────────────────────────────────────────────
FASTBOOT="${FASTBOOT:-}"; ADB="${ADB:-}"
[[ -z $FASTBOOT ]] && { command -v fastboot >/dev/null 2>&1 && FASTBOOT=fastboot || FASTBOOT=fastboot.exe; }
[[ -z $ADB ]]      && { command -v adb      >/dev/null 2>&1 && ADB=adb           || ADB=adb.exe; }
PHONE_OK=1

if (( DO_BL || DO_BOOT || DO_LOG )); then section "Device"; fi

if (( DO_BL )); then
    stage_next; t0=$(now_ms)
    if ! command -v "$ADB" >/dev/null 2>&1; then stage_fail "Bootloader" "$ADB not found"; PHONE_OK=0
    elif "$ADB" reboot bootloader >/dev/null 2>&1; then stage_ok "Bootloader" "adb reboot bootloader" "$(( $(now_ms) - t0 ))"; sleep 4
    else stage_fail "Bootloader" "adb reboot bootloader failed (device authorized? USB debugging on?)"; PHONE_OK=0; fi
fi

if (( DO_BOOT && PHONE_OK )); then
    stage_next; t0=$(now_ms); STAGE_LABEL="Fastboot"
    if ! command -v "$FASTBOOT" >/dev/null 2>&1; then
        stage_fail "Fastboot" "$FASTBOOT not found (set FASTBOOT=...)"; PHONE_OK=0
    elif spin_until "waiting for device" 30 bash -c "\"$FASTBOOT\" devices | grep -q fastboot"; then
        stage_ok "Fastboot" "device found" "$(( $(now_ms) - t0 ))"
    else
        stage_fail "Fastboot" "no device in 30s (phone in bootloader? cable? driver?)"; PHONE_OK=0
    fi
    if (( PHONE_OK )); then
        stage_next; t0=$(now_ms)
        if "$FASTBOOT" boot "$IMG" >"$LOG_FILE.fastboot" 2>&1; then
            stage_ok "Boot" "fastboot boot $IMG" "$(( $(now_ms) - t0 ))"
        else
            stage_fail "Boot" "fastboot boot failed (see $LOG_FILE.fastboot)"; PHONE_OK=0
        fi
    fi
fi

if (( DO_LOG && PHONE_OK )); then
    stage_next; t0=$(now_ms); STAGE_LABEL="Ramoops"
    if ! command -v "$ADB" >/dev/null 2>&1; then stage_fail "Ramoops" "$ADB not found"
    else
        printf '  %s%s Reboot the phone into Android (long-press POWER), enable USB debugging.%s\n' "$DIM" "$S_ARR" "$RST"
        if spin_until "waiting for adb" 180 bash -c "\"$ADB\" get-state 2>/dev/null | grep -q device"; then
            if "$ADB" shell su -c "cat /sys/fs/pstore/console-ramoops-0" > "$RAMOOPS_OUT" 2>/dev/null && [[ -s "$RAMOOPS_OUT" ]]; then
                stage_ok "Ramoops" "$RAMOOPS_OUT ($(wc -l < "$RAMOOPS_OUT") lines)" "$(( $(now_ms) - t0 ))"
                section "Device boot log"
                grep -E '\[boot\] .*\[ (OK|FAIL) \]|\[touch\] (VERDICT|SUMMARY)|psci|SMCCC|PSCI|CurrentEL' "$RAMOOPS_OUT" | head -24 | while IFS= read -r l; do
                    case "$l" in
                        *"[ OK ]"*)   printf '  %s%s%s %s\n' "$C_OK" "$S_BAR" "$RST" "$l" ;;
                        *"[ FAIL ]"*) printf '  %s%s%s %s\n' "$C_ERR" "$S_BAR" "$RST" "$l" ;;
                        *VERDICT*)    printf '  %s%s%s %s%s%s\n' "$C_WARN" "$S_BAR" "$RST" "$BOLD" "$l" "$RST" ;;
                        *)            printf '  %s%s%s %s\n' "$C_GRY" "$S_BAR" "$RST" "$l" ;;
                    esac
                done
            else
                stage_fail "Ramoops" "could not read pstore (root needed: adb shell su -c ...)"
            fi
        else
            stage_fail "Ramoops" "adb device did not appear in 180s"
        fi
    fi
fi

# ─────────────────────────────────────────────────────────────────────────────
#  Итог
# ─────────────────────────────────────────────────────────────────────────────
T_TOTAL=$(( $(now_ms) - T_ALL ))
section "Result"
if (( ${#WARN_LIST[@]} )); then head="BUILD SUCCESSFUL (with ${#WARN_LIST[@]} warnings)"; hcol="$C_WARN"; hsym="$S_WARN"
else head="BUILD SUCCESSFUL"; hcol="$C_OK"; hsym="$S_OK"; fi
(( PHONE_OK )) || { head="BUILD OK, DEVICE STEP FAILED"; hcol="$C_WARN"; hsym="$S_WARN"; }
printf '  %s%s %s%s  %s%s in %s%s\n' "$hcol$BOLD" "$hsym" "$head" "$RST" "$DIM" "$S_DOT" "$(fmt_ms "$T_TOTAL")" "$RST"
# Время сборки: компиляция отдельно + разбивка по стадиям (без ожидания телефона)
T_BUILD=0
for st in Toolchain Clean Compile Image Verify; do T_BUILD=$(( T_BUILD + ${STAGE_MS[$st]:-0} )); done
kv "compile" "${C_WHT}$(fmt_ms "${STAGE_MS[Compile]:-0}")${RST}  ${DIM}(${DONE} steps, -j${JOBS})${RST}"
kv "build"   "${C_WHT}$(fmt_ms "$T_BUILD")${RST}  ${DIM}(toolchain $(fmt_ms "${STAGE_MS[Toolchain]:-0}") ${S_DOT} clean $(fmt_ms "${STAGE_MS[Clean]:-0}") ${S_DOT} image $(fmt_ms "${STAGE_MS[Image]:-0}") ${S_DOT} verify $(fmt_ms "${STAGE_MS[Verify]:-0}"))${RST}"
(( DO_BL || DO_BOOT || DO_LOG )) && kv "total" "$(fmt_ms "$T_TOTAL")  ${DIM}(with device steps)${RST}"
printf 'build_time compile_ms=%s build_ms=%s total_ms=%s jobs=%s\n' "${STAGE_MS[Compile]:-0}" "$T_BUILD" "$T_TOTAL" "$JOBS" >> "$LOG_FILE"
kv "image"  "$C_WHT$IMG$RST"
kv "size"   "$(hsize "$IMG_SIZE")  ${DIM}(kernel $(hsize "$KIMG_SIZE"))${RST}"
kv "sha256" "${SHA:0:32}"
kv ""       "${SHA:32}"
kv "log"    "$LOG_FILE"

if (( ! DO_BOOT )); then
    printf '\n  %sNext%s  %s./build.sh --bootloader --boot --log%s\n' "$C_GRY" "$RST" "$C_ACC$BOLD" "$RST"
    printf '        %sor manually:%s %sfastboot boot %s%s\n' "$DIM" "$RST" "$C_GRY" "$IMG" "$RST"
fi
printf '\n'