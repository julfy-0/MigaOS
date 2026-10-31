/* Физический аллокатор страниц: битовая карта (1 = занято). */
#include <kernel/kernel.h>
#include <miga/string.h>

static uint8_t *bitmap;
static uint64_t total_pages, free_pages, hint;

static inline int  test(uint64_t p)  { return bitmap[p >> 3] & (1 << (p & 7)); }
static inline void mark(uint64_t p)  { bitmap[p >> 3] |= (1 << (p & 7)); }
static inline void clear(uint64_t p) { bitmap[p >> 3] &= ~(1 << (p & 7)); }

void pmm_init(const struct boot_info *bi)
{
    uint64_t top = 0;
    for (uint64_t i = 0; i < bi->mem_count; i++) {
        const struct mem_region *r = &bi->mem[i];
        if (r->type == MEM_USABLE && r->base + r->length > top) top = r->base + r->length;
    }
    total_pages = top / PAGE_SIZE;
    uint64_t bytes = (total_pages + 7) / 8;
    uint64_t bpages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    /* битовую карту кладём в первую подходящую свободную область выше 1 МиБ */
    for (uint64_t i = 0; i < bi->mem_count && !bitmap; i++) {
        const struct mem_region *r = &bi->mem[i];
        uint64_t base = r->base < 0x100000 ? 0x100000 : r->base;
        if (r->type == MEM_USABLE && base < r->base + r->length &&
            r->base + r->length - base >= bpages * PAGE_SIZE)
            bitmap = (uint8_t *)base;
    }
    if (!bitmap) panic("pmm: no room for bitmap");

    memset(bitmap, 0xFF, bpages * PAGE_SIZE);
    for (uint64_t i = 0; i < bi->mem_count; i++) {
        const struct mem_region *r = &bi->mem[i];
        if (r->type != MEM_USABLE) continue;
        uint64_t s = r->base < 0x100000 ? 0x100000 : r->base; /* низкий 1 МиБ не трогаем */
        uint64_t e = r->base + r->length;
        for (uint64_t pa = (s + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); pa + PAGE_SIZE <= e; pa += PAGE_SIZE) {
            clear(pa / PAGE_SIZE);
            free_pages++;
        }
    }
    for (uint64_t pa = (uint64_t)bitmap; pa < (uint64_t)bitmap + bpages * PAGE_SIZE; pa += PAGE_SIZE) {
        if (!test(pa / PAGE_SIZE)) { mark(pa / PAGE_SIZE); free_pages--; }
    }
    pr_info("pmm: %lu MiB free of %lu MiB tracked\n", free_pages * PAGE_SIZE >> 20, total_pages * PAGE_SIZE >> 20);
}

uint64_t pmm_alloc(void)
{
    for (uint64_t n = 0; n < total_pages; n++) {
        uint64_t p = (hint + n) % total_pages;
        if (!test(p)) { mark(p); free_pages--; hint = p + 1; return p * PAGE_SIZE; }
    }
    return 0;
}

/* Непрерывный диапазон из n страниц (для больших буферов: backbuffer GUI и т.п.). 0 = не нашлось. */
uint64_t pmm_alloc_contig(uint64_t n)
{
    if (!n || n > total_pages) return 0;
    uint64_t run = 0;
    for (uint64_t p = 0; p < total_pages; p++) {
        if (test(p)) { run = 0; continue; }
        if (++run == n) {
            uint64_t first = p + 1 - n;
            for (uint64_t k = first; k <= p; k++) mark(k);
            free_pages -= n;
            return first * PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free(uint64_t pa)
{
    uint64_t p = pa / PAGE_SIZE;
    if (p < total_pages && test(p)) { clear(p); free_pages++; }
}

uint64_t pmm_total_pages(void) { return total_pages; }
uint64_t pmm_free_pages(void)  { return free_pages; }
