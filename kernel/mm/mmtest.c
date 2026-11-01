/* Самотест подсистемы памяти. Запускается сам: регистрируется как запись реестра драйверов стадии
 * DRV_NORMAL (после pmm_init, до sti), поэтому правок в core/main.c не требуется.
 * Итог в журнал: "SELFTEST mm OK" или "SELFTEST mm FAIL" (перед FAIL печатаются причины). */
#include <kernel/kernel.h>
#include <kernel/arch.h>
#include <kernel/mm.h>
#include <miga/mem.h>
#include <miga/string.h>

static int fails;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fails++;                                                        \
            kprintf("[mm-test] FAIL line %d: ", __LINE__);                  \
            kprintf(__VA_ARGS__);                                           \
            kprintf("\n");                                                  \
        }                                                                   \
    } while (0)

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

static void fill(void *p, size_t n, uint8_t seed)
{
    uint8_t *b = p;
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(seed + i * 7);
}

/* 0 = содержимое цело */
static int verify(const void *p, size_t n, uint8_t seed)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) if (b[i] != (uint8_t)(seed + i * 7)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ куча */
static void test_heap_sizes(void)
{
    static const size_t sz[] = {1, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 100, 127, 128, 255, 256, 500, 512,
                                1000, 1024, 1025, 2000, 4000, 4032, 4033, 4096, 5000, 10000, 70000, 300000};
    enum { N = sizeof sz / sizeof sz[0] };
    void *p[N];

    for (int i = 0; i < N; i++) {
        p[i] = kmalloc(sz[i]);
        CHECK(p[i] != 0, "kmalloc(%lu) returned NULL", (unsigned long)sz[i]);
        if (!p[i]) continue;
        CHECK(((uintptr_t)p[i] & 15) == 0, "kmalloc(%lu) = %p is not 16-byte aligned", (unsigned long)sz[i], p[i]);
        fill(p[i], sz[i], (uint8_t)(i * 13 + 1));
    }
    /* Блоки не пересекаются и ничьё содержимое не затёрто соседями. */
    for (int i = 0; i < N; i++) {
        if (!p[i]) continue;
        CHECK(!verify(p[i], sz[i], (uint8_t)(i * 13 + 1)), "block %d (size %lu) corrupted", i, (unsigned long)sz[i]);
        for (int j = i + 1; j < N; j++) {
            if (!p[j]) continue;
            uintptr_t a = (uintptr_t)p[i], b = (uintptr_t)p[j];
            CHECK(a + sz[i] <= b || b + sz[j] <= a, "blocks %d and %d overlap", i, j);
        }
    }
    for (int i = 0; i < N; i += 2) kfree(p[i]);      /* сначала чётные, потом нечётные в обратном порядке */
    for (int i = N - 1; i >= 0; i--) if (i & 1) kfree(p[i]);
}

static void test_kzalloc(void)
{
    static const size_t sz[] = {16, 100, 1024, 3000, 100000};
    for (unsigned k = 0; k < sizeof sz / sizeof sz[0]; k++) {
        uint8_t *d = kmalloc(sz[k]);
        CHECK(d != 0, "kmalloc(%lu) NULL", (unsigned long)sz[k]);
        if (!d) continue;
        memset(d, 0xAB, sz[k]);
        kfree(d);                                     /* следующая выдача того же размера, скорее всего, вернёт тот же блок */
        uint8_t *z = kzalloc(sz[k]);
        CHECK(z != 0, "kzalloc(%lu) NULL", (unsigned long)sz[k]);
        if (!z) continue;
        int dirty = 0;
        for (size_t i = 0; i < sz[k]; i++) dirty |= z[i];
        CHECK(!dirty, "kzalloc(%lu) returned non-zero memory", (unsigned long)sz[k]);
        kfree(z);
    }
}

static void test_heap_stress(void)
{
    enum { N = 2500 };
    void **p = kzalloc(N * sizeof(void *));
    uint16_t *len = kzalloc(N * sizeof(uint16_t));
    CHECK(p && len, "no memory for stress tables");
    if (!p || !len) { kfree(p); kfree(len); return; }

    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < N; i++) {
            if (p[i]) continue;
            len[i] = (uint16_t)(1 + rnd() % 1500);    /* и малые классы, и крупные блоки */
            p[i] = kmalloc(len[i]);
            if (!p[i]) { CHECK(0, "stress alloc %d (%u bytes) NULL", i, (unsigned)len[i]); continue; }
            fill(p[i], len[i], (uint8_t)i);
        }
        for (int i = 0; i < N; i++) {                 /* случайная половина освобождается, содержимое проверяется перед free */
            if (!p[i] || (rnd() & 1)) continue;
            CHECK(!verify(p[i], len[i], (uint8_t)i), "stress block %d (%u bytes) corrupted", i, (unsigned)len[i]);
            kfree(p[i]);
            p[i] = 0;
        }
    }
    for (int i = 0; i < N; i++) {
        if (!p[i]) continue;
        CHECK(!verify(p[i], len[i], (uint8_t)i), "stress block %d (%u bytes) corrupted at end", i, (unsigned)len[i]);
        kfree(p[i]);
    }
    kfree(p);
    kfree(len);
}

