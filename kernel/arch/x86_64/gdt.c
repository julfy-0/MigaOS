#include <kernel/arch.h>

/* 0x08 kernel code, 0x10 kernel data, 0x18 user data, 0x20 user code.
 * TODO: TSS + IST для double fault. */
static const uint64_t gdt[5] = {
    0,
    0x00AF9A000000FFFFULL,
    0x00CF92000000FFFFULL,
    0x00CFF2000000FFFFULL,
    0x00AFFA000000FFFFULL,
};

struct __attribute__((packed)) dtr { uint16_t limit; uint64_t base; };

void gdt_init(void)
{
    struct dtr d = {sizeof gdt - 1, (uint64_t)gdt};
    __asm__ volatile(
        "lgdt %0\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "xorl %%eax, %%eax\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        :: "m"(d) : "rax", "memory");
}
