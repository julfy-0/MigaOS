/* Память для драйверов: DMA-страницы и MMIO. Физический адрес == виртуальный (identity map). */
#pragma once
#include <stdint.h>
#include <stddef.h>

void *dma_alloc_page(void);                 /* обнулённая страница 4 КиБ или NULL; phys = (uint64_t)ptr */
void *mmio_map(uint64_t phys, size_t size); /* гарантирует uncached-отображение окна, возвращает NULL при ошибке */
