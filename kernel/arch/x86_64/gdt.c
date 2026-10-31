#include <kernel/arch.h>

/* Порядок селекторов выбран под SYSCALL/SYSRET (для C4):
 *   0x08 kernel code   0x10 kernel data   0x18 user data   0x20 user code (64-bit)   0x28 TSS (16 байт)
 * SYSCALL: CS = STAR[47:32], SS = STAR[47:32] + 8           -> STAR[47:32] = 0x08
 * SYSRET (64-bit): SS = STAR[63:48] + 8, CS = STAR[63:48] + 16 -> STAR[63:48] = 0x10
 * Для SYSRET данные пользователя обязаны стоять РОВНО перед кодом: 0x18 (SS|3 = 0x1B), 0x20 (CS|3 = 0x23). */

struct __attribute__((packed)) tss {
    uint32_t reserved0;
    uint64_t rsp[3];        /* rsp[0] = RSP0: стек ядра при переходе из кольца 3 */
    uint64_t reserved1;
    uint64_t ist[7];        /* ist[0] = IST1: отдельный стек для #DF */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb;          /* смещение битовой карты I/O; = sizeof => карты нет, порты из кольца 3 запрещены */
};

/* Не const: ltr выставляет бит Busy в дескрипторе TSS, то есть пишет в GDT. */
static uint64_t gdt[7] __attribute__((aligned(16))) = {
    0,
    0x00AF9A000000FFFFULL,   /* 0x08 kernel code, L=1, DPL0 */
    0x00CF92000000FFFFULL,   /* 0x10 kernel data, DPL0 */
    0x00CFF2000000FFFFULL,   /* 0x18 user data, DPL3 */
    0x00AFFA000000FFFFULL,   /* 0x20 user code, L=1, DPL3 */
    0, 0,                    /* 0x28 TSS (заполняется в gdt_init) */
};

static struct tss tss __attribute__((aligned(16)));
static uint8_t df_stack[8192] __attribute__((aligned(16)));

struct __attribute__((packed)) dtr { uint16_t limit; uint64_t base; };

static void set_tss_desc(uint64_t base, uint32_t limit)
{
    gdt[5] = (limit & 0xFFFFULL) | ((base & 0xFFFFFFULL) << 16) | (0x89ULL << 40) |
             (((uint64_t)(limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFFULL) << 56);
    gdt[6] = base >> 32;
}

void gdt_set_rsp0(uint64_t rsp0) { tss.rsp[0] = rsp0; }

void gdt_init(void)
{
    tss.iopb = sizeof tss;
    tss.ist[0] = (uint64_t)df_stack + sizeof df_stack;
    set_tss_desc((uint64_t)&tss, sizeof tss - 1);

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
        "movw $0x28, %%ax\n\t"
        "ltr  %%ax\n\t"
        :: "m"(d) : "rax", "memory");
}
