/* Страницы для DMA: берутся из PMM, обнуляются. Память identity-mapped, поэтому адрес == физический. */
#include <kernel/kernel.h>
#include <miga/mem.h>
#include <miga/string.h>

void *dma_alloc_page(void)
{
    uint64_t pa = pmm_alloc();
    if (!pa) return 0;
    memset((void *)pa, 0, PAGE_SIZE);
    return (void *)pa;
}
