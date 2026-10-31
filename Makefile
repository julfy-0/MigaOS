# MigaOS: сборка загрузчика (MigaBoot), ядра и драйверов.
# Нужны: gcc, ld, objcopy, ar (binutils). Для запуска: qemu-system-x86_64 и OVMF.

BUILD := build
ESP   := $(BUILD)/esp

CC      ?= gcc
LD      ?= ld
AR      ?= ar
OBJCOPY ?= objcopy

COMMON_WARN := -Wall -Wextra -Wno-unused-parameter
DEPFLAGS    := -MMD -MP

# ---------------------------------------------------------------- MigaBoot (UEFI)
BOOT_CFLAGS := -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fpic -fshort-wchar \
               -fno-stack-protector -fno-asynchronous-unwind-tables -mno-red-zone \
               -mgeneral-regs-only -maccumulate-outgoing-args $(DEPFLAGS) -I include -I boot/uefi
BOOT_SRC_C := $(wildcard boot/uefi/*.c)
BOOT_C_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(BOOT_SRC_C))
BOOT_OBJS  := $(BOOT_C_OBJS) $(BUILD)/boot/uefi/crt0.o
BOOT_SO    := $(BUILD)/boot/migaboot.so
BOOT_EFI   := $(BUILD)/boot/BOOTX64.EFI

# ---------------------------------------------------------------- ядро и драйверы
KFLAGS := -std=gnu11 -O2 -g -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mgeneral-regs-only -fno-tree-loop-distribute-patterns \
          -fno-asynchronous-unwind-tables $(COMMON_WARN) $(DEPFLAGS)

# ядро видит include/ и kernel/include/
KERNEL_CFLAGS  := $(KFLAGS) -I include -I kernel/include
# драйверы видят ТОЛЬКО include/miga/: это граница между ядром и драйверами
DRIVER_CFLAGS  := $(KFLAGS) -I include

KERNEL_SRC_C := $(shell find kernel -name '*.c')
KERNEL_SRC_S := $(shell find kernel -name '*.S')
KERNEL_OBJS  := $(patsubst %.c,$(BUILD)/%.o,$(KERNEL_SRC_C)) $(patsubst %.S,$(BUILD)/%.o,$(KERNEL_SRC_S))

DRIVER_SRC_C := $(shell find drivers -name '*.c')
DRIVER_OBJS  := $(patsubst %.c,$(BUILD)/%.o,$(DRIVER_SRC_C))
DRIVER_LIB   := $(BUILD)/drivers/libdrivers.a

KERNEL_ELF := $(BUILD)/kernel.elf

# ---------------------------------------------------------------- цели
.PHONY: all boot kernel image run disk clean check
all: image

boot: $(BOOT_EFI)
kernel: $(KERNEL_ELF)

# --- загрузчик
$(BOOT_C_OBJS): $(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

$(BUILD)/boot/uefi/crt0.o: boot/uefi/crt0.S
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

$(BOOT_SO): $(BOOT_OBJS) boot/uefi/link.lds
	$(LD) -nostdlib -znocombreloc -shared -Bsymbolic -z noexecstack -T boot/uefi/link.lds $(BOOT_OBJS) -o $@

$(BOOT_EFI): $(BOOT_SO)
	$(OBJCOPY) -j .text -j .data -j .dynamic -j .dynsym -j .rela -j .reloc \
	           -O pei-x86-64 --subsystem efi-app $< $@
	@python3 tools/check_pe.py $@

# --- ядро
$(patsubst %.c,$(BUILD)/%.o,$(KERNEL_SRC_C)): $(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(patsubst %.S,$(BUILD)/%.o,$(KERNEL_SRC_S)): $(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

# --- драйверы (отдельная статическая библиотека)
$(DRIVER_OBJS): $(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(DRIVER_CFLAGS) -c $< -o $@

$(DRIVER_LIB): $(DRIVER_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

# --whole-archive: драйверы никем не вызываются напрямую, только через секцию .miga_drivers
$(KERNEL_ELF): $(KERNEL_OBJS) $(DRIVER_LIB) kernel/kernel.ld
	$(LD) -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T kernel/kernel.ld \
	      -o $@ $(KERNEL_OBJS) --whole-archive $(DRIVER_LIB) --no-whole-archive

# --- ESP-дерево
image: $(BOOT_EFI) $(KERNEL_ELF)
	@mkdir -p $(ESP)/EFI/BOOT $(ESP)/EFI/MIGA
	cp $(BOOT_EFI) $(ESP)/EFI/BOOT/BOOTX64.EFI
	cp $(KERNEL_ELF) $(ESP)/EFI/MIGA/kernel.elf
	@echo "ESP готова: $(ESP)"

disk: image
	tools/mkimage.sh $(ESP) $(BUILD)/miga.img

# --- запуск в QEMU
OVMF ?= $(firstword $(wildcard /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
                               /usr/share/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd))
ifneq (,$(findstring CODE,$(OVMF)))
  QEMU_FW := -drive if=pflash,format=raw,readonly=on,file=$(OVMF)
else
  QEMU_FW := -bios $(OVMF)
endif

run: image
	@test -n "$(OVMF)" || (echo "OVMF не найден: поставьте пакет ovmf или задайте OVMF=/path/OVMF.fd"; exit 1)
	qemu-system-x86_64 -machine q35 -m 256M $(QEMU_FW) \
	    -drive format=raw,file=fat:rw:$(ESP) -serial stdio -no-reboot

check: all
	@echo "--- kernel segments"; readelf -lW $(KERNEL_ELF) | sed -n '/Program Headers/,/Section to Segment/p'
	@echo "--- drivers registered"; nm $(KERNEL_ELF) | grep -E '__drivers_(start|end)'
	@echo "--- boundary check: drivers must not reference kernel-private symbols"
	@nm -u $(DRIVER_LIB) | grep -v '^$$' | sort -u

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

clean:
	rm -rf $(BUILD)
