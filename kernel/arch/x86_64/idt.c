#include <kernel/arch.h>
#include <kernel/kernel.h>
#include <kernel/proc.h>

struct __attribute__((packed)) idt_entry {
    uint16_t off_lo, sel;
    uint8_t ist, attr;
    uint16_t off_mid;
    uint32_t off_hi, zero;
};
struct __attribute__((packed)) dtr { uint16_t limit; uint64_t base; };

static struct idt_entry idt[256] __attribute__((aligned(16)));
extern const uint64_t isr_stub_table[48];

static const char *const exc_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow", "BOUND Range",
    "Invalid Opcode", "Device Not Available", "Double Fault", "Coprocessor Overrun",
    "Invalid TSS", "Segment Not Present", "Stack Fault", "General Protection",
    "Page Fault", "Reserved", "x87 FP", "Alignment Check", "Machine Check",
    "SIMD FP", "Virtualization", "Control Protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Hypervisor", "VMM Comm",
    "Security", "Reserved",
};

const char *exc_name(unsigned vec) { return vec < 32 ? exc_names[vec] : "IRQ"; }

void idt_init(void)
{
    for (int i = 0; i < 48; i++) {
        uint64_t h = isr_stub_table[i];
        idt[i] = (struct idt_entry){h & 0xFFFF, 0x08, 0, 0x8E, (h >> 16) & 0xFFFF, h >> 32, 0};
    }
    idt[8].ist = 1;                    /* #DF на отдельном стеке (TSS.IST1): переполнение стека ядра не станет triple fault */
    struct dtr d = {sizeof idt - 1, (uint64_t)idt};
    __asm__ volatile("lidt %0" ::"m"(d));
}

void isr_dispatch(struct regs *r)
{
    if (r->vector >= 32) {
        irq_dispatch(r->vector - 32);
        if (r->vector == 32) sched_preempt(r);   /* строго ПОСЛЕ EOI, иначе IRQ0 останется «в обслуживании» у вытесненной задачи */
        return;
    }
    uint64_t cr2 = 0;
    __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
    /* Исключение в кольце 3 убивает только процесс. NMI/#DF/#MC — аппаратные аварии, их не прощаем. */
    if ((r->cs & 3) && r->vector != 2 && r->vector != 8 && r->vector != 18)
        proc_handle_user_fault(r, cr2);          /* вернётся, только если текущая задача не пользовательский процесс */
    kprintf("\n*** CPU exception %lu: %s (err=%#lx) ***\n", r->vector, exc_names[r->vector], r->error);
    kprintf("RIP=%016lx CS=%lx RFLAGS=%lx RSP=%016lx\n", r->rip, r->cs, r->rflags, r->rsp);
    kprintf("RAX=%016lx RBX=%016lx RCX=%016lx RDX=%016lx\n", r->rax, r->rbx, r->rcx, r->rdx);
    kprintf("RSI=%016lx RDI=%016lx RBP=%016lx CR2=%016lx\n", r->rsi, r->rdi, r->rbp, cr2);
    panic("unhandled CPU exception");
}
