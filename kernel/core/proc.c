/* Процессы кольца 3: адресные пространства, создание, обработка исключений пользователя.
 * Нет syscall (C4) и ELF-загрузчика (C5): код процесса — позиционно-независимый блоб, копируемый на страницу USER_CODE_VA. */
#include <kernel/kernel.h>
#include <kernel/proc.h>
#include <miga/string.h>

#define P_P   1ULL
#define P_RW  2ULL
#define P_US  4ULL
#define ADDR_MASK 0x000FFFFFFFFFF000ULL
#define USER_PML4_IDX ((USER_BASE >> 39) & 511)

extern void uthread_tramp(void);

void proc_init(void) { sched_init(); }

static uint64_t alloc_zeroed(void)
{
    uint64_t pa = pmm_alloc();
    if (pa) memset((void *)pa, 0, PAGE_SIZE);
    return pa;
}

uint64_t uspace_create(void)
{
    uint64_t pml4 = alloc_zeroed();
    if (!pml4) return 0;
    /* Ядро общее: копируем записи PML4 ядра (без бита U/S => кольцо 3 их не видит). */
    memcpy((void *)pml4, (void *)(sched_kernel_cr3() & ADDR_MASK), PAGE_SIZE);
    ((uint64_t *)pml4)[USER_PML4_IDX] = 0;
    return pml4;
}

int uspace_map(uint64_t pml4, uint64_t va, uint64_t pa, uint64_t flags)
{
    if (((va >> 39) & 511) != USER_PML4_IDX) return -1;   /* только пользовательская ветка */
    uint64_t *t = (uint64_t *)pml4;
    unsigned idx[4] = {(va >> 39) & 511, (va >> 30) & 511, (va >> 21) & 511, (va >> 12) & 511};
    for (int l = 0; l < 3; l++) {
        if (!(t[idx[l]] & P_P)) {
            uint64_t n = alloc_zeroed();
            if (!n) return -1;
            t[idx[l]] = n | P_P | P_RW | P_US;            /* U/S должен стоять на КАЖДОМ уровне */
        }
        t = (uint64_t *)(t[idx[l]] & ADDR_MASK);
    }
    t[idx[3]] = pa | flags | P_P;
    return 0;
}

static void free_tree(uint64_t table, int level)
{
    uint64_t *t = (uint64_t *)table;
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & P_P)) continue;
        uint64_t c = t[i] & ADDR_MASK;
        if (level > 1) free_tree(c, level - 1); else pmm_free(c);
    }
    pmm_free(table);
}

void uspace_destroy(uint64_t cr3)
{
    uint64_t *pml4 = (uint64_t *)(cr3 & ADDR_MASK);
    if (pml4[USER_PML4_IDX] & P_P) free_tree(pml4[USER_PML4_IDX] & ADDR_MASK, 3);
    pmm_free((uint64_t)pml4);                              /* общие записи ядра не трогаем */
}

/* Новая страница в пространстве pml4 по адресу va. 0 при неудаче (страница не утекает). */
static uint64_t map_new_page(uint64_t pml4, uint64_t va)
{
    uint64_t pa = alloc_zeroed();
    if (!pa) return 0;
    if (uspace_map(pml4, va, pa, P_RW | P_US)) { pmm_free(pa); return 0; }
    return pa;
}

struct proc *proc_create_user(const char *name, const void *code, size_t len, uint64_t arg)
{
    if (len > PAGE_SIZE) return 0;
    struct proc *p = proc_alloc(name, PF_USER);
    if (!p) return 0;
    p->cr3 = uspace_create();
    if (!p->cr3) { p->cr3 = 0; proc_release(p); return 0; }
    p->code_pa = map_new_page(p->cr3, USER_CODE_VA);
    uint64_t stack_pa = p->code_pa ? map_new_page(p->cr3, USER_STACK_TOP - PAGE_SIZE) : 0;
    if (!p->code_pa || !stack_pa) { proc_release(p); return 0; }
    memcpy((void *)p->code_pa, code, len);
    p->user_rip = USER_CODE_VA;
    p->user_rsp = USER_STACK_TOP;

