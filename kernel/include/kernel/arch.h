#pragma once
#include <stdint.h>
#include <miga/boot_info.h>

/* ---- x86_64 ---- */
/* Селекторы GDT (порядок важен для SYSCALL/SYSRET, см. gdt.c). */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UDATA (0x18 | 3)
#define SEL_UCODE (0x20 | 3)
#define SEL_TSS   0x28

void gdt_init(void);
void gdt_set_rsp0(uint64_t rsp0);   /* TSS.RSP0: стек ядра для входа из кольца 3 */
const char *exc_name(unsigned vec);
void idt_init(void);
void pic_init(void);
void pic_unmask(uint8_t irq);
void pic_eoi(uint8_t irq);
int  pic_is_spurious(uint8_t irq);

static inline void cpu_cli(void)  { __asm__ volatile("cli" ::: "memory"); }
static inline void cpu_sti(void)  { __asm__ volatile("sti" ::: "memory"); }
static inline void cpu_hlt(void)  { __asm__ volatile("hlt"); }
/* Простой с разрешением вытеснения: пока ядро спит в hlt, планировщик вправе отдать CPU другой задаче. */
void sched_idle_enter(void);
void sched_idle_leave(void);
static inline void cpu_idle(void)
{
    sched_idle_enter();
    __asm__ volatile("sti; hlt" ::: "memory");
    sched_idle_leave();
}

static inline uint64_t cpu_irq_save(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void cpu_irq_restore(uint64_t f) { __asm__ volatile("pushq %0; popfq" :: "r"(f) : "memory", "cc"); }
static inline uint64_t cpu_read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3,%0" : "=r"(v)); return v; }
static inline void cpu_write_cr3(uint64_t v) { __asm__ volatile("mov %0,%%cr3" :: "r"(v) : "memory"); }

struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};

void paging_init(const struct boot_info *bi);
