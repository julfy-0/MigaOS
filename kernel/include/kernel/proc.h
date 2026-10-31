#pragma once
/* Процессы и планировщик (C3). Одно ядро CPU, round-robin по тику PIT. */
#include <stdint.h>
#include <stddef.h>
#include <kernel/arch.h>

#define MAX_PROCS     16
#define KSTACK_PAGES  4
#define KSTACK_SIZE   (KSTACK_PAGES * 4096)
#define SLICE_TICKS   10                     /* PIT 1000 Гц => 10 мс */

/* Адресное пространство пользователя: PML4[128]. Ядро (identity map) живёт в PML4[0] без бита U/S,
 * поэтому из кольца 3 оно недоступно; mmio_map() занимает PML4[1..] только для BAR ниже 64 ТиБ. */
#define USER_BASE       0x0000400000000000ULL
#define USER_CODE_VA    USER_BASE
#define USER_STACK_TOP  (USER_BASE + 0x8000)

enum proc_state { PROC_UNUSED = 0, PROC_NEW, PROC_RUNNABLE, PROC_RUNNING, PROC_ZOMBIE };

#define PF_USER     1u   /* процесс кольца 3 со своим CR3 */
#define PF_KPREEMPT 2u   /* поток ядра можно вытеснять по тику в любой точке (он не трогает kprintf/pmm без preempt_disable) */

struct proc {
    int pid;
    char name[16];
    enum proc_state state;
    uint32_t flags;
    uint64_t cr3;                 /* физ. адрес PML4 (у потоков ядра — общий CR3 ядра) */
    uint64_t kstack_base;         /* низ стека ядра (phys == virt) */
    uint64_t kstack_top;          /* значение TSS.RSP0 для этой задачи */
    uint64_t ksp;                 /* сохранённый RSP ядра внутри ctx_switch */
    struct regs *tf;              /* последний кадр прерывания (регистры задачи на стеке ядра) */
    uint64_t user_rip, user_rsp;  /* точка входа и стек кольца 3 */
    uint64_t code_pa;             /* физ. страница кода/данных процесса (для диагностики из ядра) */
    uint32_t slice;               /* осталось тиков */
    uint32_t preempt_count;       /* >0: вытеснение запрещено */
    int idle;                     /* задача спит в cpu_idle(): вытеснять можно */
    int exit_code;
};

extern struct proc *current;
extern uint64_t sched_switches;

/* --- sched.c --- */
void sched_init(void);                                   /* kmain становится задачей 0 */
uint64_t sched_kernel_cr3(void);
struct proc *proc_alloc(const char *name, uint32_t flags); /* слот + стек ядра, состояние NEW */
void proc_release(struct proc *p);                        /* освободить ресурсы зомби */
void sched_ready(struct proc *p);                         /* NEW -> RUNNABLE */
struct proc *kthread_create(const char *name, void (*fn)(void *), void *arg, uint32_t flags);
void sched_yield(void);
void sched_preempt(const struct regs *r);                 /* из isr_dispatch после EOI тика PIT */
void sched_exit(int code) __attribute__((noreturn));
void sched_post_switch(void);                             /* вызывается из asm-трамплинов и после ctx_switch */
void kthread_exit(void) __attribute__((noreturn));
static inline void preempt_disable(void) { if (current) current->preempt_count++; }
static inline void preempt_enable(void)  { if (current && current->preempt_count) current->preempt_count--; }

/* --- proc.c --- */
void proc_init(void);
uint64_t uspace_create(void);                             /* новый PML4: ядро скопировано, пользовательская часть пуста */
int  uspace_map(uint64_t pml4, uint64_t va, uint64_t pa, uint64_t flags);
void uspace_destroy(uint64_t pml4);
struct proc *proc_create_user(const char *name, const void *code, size_t len, uint64_t arg);
void proc_handle_user_fault(struct regs *r, uint64_t cr2);
#ifdef PROCTEST
void proc_selftest(int stage);
#endif