    /* Стек ядра: [кадр iretq: rip cs rflags rsp ss] над [контекст ctx_switch], ret -> uthread_tramp. */
    uint64_t *sp = (uint64_t *)p->kstack_top;
    *--sp = SEL_UDATA;                  /* ss */
    *--sp = p->user_rsp;                /* rsp */
    *--sp = 0x202;                      /* rflags: IF=1, IOPL=0 */
    *--sp = SEL_UCODE;                  /* cs */
    *--sp = p->user_rip;                /* rip */
    *--sp = (uint64_t)uthread_tramp;
    *--sp = 0;                          /* rbp */
    *--sp = 0;                          /* rbx */
    *--sp = 0;                          /* r12 */
    *--sp = arg;                        /* r13 -> RDI в кольце 3 */
    *--sp = 0;                          /* r14 */
    *--sp = 0;                          /* r15 */
    p->ksp = (uint64_t)sp;
    sched_ready(p);
    return p;
}

/* Исключение из кольца 3. Если текущая задача — процесс: убиваем ТОЛЬКО его (в состоянии ZOMBIE, ресурсы
 * освободит sched_post_switch на другом стеке) и больше сюда не возвращаемся. Иначе возвращаемся в isr_dispatch (паника). */
void proc_handle_user_fault(struct regs *r, uint64_t cr2)
{
    struct proc *p = current;
    if (!p || !(p->flags & PF_USER)) return;
    p->tf = r;
    kprintf("[proc] pid %d '%s' killed: #%s (vec %lu, err=%#lx) rip=%#lx rsp=%#lx", p->pid, p->name,
            exc_name((unsigned)r->vector), r->vector, r->error, r->rip, r->rsp);
    if (r->vector == 14)
        kprintf(" cr2=%#lx [%s, %s, %s%s]", cr2, (r->error & 1) ? "protection" : "not-present",
                (r->error & 2) ? "write" : "read", (r->error & 4) ? "user" : "supervisor",
                (r->error & 16) ? ", ifetch" : "");
    kprintf("\n");
    sched_exit(-(int)r->vector);
}

/* ====================================================================================================
 * Самотест: make proctest N=1..5 (см. Makefile). Строки для консоли только ASCII: шрифт fbcon 8x16.
 * ==================================================================================================== */
#ifdef PROCTEST
#include <miga/time.h>
#include <miga/io.h>

/* Пользовательские блобы. Позиционно-независимые: данные внутри самого блоба, доступ через rip. */
__asm__(
    ".pushsection .rodata\n"
    ".balign 16\n"
    ".globl ublob_ud2_start, ublob_ud2_end\n"
    "ublob_ud2_start:\n"
    "    ud2\n"
    "ublob_ud2_end:\n"

    ".balign 16\n"
    ".globl ublob_loop_start, ublob_loop_end, ublob_loop_id, ublob_loop_ctr\n"
    "ublob_loop_start:\n"
    "    movq %rdi, ublob_loop_id(%rip)\n"      /* кто я: аргумент процесса */
    "1:  incq ublob_loop_ctr(%rip)\n"            /* работа: счётчик в СОБСТВЕННОЙ странице */
    "    jmp  1b\n"
    ".balign 8\n"
    "ublob_loop_id:  .quad 0\n"
    "ublob_loop_ctr: .quad 0\n"
    "ublob_loop_end:\n"

    ".balign 16\n"
    ".globl ublob_peek_start, ublob_peek_end\n"
    "ublob_peek_start:\n"
    "    movabsq $0x200000, %rax\n"              /* адрес образа ядра */
    "    movq (%rax), %rbx\n"                    /* чтение памяти ядра из кольца 3 => #PF */
    "    jmp  .\n"
    "ublob_peek_end:\n"

    ".balign 16\n"
    ".globl ublob_hlt_start, ublob_hlt_end\n"
    "ublob_hlt_start:\n"
    "    hlt\n"                                  /* привилегированная инструкция => #GP */
    "    jmp  .\n"
    "ublob_hlt_end:\n"
    ".popsection\n");