static void test_heap_edges(void)
{
    kfree(0);                                          /* допустимо */
    void *z = kmalloc(0);
    CHECK(z != 0, "kmalloc(0) must return a valid block");
    kfree(z);
    CHECK(kmalloc((size_t)1 << 40) == 0, "kmalloc(1 TiB) must fail");
    CHECK(kmalloc((size_t)-1) == 0, "kmalloc(SIZE_MAX) must fail");
    CHECK(kzalloc((size_t)-1) == 0, "kzalloc(SIZE_MAX) must fail");
}

/* ------------------------------------------------------------------ DMA: старый интерфейс работает как раньше */
static void test_dma(void)
{
    uint64_t before = pmm_free_pages();
    uint8_t *a = dma_alloc_page(), *b = dma_alloc_page();
    CHECK(a && b && a != b, "dma_alloc_page returned %p, %p", (void *)a, (void *)b);
    if (a && b) {
        CHECK(((uint64_t)a & (PAGE_SIZE - 1)) == 0 && ((uint64_t)b & (PAGE_SIZE - 1)) == 0, "DMA pages not page-aligned");
        int dirty = 0;
        for (unsigned i = 0; i < PAGE_SIZE; i++) dirty |= a[i] | b[i];
        CHECK(!dirty, "dma_alloc_page must return zeroed pages");
        CHECK(pmm_free_pages() == before - 2, "dma_alloc_page must take exactly one PMM page each");
    }
    dma_free_page(a);
    dma_free_page(b);
    CHECK(pmm_free_pages() == before, "dma pages leaked (%lu -> %lu)", before, pmm_free_pages());
}

/* ------------------------------------------------------------------ адресные пространства */
#define KVA1 0xFFFF900000000000ULL                 /* PML4[288]: таблицы создаём заранее ("прогрев") */
#define KVA2 0xFFFFA00000000000ULL                 /* PML4[320]: НОВАЯ запись PML4, появляется после создания пространства */
#define UVA  (VM_USER_BASE + 0x10000000ULL)

static uint64_t vm_persist_pages;                  /* страницы, навсегда ушедшие под ядерные таблицы верхней половины */

static uint64_t pml4_entry(uint64_t cr3, unsigned idx) { return ((uint64_t *)P2V(cr3))[idx]; }

/* Биты U/S на всех четырёх уровнях пути к va (бит 0 = PML4, ... бит 3 = PT); 0xFF = путь оборван. */
static unsigned us_path(uint64_t cr3, uint64_t va)
{
    static const unsigned sh[4] = {39, 30, 21, 12};
    uint64_t *t = P2V(cr3);
    unsigned r = 0;
    for (int l = 0; l < 4; l++) {
        uint64_t e = t[(va >> sh[l]) & 511];
        if (!(e & 1)) return 0xFF;
        if (e & 4) r |= 1u << l;
        t = P2V(e & 0x000FFFFFFFFFF000ULL);
    }
    return r;
}

