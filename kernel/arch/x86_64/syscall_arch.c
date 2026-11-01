/* kernel/arch/x86_64/syscall_arch.c — настройка SYSCALL/SYSRET (Intel SDM Vol.2 «SYSCALL»,
 * «SYSRET»; Vol.3 §5.8.8 «Fast System Calls in 64-bit Mode»; MSR — Vol.4). */
#include <kernel/syscall.h>
#include <kernel/arch.h>
#include <kernel/kernel.h>

#define MSR_EFER    0xC0000080u   /* бит 0 = SCE (SysCall Enable)                           */
#define MSR_STAR    0xC0000081u   /* [47:32] CS при SYSCALL, [63:48] база для SYSRET        */
#define MSR_LSTAR   0xC0000082u   /* RIP входа из 64-битного режима                         */
#define MSR_SFMASK  0xC0000084u   /* биты RFLAGS, сбрасываемые при SYSCALL                  */
#define EFER_SCE    (1ull << 0)

/* GDT (gdt.c): 0x08 kcode64, 0x10 kdata, 0x18 udata, 0x20 ucode64.
 *   SYSCALL:    CS = STAR[47:32]       = 0x08, SS = STAR[47:32] + 8  = 0x10
 *   SYSRET q:   CS = STAR[63:48] + 16  = 0x20|3, SS = STAR[63:48] + 8 = 0x18|3
 * => STAR[63:48] = 0x10. Данные пользователя обязаны стоять ровно перед кодом. */
_Static_assert(SEL_KCODE == 0x08 && SEL_KDATA == 0x10, "STAR[47:32] / SYSCALL SS");
_Static_assert(SEL_UDATA == (0x10 + 8 + 3) && SEL_UCODE == (0x10 + 16 + 3), "STAR[63:48] / SYSRET");
#define STAR_SYSRET_BASE 0x10

/* TF(8) IF(9) DF(10) NT(14) AC(18): IF=0 до переключения стека и на всё время вызова;
 * DF=0 — требование SysV ABI к C-коду ядра; TF/NT/AC — чтобы user не влиял на ядро. */
#define SFMASK_BITS ((1ull << 8) | (1ull << 9) | (1ull << 10) | (1ull << 14) | (1ull << 18))

static inline uint64_t rdmsr(uint32_t m)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t m, uint64_t v)
{
    __asm__ volatile("wrmsr" ::"c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}

extern void syscall_entry(void);
extern uint64_t syscall_kstack_top;            /* syscall.S */

void syscall_set_kstack(uint64_t top) { syscall_kstack_top = top; }

void syscall_arch_init(void)
{
    /* CPUID.80000001H:EDX[11] = SYSCALL/SYSRET в 64-битном режиме */
    uint32_t a = 0x80000001u, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if (!(d & (1u << 11))) panic("syscall: CPU has no SYSCALL/SYSRET");

    wrmsr(MSR_EFER,   rdmsr(MSR_EFER) | EFER_SCE);
    wrmsr(MSR_STAR,   ((uint64_t)STAR_SYSRET_BASE << 48) | ((uint64_t)SEL_KCODE << 32));
    wrmsr(MSR_LSTAR,  (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, SFMASK_BITS);

    if (!(rdmsr(MSR_EFER) & EFER_SCE) || rdmsr(MSR_LSTAR) != (uint64_t)syscall_entry)
        panic("syscall: MSR readback mismatch");
}
