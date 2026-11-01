/* Страницы для DMA: берутся из PMM, обнуляются. Память identity-mapped, поэтому адрес == физический. */
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <miga/mem.h>
#include <miga/string.h>

void *dma_alloc_page(void)
{
    uint64_t pa = pmm_alloc();
    if (!pa) return 0;
    memset(P2V(pa), 0, PAGE_SIZE);
    return (void *)pa;          /* контракт mem.h: phys = (uint64_t)ptr */
}

void dma_free_page(void *p)
{
    if (p) pmm_free((uint64_t)p);
}