#ifndef MM_HOSTTEST
/* Настоящая трансляция адресов: переключаем CR3 на новое пространство и читаем/пишем через его отображения. */
static void test_vm_hw(uint64_t kcr3, uint64_t sp, uint64_t fk, uint64_t fu)
{
    uint64_t old = cpu_read_cr3();
    CHECK((old & ~0xFFFULL) == kcr3, "boot CR3 %#lx is not the kernel PML4 %#lx", old, kcr3);

    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
    CHECK(((lo >> 11) & 1) == (uint32_t)vm_nx_enabled(), "EFER.NXE=%u but vm_nx_enabled()=%d", (lo >> 11) & 1, vm_nx_enabled());

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    int smap = (cr4 >> 21) & 1;                    /* при SMAP кольцо 0 не может трогать страницы U/S: этот кусок пропускаем */

    volatile uint64_t *pk = P2V(fk), *pu = P2V(fu);
    *pk = 0x1122334455667788ULL;
    *pu = 0;

    uint64_t rk = 0;
    uint64_t f = cpu_irq_save();
    cpu_write_cr3(sp);
    rk = *(volatile uint64_t *)KVA2;               /* ядерная страница верхней половины, созданная ПОСЛЕ vm_space_create */
    if (!smap) *(volatile uint64_t *)UVA = 0xA5A5A5A55A5A5A5AULL;
    cpu_write_cr3(old);
    cpu_irq_restore(f);

    CHECK(rk == 0x1122334455667788ULL, "read through KVA2 in new space gave %#lx", rk);
    if (!smap) CHECK(*pu == 0xA5A5A5A55A5A5A5AULL, "write through UVA in new space did not reach the frame (%#lx)", *pu);
    else kprintf("[mm-test] note: CR4.SMAP is set, user-page access check skipped\n");

    uint64_t pa; unsigned fl;
    uint64_t code = (uint64_t)&fails & ~0xFFFULL;  /* адрес из образа ядра: identity map 2 МиБ, только для ядра, без NX */
    CHECK(vm_query(kcr3, code, &pa, &fl) == VM_OK && pa == code, "identity query of kernel data page failed");
    CHECK(!(fl & VM_USER) && (fl & VM_WRITE), "identity map flags are %#x", fl);
}
#endif

