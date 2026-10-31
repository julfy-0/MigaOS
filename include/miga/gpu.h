/* Публичный интерфейс видеоадаптеров для ядра (kshell и т.п.): что найдено на PCI.
 * Реализация — drivers/video/gpu/. Вывод изображения по-прежнему идёт через UEFI-фреймбуфер (fbcon). */
#pragma once
#include <stdint.h>

#define GPU_MAX      4
#define GPU_BARS     6

struct gpu_bar {
    uint64_t base, size;               /* size == 0: BAR не реализован или это порт ввода-вывода */
    uint8_t  prefetch, is64;
};

struct gpu_info {
    uint8_t  bus, dev, fn, rev;
    uint16_t vendor, device;
    uint8_t  integrated;               /* 1 = встроенное в CPU/чипсет, 0 = дискретное */
    uint8_t  boot_fb;                  /* загрузочный UEFI-фреймбуфер лежит в BAR этой карты */
    uint8_t  link_speed, link_width;   /* текущая PCIe-ссылка (speed: 1=2.5 2=5 3=8 4=16 5=32 GT/s) */
    uint8_t  link_max_speed, link_max_width;
    uint32_t chip_id;                  /* NVIDIA: PMC_BOOT_0, иначе 0 */
    uint64_t vram_aperture;            /* крупнейший prefetchable BAR (окно в VRAM) */
    struct gpu_bar bar[GPU_BARS];
    const char *vendor_name;
    const char *family;                /* поколение GPU, определяется эвристически */
};

unsigned    gpu_count(void);
int         gpu_get(unsigned idx, struct gpu_info *out);
const char *gpu_link_gts(unsigned speed);
