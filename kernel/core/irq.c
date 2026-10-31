#include <miga/irq.h>
#include <kernel/kernel.h>
#include <kernel/arch.h>

static struct { irq_handler_t fn; void *ctx; } table[16];

int irq_register(uint8_t irq, irq_handler_t fn, void *ctx)
{
    if (irq >= 16 || table[irq].fn) return -1;
    table[irq].fn = fn;
    table[irq].ctx = ctx;
    pic_unmask(irq);
    return 0;
}

void irq_dispatch(unsigned irq)
{
    if (pic_is_spurious(irq)) return;
    if (table[irq].fn) table[irq].fn(table[irq].ctx);
    pic_eoi(irq);
}