static void test_vm(void)
{
    uint64_t kcr3 = vm_kernel_cr3();
    CHECK(kcr3 != 0 && (kcr3 & 0xFFF) == 0, "vm_kernel_cr3() = %#lx", kcr3);
    unsigned nxf = vm_nx_enabled() ? VM_NOEXEC : 0;
    uint64_t pa; unsigned fl;

    uint64_t f1 = pmm_alloc(), f2 = pmm_alloc(), f3 = pmm_alloc();
    CHECK(f1 && f2 && f3, "no frames for vm test");
    if (!(f1 && f2 && f3)) { pmm_free(f1); pmm_free(f2); pmm_free(f3); return; }

    /* прогрев: таблицы под KVA1 создаются в главной PML4 и остаются в ней */
    uint64_t w0 = pmm_free_pages();
    CHECK(vm_map(kcr3, KVA1, f1, VM_WRITE) == VM_OK, "vm_map(kernel, KVA1)");
    CHECK(vm_unmap(kcr3, KVA1, &pa) == VM_OK && pa == f1, "vm_unmap(kernel, KVA1)");
    vm_persist_pages = w0 - pmm_free_pages();

    kheap_trim();
    uint64_t base = pmm_free_pages();

    uint64_t sp = vm_space_create();
    CHECK(sp != 0 && sp != kcr3 && (sp & 0xFFF) == 0, "vm_space_create() = %#lx", sp);
    if (!sp) { pmm_free(f1); pmm_free(f2); pmm_free(f3); return; }

    /* ядерные записи PML4 скопированы, пользовательские пусты */
    unsigned bad = 0;
    for (unsigned i = 0; i < 512; i++) {
        uint64_t want = (i >= 128 && i <= 255) ? 0 : pml4_entry(kcr3, i);
        if (pml4_entry(sp, i) != want) bad++;
    }
    CHECK(!bad, "%u PML4 entries of the new space differ from the kernel PML4 / user half not empty", bad);

    /* НОВАЯ ядерная запись PML4, появившаяся после создания пространства, должна дойти до него */
    uint64_t b2 = pmm_free_pages();
    CHECK(vm_map(kcr3, KVA2, f1, VM_WRITE | VM_NOEXEC) == VM_OK, "vm_map(kernel, KVA2)");
    uint64_t persisted2 = b2 - pmm_free_pages();
    vm_persist_pages += persisted2;
    CHECK((pml4_entry(kcr3, 320) & 1) && pml4_entry(sp, 320) == pml4_entry(kcr3, 320), "new kernel PML4[320] was not propagated to the existing space");
    CHECK(vm_query(sp, KVA2, &pa, &fl) == VM_OK && pa == f1 && fl == (VM_WRITE | nxf),
          "KVA2 seen from the other space: pa=%#lx flags=%#x", pa, fl);

    /* флаги пользовательских страниц */
    CHECK(vm_map(sp, UVA, f2, VM_USER | VM_WRITE | VM_NOEXEC) == VM_OK, "vm_map(user rw nx)");
    CHECK(vm_query(sp, UVA, &pa, &fl) == VM_OK && pa == f2 && fl == (VM_USER | VM_WRITE | nxf),
          "user rw+nx round trip: pa=%#lx flags=%#x", pa, fl);
    CHECK(us_path(sp, UVA) == 0xF, "U/S must be set on every level of a user page path (got %#x)", us_path(sp, UVA));
    CHECK(us_path(kcr3, KVA2) == 0, "kernel page path must not have U/S on any level (got %#x)", us_path(kcr3, KVA2));
    CHECK(vm_map(sp, UVA + 0x1000, f3, VM_USER) == VM_OK, "vm_map(user ro x)");
    CHECK(vm_query(sp, UVA + 0x1000, &pa, &fl) == VM_OK && pa == f3 && fl == VM_USER, "user ro round trip: flags=%#x", fl);
    CHECK(vm_query(kcr3, UVA, 0, 0) == VM_E_NOENT, "user mapping leaked into the kernel space");

    /* отказы */
    CHECK(vm_map(sp, UVA, f2, VM_USER) == VM_E_EXIST, "double map must fail with EXIST");
    CHECK(vm_map(sp, UVA + 1, f2, VM_USER) == VM_E_INVAL, "unaligned va must fail");
    CHECK(vm_map(sp, UVA + 0x5000, f2 + 1, VM_USER) == VM_E_INVAL, "unaligned pa must fail");
    CHECK(vm_map(sp, KVA1, f2, VM_USER) == VM_E_INVAL, "VM_USER in the kernel range must fail");
    CHECK(vm_map(sp, UVA + 0x5000, f2, 0) == VM_E_INVAL, "kernel mapping in the user range must fail");
    CHECK(vm_map(sp, 0x400000, f2, 0) == VM_E_INVAL, "mapping into the identity region must fail");
    CHECK(vm_map(sp, UVA + 0x5000, f2, 0x80) == VM_E_INVAL, "unknown flags must fail");
    CHECK(vm_map(sp, 0x0000900000000000ULL, f2, VM_USER) == VM_E_INVAL, "non-canonical address must fail");
    CHECK(vm_unmap(sp, UVA + 0x7000, 0) == VM_E_NOENT, "unmap of a hole in an existing table");
    CHECK(vm_unmap(sp, UVA + (3ULL << 39), 0) == VM_E_NOENT, "unmap where no tables exist");
    CHECK(vm_query(sp, UVA + 0x9000, 0, 0) == VM_E_NOENT, "query of a hole");

    /* unmap, повторный map с другими правами */
    CHECK(vm_unmap(sp, UVA + 0x1000, &pa) == VM_OK && pa == f3, "vm_unmap returned pa=%#lx", pa);
    CHECK(vm_query(sp, UVA + 0x1000, 0, 0) == VM_E_NOENT, "page still mapped after unmap");
    CHECK(vm_map(sp, UVA + 0x1000, f3, VM_USER | VM_WRITE) == VM_OK, "remap after unmap");

    /* все уровни таблиц: разные PT, PD, PDPT (кадры повторяются намеренно, это алиасы) */
    static const uint64_t far[] = {UVA + (2ULL << 20), UVA + (1ULL << 30), UVA + (512ULL << 30)};
    for (unsigned i = 0; i < 3; i++) {
        CHECK(vm_map(sp, far[i], f3, VM_USER | VM_WRITE) == VM_OK, "vm_map(user far %u)", i);
        CHECK(vm_query(sp, far[i], &pa, 0) == VM_OK && pa == f3, "user far %u query", i);
    }

#ifndef MM_HOSTTEST
    test_vm_hw(kcr3, sp, f1, f2);
#endif

    CHECK(vm_unmap(kcr3, KVA2, &pa) == VM_OK && pa == f1, "vm_unmap(kernel, KVA2)");
    CHECK(vm_query(sp, KVA2, 0, 0) == VM_E_NOENT, "KVA2 still visible from the other space after unmap");

    /* destroy без VM_FREE_FRAMES освобождает только таблицы (кадры f1..f3 наши), с VM_FREE_FRAMES и кадры */
    vm_space_destroy(sp, 0);
    pmm_free(f1); pmm_free(f2); pmm_free(f3);
    kheap_trim();
    CHECK(pmm_free_pages() == base + 3 - persisted2, "address space leak: %lu free pages, expected %lu",
          pmm_free_pages(), base + 3 - persisted2);

    uint64_t sp2 = vm_space_create();
    uint64_t g1 = pmm_alloc(), g2 = pmm_alloc();
    CHECK(sp2 && g1 && g2, "no memory for second space");
    if (sp2 && g1 && g2) {
        CHECK(vm_map(sp2, UVA, g1, VM_USER | VM_WRITE) == VM_OK && vm_map(sp2, UVA + (1ULL << 30), g2, VM_USER) == VM_OK,
              "vm_map in second space");
        vm_space_destroy(sp2, VM_FREE_FRAMES);     /* вернёт и g1, g2 */
    } else {
        if (sp2) vm_space_destroy(sp2, 0);
        pmm_free(g1); pmm_free(g2);
    }
    kheap_trim();
    CHECK(pmm_free_pages() == base + 3 - persisted2, "VM_FREE_FRAMES leak: %lu free pages, expected %lu",
          pmm_free_pages(), base + 3 - persisted2);
}

