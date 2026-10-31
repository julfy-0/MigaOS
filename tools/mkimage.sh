#!/bin/sh
# Собирает FAT32-образ (без таблицы разделов) из дерева ESP. OVMF/QEMU грузятся с него как со "съёмного" носителя.
# Использование: tools/mkimage.sh build/esp build/miga.img
set -e
ESP=${1:-build/esp}; IMG=${2:-build/miga.img}
command -v mformat >/dev/null || { echo "нужен mtools (apt install mtools)"; exit 1; }
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
mformat -i "$IMG" -F ::
mmd   -i "$IMG" ::/EFI ::/EFI/BOOT ::/EFI/MIGA
mcopy -i "$IMG" "$ESP/EFI/BOOT/BOOTX64.EFI" ::/EFI/BOOT/
mcopy -i "$IMG" "$ESP/EFI/MIGA/kernel.elf"  ::/EFI/MIGA/
echo "Образ: $IMG (qemu ... -drive format=raw,file=$IMG)"
