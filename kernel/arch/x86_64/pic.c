#include <kernel/arch.h>
#include <miga/io.h>

#define PIC1 0x20
#define PIC2 0xA0

void pic_init(void)
{
    outb(PIC1, 0x11); io_wait();
    outb(PIC2, 0x11); io_wait();
    outb(PIC1 + 1, 0x20); io_wait(); /* IRQ0-7  -> векторы 32..39 */
    outb(PIC2 + 1, 0x28); io_wait(); /* IRQ8-15 -> векторы 40..47 */
    outb(PIC1 + 1, 0x04); io_wait();
    outb(PIC2 + 1, 0x02); io_wait();
    outb(PIC1 + 1, 0x01); io_wait();
    outb(PIC2 + 1, 0x01); io_wait();
    outb(PIC1 + 1, 0xFB); /* всё замаскировано, кроме каскада (IRQ2) */
    outb(PIC2 + 1, 0xFF);
}

void pic_unmask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1 + 1 : PIC2 + 1;
    outb(port, inb(port) & ~(1u << (irq & 7)));
}

void pic_eoi(uint8_t irq)
{
    if (irq >= 8) outb(PIC2, 0x20);
    outb(PIC1, 0x20);
}

/* Ложные IRQ7/IRQ15: бит в ISR не выставлен. */
int pic_is_spurious(uint8_t irq)
{
    if (irq != 7 && irq != 15) return 0;
    uint16_t port = (irq == 7) ? PIC1 : PIC2;
    outb(port, 0x0B);
    int real = inb(port) & 0x80;
    if (!real && irq == 15) outb(PIC1, 0x20); /* мастеру EOI всё равно нужен */
    return !real;
}