int mm_selftest(void)
{
    fails = 0;
    uint64_t fl = cpu_irq_save();

    kheap_trim();
    uint64_t pages0 = pmm_free_pages();
    struct kheap_stats s0, s1;
    kheap_stats_get(&s0);

    test_heap_sizes();
    test_kzalloc();
    test_heap_stress();
    test_heap_edges();
    test_dma();
    test_vm();

    /* Утечки: ни одного живого блока сверх исходных, и после trim PMM получил все страницы назад. */
    kheap_stats_get(&s1);
    CHECK(s1.live_allocs == s0.live_allocs, "heap leak: %lu live blocks (was %lu)", s1.live_allocs, s0.live_allocs);
    CHECK(s1.live_bytes == s0.live_bytes, "heap leak: %lu live bytes (was %lu)", s1.live_bytes, s0.live_bytes);
    CHECK(s1.large_pages == s0.large_pages, "large-block pages leaked: %lu (was %lu)", s1.large_pages, s0.large_pages);
    CHECK(s1.total_allocs - s0.total_allocs == s1.total_frees - s0.total_frees,
          "alloc/free counters differ: %lu vs %lu", s1.total_allocs - s0.total_allocs, s1.total_frees - s0.total_frees);
    kheap_trim();
    CHECK(pmm_free_pages() == pages0 - vm_persist_pages, "PMM pages leaked: %lu free now, %lu before (%lu are kernel page tables kept on purpose)",
          pmm_free_pages(), pages0, vm_persist_pages);

    cpu_irq_restore(fl);
    kprintf("SELFTEST mm %s\n", fails ? "FAIL" : "OK");
    return fails ? -1 : 0;
}

static int mm_selftest_probe(const struct boot_info *bi) { return mm_selftest(); }
MIGA_DRIVER(mm_selftest, "mm-selftest", DRV_VER(1, 0), DRV_NORMAL, mm_selftest_probe);
