#!/usr/bin/env bash
# ╔══════════════════════════════════════════════════════════════╗
# ║  MigaOS build script — ./build.sh help                       ║
# ╚══════════════════════════════════════════════════════════════╝
set -Eeuo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

BUILD=build
ESP=$BUILD/esp
LOG=$BUILD/build.log
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}
MEM=${MEM:-256M}
VERBOSE=0 USB=1 HEADLESS=0 GDB=0 OVMF_PATH=${OVMF:-}
CMD=build

# ---------------------------------------------------------------- оформление
if [[ -t 1 && -z "${NO_COLOR:-}" ]]; then
    TTY=1
    RST=$'\e[0m' BOLD=$'\e[1m' DIM=$'\e[2m'
    RED=$'\e[31m' GRN=$'\e[32m' YEL=$'\e[33m' BLU=$'\e[34m' MAG=$'\e[35m' CYN=$'\e[36m'
else
    TTY=0 RST= BOLD= DIM= RED= GRN= YEL= BLU= MAG= CYN=
fi

banner() {
    printf '\n%s' "$MAG$BOLD"
    cat <<'ART'
   __  __ _             ____   _____
  |  \/  (_) __ _  __ _/ __ \ / ____|
  | |\/| | |/ _` |/ _` | |  | | (___
  | |  | | | (_| | (_| | |__| |\___ \
  |_|  |_|_|\__, |\__,_|\____/_____) |
            |___/                 |_/
ART
    printf '%s%s  x86_64 · UEFI · MigaBoot%s\n\n' "$RST" "$DIM" "$RST"
}

info() { printf '  %s•%s %s\n' "$BLU" "$RST" "$*"; }
warn() { printf '  %s!%s %s\n' "$YEL" "$RST" "$*" >&2; }
die()  { printf '  %s✗ %s%s\n' "$RED" "$*" "$RST" >&2; exit 1; }

now_ms() { echo $(( $(date +%s%N) / 1000000 )); }
fmt_ms() { printf '%d.%01ds' $(( $1 / 1000 )) $(( $1 % 1000 / 100 )); }
human()  { awk -v b="$1" 'BEGIN { split("B KB MB GB", u, " "); i = 1; while (b >= 1024 && i < 4) { b /= 1024; i++ } printf (i == 1 ? "%d %s" : "%.1f %s"), b, u[i] }'; }

# step "Название" команда… — выполняет с логом и спиннером, при ошибке показывает хвост лога
step() {
    local title=$1; shift
    local t0 rc=0 pid i=0 frames='|/-\'
    t0=$(now_ms)
    mkdir -p "$BUILD"
    printf '\n### %s\n$ %s\n' "$title" "$*" >>"$LOG"

    if (( VERBOSE )); then
        printf '  %s▶ %s%s\n' "$CYN" "$title" "$RST"
        "$@" 2>&1 | tee -a "$LOG" || rc=$?
    else
        "$@" >>"$LOG" 2>&1 &
        pid=$!
        if (( TTY )); then
            while kill -0 "$pid" 2>/dev/null; do
                printf '\r  %s%s%s %s' "$CYN" "${frames:i++%4:1}" "$RST" "$title"
                sleep 0.08
            done
        fi
        wait "$pid" || rc=$?
    fi

    local dt; dt=$(fmt_ms $(( $(now_ms) - t0 )))
    if (( TTY && ! VERBOSE )); then printf '\r\e[K'; fi
    if (( rc == 0 )); then
        printf '  %s✓%s %-34s %s%s%s\n' "$GRN" "$RST" "$title" "$DIM" "$dt" "$RST"
    else
        printf '  %s✗%s %-34s %s%s%s\n' "$RED" "$RST" "$title" "$DIM" "$dt" "$RST"
        printf '\n%s── последние строки лога (%s) ──%s\n' "$RED" "$LOG" "$RST"
        tail -n 40 "$LOG"
        exit "$rc"
    fi
}

# ---------------------------------------------------------------- проверки
check_deps() {
    local missing=() t
    for t in gcc ld ar objcopy make python3 "$@"; do
        command -v "$t" >/dev/null 2>&1 || missing+=("$t")
    done
    if (( ${#missing[@]} )); then
        die "не хватает инструментов: ${missing[*]}  (Debian/Ubuntu: sudo apt install build-essential qemu-system-x86 ovmf mtools)"
    fi
}

find_ovmf() {
    local f
    for f in "$OVMF_PATH" \
             /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/OVMF/OVMF.fd \
             /usr/share/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd \
             /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/edk2-ovmf/x64/OVMF_CODE.fd; do
        if [[ -n $f && -f $f ]]; then echo "$f"; return 0; fi
    done
    return 1
}

# ---------------------------------------------------------------- сборка
do_build() {
    check_deps
    mkdir -p "$BUILD"; : >"$LOG"
    local t0; t0=$(now_ms)
    local MK=(make -j"$JOBS")

    info "компилятор: ${BOLD}$(${CC:-gcc} --version | head -n1)${RST}, потоков: ${BOLD}$JOBS${RST}"
    echo
    step "MigaBoot (UEFI loader)" "${MK[@]}" boot
    step "Kernel + drivers" "${MK[@]}" kernel
    step "ESP tree" "${MK[@]}" image

    # граница ядро <-> драйверы: сколько функций ядра реально используют драйверы
    local api
    api=$(nm -u "$BUILD/drivers/libdrivers.a" 2>/dev/null | awk '$1 == "U" { print $2 }' | sort -u | wc -l)
    printf '  %s✓%s %-34s %s%s функций%s\n' "$GRN" "$RST" "Driver API (kernel boundary)" "$DIM" "$api" "$RST"

    print_summary "$(fmt_ms $(( $(now_ms) - t0 )))"
}

list_drivers() {
    grep -rhE '^MIGA_DRIVER\(' drivers 2>/dev/null |
    sed -E 's/^MIGA_DRIVER\([^,]*, *"([^"]+)", *DRV_VER\(([0-9]+), *([0-9]+)\), *DRV_([A-Z]+).*/\4 \1 \2 \3/' |
    sort | while read -r stage name maj min; do
        printf '    %s%-8s%s %-12s v%02d.%02d\n' "$DIM" "$stage" "$RST" "$name" "$((10#$maj))" "$((10#$min))"
    done
}

print_summary() {
    printf '\n  %s──────────────── готово за %s ────────────────%s\n\n' "$BOLD" "$1" "$RST"
    printf '    %-34s %s\n' "$ESP/EFI/BOOT/BOOTX64.EFI" "$(human "$(stat -c %s "$ESP/EFI/BOOT/BOOTX64.EFI")")"
    printf '    %-34s %s\n' "$ESP/EFI/MIGA/kernel.elf"  "$(human "$(stat -c %s "$ESP/EFI/MIGA/kernel.elf")")"
    printf '\n  %sдрайверы:%s\n' "$BOLD" "$RST"
    list_drivers
    printf '\n  запуск: %s./build.sh run%s    образ: %s./build.sh disk%s    справка: %s./build.sh help%s\n\n' \
        "$CYN" "$RST" "$CYN" "$RST" "$CYN" "$RST"
}

do_disk() {
    check_deps mformat
    do_build
    step "Disk image (FAT32)" make disk
    info "образ: ${BOLD}$BUILD/miga.img${RST} ($(human "$(stat -c %s "$BUILD/miga.img")"))"
}

do_run() {
    check_deps qemu-system-x86_64
    do_build
    local fw
    fw=$(find_ovmf) || die "OVMF не найден: sudo apt install ovmf  или  ./build.sh run --ovmf /path/OVMF.fd"

    local args=(-machine q35 -m "$MEM" -drive "format=raw,file=fat:rw:$ESP" -serial stdio -no-reboot)
    if [[ $fw == *CODE* ]]; then
        args+=(-drive "if=pflash,format=raw,readonly=on,file=$fw")
    else
        args+=(-bios "$fw")
    fi
    if (( USB )); then args+=(-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0); fi
    if (( HEADLESS )); then args+=(-display none); fi
    if (( GDB )); then
        args+=(-s -S)
        warn "ждём отладчик: gdb build/kernel.elf -ex 'target remote :1234'"
    fi

    info "QEMU: ${DIM}${args[*]}${RST}"
    echo
    exec qemu-system-x86_64 "${args[@]}"
}

do_check() {
    do_build
    echo
    make check 2>&1 | sed -n '/--- kernel segments/,$p'
}

do_clean() {
    rm -rf "$BUILD"
    printf '  %s✓%s build/ удалён\n' "$GRN" "$RST"
}

usage() {
    cat <<USAGE
${BOLD}Использование:${RST} ./build.sh [команда] [опции]

${BOLD}Команды:${RST}
  build       собрать загрузчик, ядро и драйверы (по умолчанию)
  run         собрать и запустить в QEMU (с USB-клавиатурой через xHCI)
  disk        собрать и упаковать FAT32-образ build/miga.img (нужен mtools)
  check       собрать и показать сегменты ядра / таблицу драйверов / API драйверов
  rebuild     clean + build
  clean       удалить build/
  help        эта справка

${BOLD}Опции:${RST}
  -v, --verbose      показывать вывод компилятора
  -j N               число параллельных задач (по умолчанию: $JOBS)
  --no-usb           run: не подключать xHCI и USB-клавиатуру
  --headless         run: без окна QEMU (логи в терминал через serial)
  --gdb              run: ждать отладчик на :1234
  --mem SIZE         run: память ВМ (по умолчанию: $MEM)
  --ovmf PATH        run: путь к OVMF.fd / OVMF_CODE.fd

Переменные окружения: CC, JOBS, MEM, OVMF, NO_COLOR
USAGE
}

# ---------------------------------------------------------------- аргументы
while (( $# )); do
    case $1 in
        build|run|disk|check|clean|rebuild) CMD=$1 ;;
        help|-h|--help) usage; exit 0 ;;
        -v|--verbose)   VERBOSE=1 ;;
        -j)             shift; JOBS=${1:?-j требует число} ;;
        -j[0-9]*)       JOBS=${1#-j} ;;
        --no-usb)       USB=0 ;;
        --headless)     HEADLESS=1 ;;
        --gdb)          GDB=1 ;;
        --mem)          shift; MEM=${1:?--mem требует размер} ;;
        --ovmf)         shift; OVMF_PATH=${1:?--ovmf требует путь} ;;
        *) usage >&2; die "неизвестный аргумент: $1" ;;
    esac
    shift
done

banner
case $CMD in
    build)   do_build ;;
    run)     do_run ;;
    disk)    do_disk ;;
    check)   do_check ;;
    clean)   do_clean ;;
    rebuild) do_clean; do_build ;;
esac
