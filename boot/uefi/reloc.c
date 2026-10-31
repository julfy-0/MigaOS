/* Самоперемещение образа: применяем R_X86_64_RELATIVE из .rela. */
#include <stdint.h>

typedef struct { int64_t d_tag; uint64_t d_val; } Elf64_Dyn;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;

#define DT_NULL 0
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define R_X86_64_RELATIVE 8

int _relocate(uint64_t base, Elf64_Dyn *dyn)
{
    Elf64_Rela *rela = 0;
    uint64_t relsz = 0, relent = sizeof(Elf64_Rela);

    for (; dyn->d_tag != DT_NULL; dyn++) {
        switch (dyn->d_tag) {
        case DT_RELA:    rela = (Elf64_Rela *)(base + dyn->d_val); break;
        case DT_RELASZ:  relsz = dyn->d_val; break;
        case DT_RELAENT: relent = dyn->d_val; break;
        }
    }
    if (!rela) return 0;
    for (; relsz >= relent; relsz -= relent, rela = (Elf64_Rela *)((char *)rela + relent)) {
        if ((rela->r_info & 0xffffffffu) != R_X86_64_RELATIVE) return 1;
        *(uint64_t *)(base + rela->r_offset) = base + rela->r_addend;
    }
    return 0;
}
