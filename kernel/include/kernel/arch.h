#pragma once
#include <stdint.h>
#include <miga/boot_info.h>

/* ---- x86_64 ---- */
void gdt_init(void);
void idt_init(void);
void pic_init(void);
void pic_unmask(uint8_t irq);
void pic_eoi(uint8_t irq);
int  pic_is_spurious(uint8_t irq);

static inline void cpu_cli(void)  { __asm__ volatile("cli" ::: "memory"); }
static inline void cpu_sti(void)  { __asm__ volatile("sti" ::: "memory"); }
static inline void cpu_hlt(void)  { __asm__ volatile("hlt"); }
static inline void cpu_idle(void) { __asm__ volatile("sti; hlt" ::: "memory"); }

struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};

void paging_init(const struct boot_info *bi);
