/* Контракт между MigaBoot (UEFI-загрузчик) и ядром.
 * Загрузчик заполняет структуру и передаёт указатель на неё в RDI (SysV ABI). */
#pragma once
#include <stdint.h>

#define MIGA_BOOT_MAGIC 0x4D49474142303031ULL /* "MIGAB001" */

enum mem_type {
    MEM_USABLE = 1,    /* свободная RAM */
    MEM_LOADER,        /* ядро, BootInfo, карта памяти: можно освободить позже */
    MEM_RESERVED,      /* firmware, runtime services и т.п. */
    MEM_ACPI_RECLAIM,
    MEM_ACPI_NVS,
    MEM_MMIO,
    MEM_BAD,
};

struct mem_region {
    uint64_t base;
    uint64_t length;
    uint32_t type; /* enum mem_type */
    uint32_t _pad;
};

enum fb_format {
    FB_NONE = 0,
    FB_RGBX = 1, /* байты в памяти: R, G, B, x */
    FB_BGRX = 2, /* байты в памяти: B, G, R, x */
};

struct boot_fb {
    uint64_t base;   /* физический адрес, 0 если фреймбуфера нет */
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;  /* байт в строке */
    uint32_t format; /* enum fb_format, всегда 32 bpp */
};

struct boot_info {
    uint64_t magic;
    uint32_t version;
    uint32_t _pad;
    struct boot_fb fb;
    uint64_t rsdp;                 /* физический адрес ACPI RSDP или 0 */
    uint64_t mem_count;
    struct mem_region *mem;
    uint64_t kernel_base;
    uint64_t kernel_size;
    uint64_t smbios;               /* физический адрес SMBIOS entry point (предпочтительно 3.x) или 0 */
    uint64_t edid;                 /* физический адрес копии EDID монитора (MEM_LOADER) или 0 */
    uint32_t edid_size;            /* размер в байтах (128..512) */
    uint32_t _pad2;
};
