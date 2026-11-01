/* Собственные таблицы страниц ядра.
 * UEFI оставляет свои таблицы в памяти типа BootServicesData, которую мы отдаём PMM,
 * поэтому до pmm_init() переключаемся на статические таблицы в .bss:
 * identity map [0, top) страницами по 2 МиБ (до 64 ГиБ).
 *  - ОЗУ: write-back
 *  - фреймбуфер: write-combining (PAT[1]=WC; бит PWT) — на порядок быстрее write-through при выводе кадров 240 Гц
 *  - всё остальное (MMIO): uncached */
#include <kernel/kernel.h>
#include <kernel/arch.h>
#include <kernel/mm.h>
#include <miga/mem.h>
#include <miga/string.h>

#define GIB (1ULL << 30)
#define HUGE (2ULL << 20)
#define MAX_GIB 64
#define P_PRESENT 1ULL
#define P_RW 2ULL
#define P_PWT 8ULL
#define P_PCD 0x10ULL
#define P_HUGE 0x80ULL

static uint64_t pml4[512] __attribute__((aligned(4096)));
static uint64_t pdpt[512] __attribute__((aligned(4096)));
static uint64_t pd[MAX_GIB][512] __attribute__((aligned(4096)));

static int overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    return a < b + blen && b < a + alen;
}

static uint64_t cache_bits(const struct boot_info *bi, uint64_t pa)
{
    if (bi->fb.base && overlaps(pa, HUGE, bi->fb.base, bi->fb.size)) return P_PWT;
    for (uint64_t i = 0; i < bi->mem_count; i++) {
        uint32_t t = bi->mem[i].type;
        if ((t == MEM_USABLE || t == MEM_LOADER || t == MEM_ACPI_RECLAIM || t == MEM_ACPI_NVS) &&
            overlaps(pa, HUGE, bi->mem[i].base, bi->mem[i].length))
            return 0;
    }
    return P_PCD | P_PWT;
}

/* PAT: запись 1 (PWT=1,PCD=0) по умолчанию WT; переводим в WC (0x01). Остальные записи не трогаем. */
static void pat_setup_wc(void)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    if (!(d & (1u << 16))) return;                                  /* PAT не поддерживается */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277));
    lo = (lo & ~0xFF00u) | 0x0100u;
    __asm__ volatile("wbinvd; wrmsr" ::"a"(lo), "d"(hi), "c"(0x277) : "memory");
}

/* NX: без EFER.NXE бит 63 в записях таблиц зарезервирован (#PF), поэтому включаем его до первой записи с VM_NOEXEC.
 * Таблицы identity map его не используют, так что сами по себе они не меняются. */
static int nx_on;

static void nx_setup(void)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u));
    if (a < 0x80000001u) return;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001u));
    if (!(d & (1u << 20))) return;                                  /* NX не поддерживается */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
    lo |= 1u << 11;                                                 /* EFER.NXE */
    __asm__ volatile("wrmsr" ::"a"(lo), "d"(hi), "c"(0xC0000080u) : "memory");
    nx_on = 1;
}

int vm_nx_enabled(void) { return nx_on; }
uint64_t vm_kernel_cr3(void) { return (uint64_t)pml4; }

void paging_init(const struct boot_info *bi)
{
    pat_setup_wc();
    nx_setup();
    uint64_t top = 4 * GIB; /* первые 4 ГиБ мапим всегда: там APIC/PCI-окна */
    for (uint64_t i = 0; i < bi->mem_count; i++) {
        if (bi->mem[i].type == MEM_MMIO || bi->mem[i].type == MEM_BAD) continue;
        uint64_t e = bi->mem[i].base + bi->mem[i].length;
        if (e > top) top = e;
    }
    if (bi->fb.base && bi->fb.base + bi->fb.size > top) top = bi->fb.base + bi->fb.size;

    uint64_t gibs = (top + GIB - 1) / GIB;
    if (gibs > MAX_GIB) { pr_warn("paging: memory above %d GiB is not mapped\n", MAX_GIB); gibs = MAX_GIB; }

    for (uint64_t g = 0; g < gibs; g++) {
        pdpt[g] = (uint64_t)pd[g] | P_PRESENT | P_RW;
        for (uint64_t i = 0; i < 512; i++) {
            uint64_t pa = g * GIB + i * HUGE;
            pd[g][i] = pa | P_PRESENT | P_RW | P_HUGE | cache_bits(bi, pa);
        }
    }
    pml4[0] = (uint64_t)pdpt | P_PRESENT | P_RW;
    __asm__ volatile("mov %0, %%cr3" ::"r"((uint64_t)pml4) : "memory");
}

/* MMIO-окно выше уже отображённой области (например, 64-битный BAR контроллера USB).
 * Для недостающих гигабайт заводим таблицу PD из страниц 2 МиБ, uncached. */
void *mmio_map(uint64_t phys, size_t size)
{
    uint64_t end = phys + (size ? size : 1);
    if (end > VM_USER_BASE || end < phys) return 0;          /* окно выше 64 ТиБ принадлежит пользователю (PML4[128..]) */
    for (uint64_t g = phys / GIB; g <= (end - 1) / GIB; g++) {
        uint64_t *pdp;
        if (g < 512) pdp = pdpt;
        else {                                  /* BAR выше 512 ГиБ (OVMF кладёт 64-битные окна PCI на 768 ГиБ): свой PDPT */
            uint64_t pe = g >> 9;
            if (!(pml4[pe] & P_PRESENT)) {
                uint64_t *n = (uint64_t *)pmm_alloc();
                if (!n) return 0;
                memset(n, 0, PAGE_SIZE);
                vm_kernel_pml4e_set((unsigned)pe, (uint64_t)n | P_PRESENT | P_RW); /* во все пространства, не только в главное */
            }
            pdp = (uint64_t *)(pml4[pe] & ~0xFFFULL);
        }
        if (pdp[g & 511] & P_PRESENT) continue; /* уже отображено (ОЗУ или MMIO < 4 ГиБ) */
        uint64_t *t = (uint64_t *)pmm_alloc();
        if (!t) return 0;
        memset(t, 0, PAGE_SIZE);
        for (uint64_t i = 0; i < 512; i++)
            t[i] = (g * GIB + i * HUGE) | P_PRESENT | P_RW | P_HUGE | P_PCD | P_PWT;
        pdp[g & 511] = (uint64_t)t | P_PRESENT | P_RW;
    }
    cpu_write_cr3(cpu_read_cr3());                                       /* сброс TLB текущего пространства (не переключаем CR3!) */
    return (void *)phys;
}
