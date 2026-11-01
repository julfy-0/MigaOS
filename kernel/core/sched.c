/* Планировщик: round-robin по тику PIT, контекстное переключение стеков ядра.
 *
 * Политика вытеснения (одно CPU, ядро не реентерабельно: printk с общим буфером, pmm без блокировок):
 *  - код кольца 3 вытесняется на любом тике;
 *  - ядро — только если задача спит в cpu_idle() (hlt), либо это поток с PF_KPREEMPT и preempt_count == 0;
 *  - всё остальное переключается добровольно через sched_yield().
 * Регистры вытесненной задачи лежат на её стеке ядра (isr_common), в struct proc хранится только ksp. */
#include <kernel/kernel.h>
#include <kernel/proc.h>
#include <miga/string.h>

extern void ctx_switch(uint64_t *old_ksp, uint64_t new_ksp);
extern void kthread_tramp(void);

static struct proc pool[MAX_PROCS];
struct proc *current;
uint64_t sched_switches;
static int next_pid = 1;
static uint64_t kernel_cr3;

uint64_t sched_kernel_cr3(void) { return kernel_cr3; }

static void copy_name(char *d, const char *s)
{
    size_t i = 0;
    for (; s && s[i] && i < sizeof(((struct proc *)0)->name) - 1; i++) d[i] = s[i];
    d[i] = 0;
}

void sched_init(void)
{
    kernel_cr3 = cpu_read_cr3();
    struct proc *t = &pool[0];
    memset(t, 0, sizeof *t);
    t->pid = 0;
    copy_name(t->name, "kmain");
    t->state = PROC_RUNNING;
    t->cr3 = kernel_cr3;
    t->slice = SLICE_TICKS;
    current = t;                        /* работаем на загрузочном стеке entry.S; kstack_top = 0 */
}

struct proc *proc_alloc(const char *name, uint32_t flags)
{
    uint64_t fl = cpu_irq_save();
    struct proc *p = 0;
    for (int i = 1; i < MAX_PROCS; i++)
        if (pool[i].state == PROC_UNUSED) { p = &pool[i]; break; }
    if (p) { memset(p, 0, sizeof *p); p->state = PROC_NEW; }
    cpu_irq_restore(fl);
    if (!p) return 0;

    uint64_t ks = pmm_alloc_contig(KSTACK_PAGES);
    if (!ks) { p->state = PROC_UNUSED; return 0; }
    p->pid = next_pid++;
    copy_name(p->name, name);
    p->flags = flags;
    p->cr3 = kernel_cr3;
    p->kstack_base = ks;
    p->kstack_top = ks + KSTACK_SIZE;
    return p;
}

void proc_release(struct proc *p)
{
    if ((p->flags & PF_USER) && p->cr3 && p->cr3 != kernel_cr3) uspace_destroy(p->cr3);
    for (uint64_t i = 0; p->kstack_base && i < KSTACK_PAGES; i++) pmm_free(p->kstack_base + i * PAGE_SIZE);
    memset(p, 0, sizeof *p);            /* state = PROC_UNUSED */
}

void sched_ready(struct proc *p)
{
    uint64_t fl = cpu_irq_save();
    p->state = PROC_RUNNABLE;
    cpu_irq_restore(fl);
}

struct proc *kthread_create(const char *name, void (*fn)(void *), void *arg, uint32_t flags)
{
    struct proc *p = proc_alloc(name, flags & ~PF_USER);
    if (!p) return 0;
    uint64_t *sp = (uint64_t *)p->kstack_top;
    *--sp = (uint64_t)kthread_tramp;    /* адрес возврата ctx_switch; после ret RSP == kstack_top (выровнен по 16) */
    *--sp = 0;                          /* rbp */
    *--sp = 0;                          /* rbx */
    *--sp = (uint64_t)fn;               /* r12 */
    *--sp = (uint64_t)arg;              /* r13 */
    *--sp = 0;                          /* r14 */
    *--sp = 0;                          /* r15 */
    p->ksp = (uint64_t)sp;
    sched_ready(p);
    return p;
}

/* Выбор следующей: круг по пулу, начиная со следующего за текущим. Вызывать с IF=0. */
static struct proc *pick_next(void)
{
    int start = (int)(current - pool);
    for (int i = 1; i < MAX_PROCS; i++) {
        struct proc *p = &pool[(start + i) % MAX_PROCS];
        if (p->state == PROC_RUNNABLE) return p;
    }
    return current;
}

static void schedule(void)
{
    struct proc *prev = current, *next = pick_next();
    if (next == prev) {
        if (prev->state == PROC_ZOMBIE) panic("sched: nothing to run");
        prev->slice = SLICE_TICKS;
        return;
    }
    if (prev->state == PROC_RUNNING) prev->state = PROC_RUNNABLE;
    next->state = PROC_RUNNING;
    next->slice = SLICE_TICKS;
    current = next;
    sched_switches++;
    if (next->cr3 != cpu_read_cr3()) cpu_write_cr3(next->cr3);   /* стеки и код ядра отображены во всех пространствах */
    if (next->kstack_top) gdt_set_rsp0(next->kstack_top);
    ctx_switch(&prev->ksp, next->ksp);
    sched_post_switch();                /* мы уже на стеке другой (или той же) задачи */
}

/* Зомби освобождаем уже не на их стеке и не под их CR3. IF=0 во всех местах вызова. */
void sched_post_switch(void)
{
    for (int i = 1; i < MAX_PROCS; i++)
        if (pool[i].state == PROC_ZOMBIE && &pool[i] != current) proc_release(&pool[i]);
}

void sched_yield(void)
{
    uint64_t fl = cpu_irq_save();
    schedule();
    cpu_irq_restore(fl);
}

void sched_block(void)
{
    uint64_t fl = cpu_irq_save();
    current->state = PROC_BLOCKED;
    schedule();                          /* вернулись: нас разбудили (или больше некого запускать) */
    while (current->state == PROC_BLOCKED) {
        cpu_irq_restore(fl | (1ull << 9));   /* IF=1: дать прийти прерыванию, которое разбудит */
        __asm__ volatile("hlt");
        cpu_irq_save();
        schedule();
    }
    cpu_irq_restore(fl);
}

void sched_wake(struct proc *p)
{
    uint64_t fl = cpu_irq_save();
    if (p && p->state == PROC_BLOCKED) p->state = PROC_RUNNABLE;
    cpu_irq_restore(fl);
}

void sched_preempt(const struct regs *r)
{
    struct proc *c = current;
    if (!c) return;
    c->tf = (struct regs *)r;
    if (c->slice) c->slice--;
    if (c->slice) return;
    if (!(r->cs & 3) && !c->idle && !((c->flags & PF_KPREEMPT) && !c->preempt_count)) return;  /* ядро: не сейчас */
    schedule();
}

void sched_exit(int code)
{
    cpu_irq_save();
    current->exit_code = code;
    current->state = PROC_ZOMBIE;
    schedule();
    panic("sched: zombie %d resumed", current->pid);
}

void kthread_exit(void) { sched_exit(0); }

void sched_idle_enter(void) { if (current) current->idle = 1; }
void sched_idle_leave(void) { if (current) current->idle = 0; }
