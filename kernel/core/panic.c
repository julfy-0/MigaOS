#include <stdarg.h>
#include <kernel/kernel.h>
#include <kernel/arch.h>

void panic(const char *fmt, ...)
{
    cpu_cli();
    kprintf("\n*** KERNEL PANIC: ");
    va_list ap;
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
    kprintf(" ***\nSystem halted.\n");
    for (;;) cpu_hlt();
}