extern const char ublob_ud2_start[], ublob_ud2_end[];
extern const char ublob_loop_start[], ublob_loop_end[], ublob_loop_id[], ublob_loop_ctr[];
extern const char ublob_peek_start[], ublob_peek_end[];
extern const char ublob_hlt_start[], ublob_hlt_end[];

static void qemu_poweroff(void) { cpu_cli(); outw(0x604, 0x2000); for (;;) cpu_hlt(); }  /* ACPI PM1a q35 */

/* --- 1: GDT и TSS --- */
static void st_gdt(void)
{
    uint16_t tr;
    struct __attribute__((packed)) { uint16_t l; uint64_t b; } g;
    __asm__ volatile("str %0" : "=r"(tr));
    __asm__ volatile("sgdt %0" : "=m"(g));
    const uint64_t *d = (const uint64_t *)g.b;
    kprintf("[test1] TR=%#x (want 0x28), GDT limit=%u (want 55)\n", tr, g.l);
    for (int i = 0; i < 7; i++) kprintf("[test1]   gdt[%d] = %016lx\n", i, d[i]);
    kprintf("[test1] selectors: kcode=%#x kdata=%#x udata=%#x ucode=%#x tss=%#x\n",
            SEL_KCODE, SEL_KDATA, SEL_UDATA, SEL_UCODE, SEL_TSS);
    kprintf("[test1] SYSCALL/SYSRET for C4: STAR[47:32]=%#x STAR[63:48]=%#x -> sysret SS=%#x CS=%#x\n",
            SEL_KCODE, 0x10, 0x10 + 8 + 3, 0x10 + 16 + 3);
    kprintf("[test1] %s\n", (tr == SEL_TSS && g.l == 55) ? "PASS" : "FAIL");
}

/* --- 2: вход в кольцо 3 через iretq, затем намеренное #UD --- */
static void st_ring3(void)
{
    uint64_t pml4 = uspace_create();
    uint64_t code = pml4 ? alloc_zeroed() : 0, stack = code ? alloc_zeroed() : 0;
    uint64_t ks = stack ? pmm_alloc_contig(KSTACK_PAGES) : 0;
    if (!ks || uspace_map(pml4, USER_CODE_VA, code, P_RW | P_US) ||
        uspace_map(pml4, USER_STACK_TOP - PAGE_SIZE, stack, P_RW | P_US))
        panic("test2: out of memory");
    memcpy((void *)code, ublob_ud2_start, (size_t)(ublob_ud2_end - ublob_ud2_start));
    kprintf("[test2] iretq to ring 3: CS=%#x RIP=%#lx SS=%#x RSP=%#lx CR3=%#lx; expect #UD with CS=0x23\n",
            SEL_UCODE, (uint64_t)USER_CODE_VA, SEL_UDATA, (uint64_t)USER_STACK_TOP, pml4);
    gdt_set_rsp0(ks + KSTACK_SIZE);
    cpu_cli();
    cpu_write_cr3(pml4);
    __asm__ volatile("pushq %0\n\tpushq %1\n\tpushq %2\n\tpushq %3\n\tpushq %4\n\tiretq"
                     :: "r"((uint64_t)SEL_UDATA), "r"((uint64_t)USER_STACK_TOP), "r"(0x202ULL),
                        "r"((uint64_t)SEL_UCODE), "r"((uint64_t)USER_CODE_VA) : "memory");
    __builtin_unreachable();
}

/* --- 3: два потока ядра, переключение только через sched_yield --- */
static volatile int st3_done;
static void st3_thread(void *arg)
{
    for (int i = 0; i < 4; i++) {
        kprintf("[test3] thread %s step %d (pid %d)\n", (const char *)arg, i, current->pid);
        sched_yield();
    }
    st3_done++;
}

static void st_coop(void)
{
    uint64_t sw = sched_switches;
    kthread_create("A", st3_thread, "A", 0);
    kthread_create("B", st3_thread, "B", 0);
    while (st3_done < 2) sched_yield();
    kprintf("[test3] switches=%lu %s\n", sched_switches - sw, st3_done == 2 ? "PASS" : "FAIL");
}

