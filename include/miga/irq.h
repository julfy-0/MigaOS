#pragma once
#include <stdint.h>

typedef void (*irq_handler_t)(void *ctx);

/* irq: номер линии 0..15 (ISA IRQ). Возвращает 0 или -1. Размаскирует линию. */
int irq_register(uint8_t irq, irq_handler_t handler, void *ctx);
