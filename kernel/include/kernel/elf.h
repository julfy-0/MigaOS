#pragma once
#include <stdint.h>

struct aspace;                       /* адресное пространство процесса (C2/C3) */

#define ELF_MAP_W 1u                 /* R + USER подразумеваются всегда */
#define ELF_MAP_X 2u

enum {
    ELF_OK      =  0,
    ELF_EINVAL  = -1,                /* не тот формат / неподдерживаемый ELF */
    ELF_EBOUNDS = -2,                /* выход за границы файла или за user-диапазон */
    ELF_EPERM   = -3,                /* W+X, отказ */
    ELF_ENOMEM  = -4,
};

/* Разбирает ELF64 (ET_EXEC, x86_64) из img[0..size) и отображает все PT_LOAD
 * в as. Сначала всё проверяется, потом выделяется память. При ошибке после
 * начала отображения вызывающий обязан уничтожить as целиком. */
int elf_load(const void *img, uint64_t size, struct aspace *as, uint64_t *entry);
