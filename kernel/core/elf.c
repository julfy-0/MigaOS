#include <kernel/elf.h>

#include <kernel/kernel.h>
#include <kernel/mm.h>

/* ---- адаптер к памяти ядра (C2): struct aspace * здесь — это CR3 (vm_space_create()), приведённый к указателю ---- */
static inline uint64_t pmm_alloc_page(void) { return pmm_alloc(); }
static inline void *phys_to_virt(uint64_t pa) { return P2V(pa); }
static inline int vmm_map_user(struct aspace *as, uint64_t va, uint64_t pa, unsigned flags)
{
    unsigned vf = VM_USER | ((flags & ELF_MAP_W) ? VM_WRITE : 0) | ((flags & ELF_MAP_X) ? 0 : VM_NOEXEC);
    return vm_map((uint64_t)(uintptr_t)as, va, pa, vf);
}
/* -------------------------------------------------------------------------- */

#define PAGE      4096ULL
#define USER_LO   0x400000ULL                 /* ниже: нулевая страница и мусор */
#define USER_HI   0x0000800000000000ULL       /* канонический верх нижней половины */
#define MAX_PH    16
#define MAX_PAGES 4096ULL                     /* 16 МиБ на образ */

#define PT_LOAD 1
#define PF_X 1u
#define PF_W 2u

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Ehdr;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Phdr;

static void cp(void *d, const void *s, uint64_t n)
{ uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; }

static void zero(void *d, uint64_t n)
{ uint8_t *a = d; while (n--) *a++ = 0; }

int elf_load(const void *img, uint64_t size, struct aspace *as, uint64_t *entry_out)
{
    const uint8_t *b = img;
    Ehdr eh;
    Phdr ph;
    struct { uint64_t lo, hi; } rng[MAX_PH];
    unsigned nr = 0, entry_ok = 0;
    uint64_t pages = 0, tbl;

    if (!img || !as || !entry_out || size < sizeof eh) return ELF_EINVAL;
    cp(&eh, b, sizeof eh);

    if (eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' ||
        eh.e_ident[2] != 'L'  || eh.e_ident[3] != 'F') return ELF_EINVAL;
    if (eh.e_ident[4] != 2 /*64-bit*/ || eh.e_ident[5] != 1 /*LE*/ ||
        eh.e_ident[6] != 1) return ELF_EINVAL;
    if (eh.e_type != 2 /*ET_EXEC*/ || eh.e_machine != 62 /*x86-64*/ ||
        eh.e_version != 1) return ELF_EINVAL;
    if (eh.e_phentsize != sizeof(Phdr) || eh.e_phnum == 0 || eh.e_phnum > MAX_PH)
        return ELF_EINVAL;
    tbl = (uint64_t)eh.e_phnum * sizeof(Phdr);
    if (eh.e_phoff > size || tbl > size - eh.e_phoff) return ELF_EBOUNDS;

    /* проход 1: проверить всё, ничего не выделяя */
    for (unsigned i = 0; i < eh.e_phnum; i++) {
        uint64_t lo, hi;
        cp(&ph, b + eh.e_phoff + (uint64_t)i * sizeof ph, sizeof ph);
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        if (ph.p_filesz > ph.p_memsz) return ELF_EINVAL;
        if (ph.p_offset > size || ph.p_filesz > size - ph.p_offset) return ELF_EBOUNDS;
        if (ph.p_vaddr < USER_LO || ph.p_vaddr >= USER_HI ||
            ph.p_memsz > USER_HI - ph.p_vaddr) return ELF_EBOUNDS;
        if ((ph.p_flags & PF_W) && (ph.p_flags & PF_X)) return ELF_EPERM;   /* W^X */

        lo = ph.p_vaddr & ~(PAGE - 1);
        hi = (ph.p_vaddr + ph.p_memsz + PAGE - 1) & ~(PAGE - 1);
        for (unsigned k = 0; k < nr; k++)
            if (lo < rng[k].hi && rng[k].lo < hi) return ELF_EINVAL;        /* пересечение */
        pages += (hi - lo) / PAGE;
        if (pages > MAX_PAGES) return ELF_EBOUNDS;
        rng[nr].lo = lo; rng[nr].hi = hi; nr++;

        if ((ph.p_flags & PF_X) && eh.e_entry >= ph.p_vaddr &&
            eh.e_entry < ph.p_vaddr + ph.p_memsz) entry_ok = 1;
    }
    if (nr == 0 || !entry_ok) return ELF_EINVAL;   /* точка входа обязана быть в RX-сегменте */

    /* проход 2: выделить, скопировать, отобразить */
    for (unsigned i = 0; i < eh.e_phnum; i++) {
        uint64_t va, lo, hi, fend;
        unsigned fl;
        cp(&ph, b + eh.e_phoff + (uint64_t)i * sizeof ph, sizeof ph);
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        fl = ((ph.p_flags & PF_W) ? ELF_MAP_W : 0) | ((ph.p_flags & PF_X) ? ELF_MAP_X : 0);
        lo = ph.p_vaddr & ~(PAGE - 1);
        hi = (ph.p_vaddr + ph.p_memsz + PAGE - 1) & ~(PAGE - 1);
        fend = ph.p_vaddr + ph.p_filesz;

        for (va = lo; va < hi; va += PAGE) {
            uint64_t pa = pmm_alloc_page();
            uint8_t *dst;
            uint64_t s, e;
            if (!pa) return ELF_ENOMEM;
            dst = phys_to_virt(pa);
            zero(dst, PAGE);                               /* .bss и хвосты страниц */
            s = va > ph.p_vaddr ? va : ph.p_vaddr;
            e = va + PAGE < fend ? va + PAGE : fend;
            if (s < e) cp(dst + (s - va), b + ph.p_offset + (s - ph.p_vaddr), e - s);
            if (vmm_map_user(as, va, pa, fl) != 0) return ELF_ENOMEM;
        }
    }
    *entry_out = eh.e_entry;
    return ELF_OK;
}
