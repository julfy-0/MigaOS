#pragma once
/* Формат initrd MIGARD01 (без сжатия, всё little-endian):
 *   struct initrd_hdr                16 байт
 *   struct initrd_ent[count]         по 64 байта
 *   данные файлов, каждый с выравниванием 16
 * Образ собирает tools/mkinitrd.py. */
#include <stdint.h>

#define INITRD_MAGIC    "MIGARD01"
#define INITRD_NAME_MAX 40
#define INITRD_MAX_ENT  128

struct initrd_hdr {
    char     magic[8];
    uint32_t count;
    uint32_t total;      /* размер образа целиком */
};

struct initrd_ent {
    char     name[INITRD_NAME_MAX];  /* NUL-терминированное, без каталогов */
    uint64_t off;                    /* от начала образа */
    uint64_t size;
    uint32_t mode;                   /* зарезервировано */
    uint32_t reserved;
};

/* 0 = ок, <0 = образ повреждён. base/size приходят из boot_info. */
int initrd_init(const void *base, uint64_t size);
/* 0 = найден; data указывает внутрь образа (read-only). */
int initrd_find(const char *name, const void **data, uint64_t *size);