/* --- 4: вытеснение по тику PIT, потоки не вызывают yield --- */
static volatile uint64_t st4_ctr[2];
static void st4_thread(void *arg) { volatile uint64_t *c = arg; for (;;) (*c)++; }

static void wait_ms(unsigned ms)
{
    uint64_t end = time_ticks() + ms;
    while (time_ticks() < end) cpu_idle();
}

static void st_preempt(void)
{
    cpu_sti();
    kthread_create("spinA", st4_thread, (void *)&st4_ctr[0], PF_KPREEMPT);
    kthread_create("spinB", st4_thread, (void *)&st4_ctr[1], PF_KPREEMPT);
    int ok = 1;
    for (int i = 0; i < 5; i++) {
        uint64_t a0 = st4_ctr[0], b0 = st4_ctr[1], s0 = sched_switches;
        wait_ms(100);
        kprintf("[test4] +100ms: A+=%lu B+=%lu switches+=%lu\n", st4_ctr[0] - a0, st4_ctr[1] - b0, sched_switches - s0);
        if (st4_ctr[0] == a0 || st4_ctr[1] == b0) ok = 0;
    }
    kprintf("[test4] %s\n", ok ? "PASS" : "FAIL");
}

/* --- 5: процессы кольца 3 со своим CR3; нарушитель убивается, остальные живут --- */
static uint64_t rd(const struct proc *p, const char *sym) { return *(volatile uint64_t *)(p->code_pa + (uint64_t)(sym - ublob_loop_start)); }

static void st_user(void)
{
    cpu_sti();
    struct proc *a = proc_create_user("A", ublob_loop_start, (size_t)(ublob_loop_end - ublob_loop_start), 0xA);
    struct proc *b = proc_create_user("B", ublob_loop_start, (size_t)(ublob_loop_end - ublob_loop_start), 0xB);
    struct proc *c = proc_create_user("peek-kernel", ublob_peek_start, (size_t)(ublob_peek_end - ublob_peek_start), 0);
    struct proc *d = proc_create_user("hlt", ublob_hlt_start, (size_t)(ublob_hlt_end - ublob_hlt_start), 0);
    if (!a || !b || !c || !d) panic("test5: cannot create processes");
    int pc = c->pid, pd = d->pid;
    kprintf("[test5] pids A=%d B=%d peek=%d hlt=%d; CR3 A=%#lx B=%#lx (user VA %#lx in both)\n",
            a->pid, b->pid, pc, pd, a->cr3, b->cr3, (uint64_t)USER_CODE_VA);

    int ok = a->cr3 != b->cr3 && a->code_pa != b->code_pa;
    for (int i = 0; i < 5; i++) {
        uint64_t ca = rd(a, ublob_loop_ctr), cb = rd(b, ublob_loop_ctr), s0 = sched_switches;
        wait_ms(100);
        uint64_t na = rd(a, ublob_loop_ctr), nb = rd(b, ublob_loop_ctr);
        kprintf("[test5] +100ms: A ctr+=%lu B ctr+=%lu switches+=%lu\n", na - ca, nb - cb, sched_switches - s0);
        if (na == ca || nb == cb) ok = 0;
    }
    uint64_t ida = rd(a, ublob_loop_id), idb = rd(b, ublob_loop_id);
    kprintf("[test5] same VA, different memory: A.id=%#lx B.id=%#lx\n", ida, idb);
    if (ida != 0xA || idb != 0xB) ok = 0;
    if (c->pid == pc || d->pid == pd) { /* слот ещё занят тем же pid => не убит */
        kprintf("[test5] violator was not killed\n");
        ok = 0;
    }
    kprintf("[test5] violators killed, system alive: A state=%d B state=%d (want %d = RUNNING/RUNNABLE)\n",
            a->state, b->state, PROC_RUNNABLE);
    kprintf("[test5] %s\n", ok ? "PASS" : "FAIL");
}

void proc_selftest(int stage)
{
    st_gdt();
    switch (stage) {
    case 2: st_ring3(); break;
    case 3: st_coop(); break;
    case 4: st_preempt(); break;
    case 5: st_user(); break;
    default: break;
    }
    kprintf("[proctest] done, powering off\n");
    qemu_poweroff();
}
#endif
