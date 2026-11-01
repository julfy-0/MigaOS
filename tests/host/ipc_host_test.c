/* Хост-тест логики C4: настоящие kernel/core/{ipc,caps,syscall}.c + моки планировщика/PMM.
 * User-память — реальный mmap по USER_BASE; таблицы страниц — настоящие 4-уровневые (phys == host-указатель).
 * Блокировка моделируется: sched_block() вызывает hook, который делает то, что сделал бы «другой процесс». */
#define _GNU_SOURCE
#include <kernel/kernel.h>
#include <kernel/proc.h>
#include <kernel/syscall.h>
#include <miga/irq.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); g_checks++; \
    if (_a != _b) { g_fail++; printf("  FAIL line %d: %s == %lld, want %lld\n", __LINE__, #a, _a, _b); } } while (0)

/* ---------------- моки ядра ---------------- */
struct proc *current;
static char logbuf[512];
void kprintf(const char *f, ...) { va_list a; va_start(a, f); vsnprintf(logbuf, sizeof logbuf, f, a); va_end(a); }
void panic(const char *f, ...) { printf("PANIC: %s\n", f); abort(); }

static long pmm_live, pmm_allocs, pmm_frees;
uint64_t pmm_alloc(void) { void *p = aligned_alloc(4096, 4096); memset(p, 0xA5, 4096); pmm_live++; pmm_allocs++; return (uint64_t)p; }
void pmm_free(uint64_t pa) { pmm_live--; pmm_frees++; free((void *)pa); }
uint64_t pmm_alloc_contig(uint64_t n) { (void)n; return 0; }

static uint64_t *kpml4;
uint64_t sched_kernel_cr3(void) { return (uint64_t)kpml4; }
uint64_t mock_cr3(void) { return 0; }
static int tlb_flushes;
void mock_flush(uint64_t c) { (void)c; tlb_flushes++; }
void syscall_arch_init(void) {}

static uint32_t io_last_port, io_last_val; static int io_last_w, io_ops;
uint8_t mock_inb(uint16_t p) { io_ops++; return (uint8_t)(p ^ 0x5A); }
uint16_t mock_inw(uint16_t p) { io_ops++; return p; }
uint32_t mock_inl(uint16_t p) { io_ops++; return 0x80000000u | p; }
void mock_out(uint16_t p, uint32_t v, int w) { io_ops++; io_last_port = p; io_last_val = v; io_last_w = w; }

static irq_handler_t irq_h[16]; static void *irq_c[16];
int irq_register(uint8_t irq, irq_handler_t fn, void *ctx) { if (irq >= 16 || irq_h[irq]) return -1; irq_h[irq] = fn; irq_c[irq] = ctx; return 0; }

static jmp_buf exit_jmp; static int exited, exit_code_seen;
static int wakes;
void sched_wake(struct proc *p) { if (p->state == PROC_BLOCKED) { p->state = PROC_RUNNABLE; wakes++; } }
void sched_yield(void) {}
static void (*block_hook)(struct proc *);
static int blocks;
void sched_block(void)
{
    struct proc *me = current;
    blocks++;
    me->state = PROC_BLOCKED;
    if (!block_hook) { printf("  FAIL: sched_block without hook (deadlock)\n"); g_fail++; me->state = PROC_RUNNABLE; return; }
    void (*h)(struct proc *) = block_hook;
    block_hook = 0;                         /* одноразовый: что сделали бы остальные, пока мы спали */
    h(me);
    current = me;
    me->state = PROC_RUNNING;
}
void sched_exit(int code) { ipc_proc_exit(current); exited = 1; exit_code_seen = code; longjmp(exit_jmp, 1); }

static int next_pid = 100; static struct proc *last_created;
struct proc *proc_create_user(const char *name, const void *code, size_t len, uint64_t arg)
{
    (void)code; (void)len;
    struct proc *c = calloc(1, sizeof *c);
    c->pid = next_pid++; strncpy(c->name, name, 15); c->flags = PF_USER; c->state = PROC_RUNNABLE;
    c->user_rsp = arg;                       /* для проверки передачи arg */
    last_created = c;
    return c;
}

/* ---------------- user-память и таблицы страниц ---------------- */
#define PTE_P 1ULL
#define PTE_RW 2ULL
#define PTE_US 4ULL
#define MASK 0x000FFFFFFFFFF000ULL
static uint64_t *tzalloc(void) { uint64_t *t = aligned_alloc(4096, 4096); memset(t, 0, 4096); return t; }

static void map_page(struct proc *p, uint64_t va, uint64_t pa, uint64_t leaf_flags, uint64_t dir_flags)
{
    uint64_t *t = (uint64_t *)p->cr3;
    for (int l = 3; l >= 1; l--) {
        uint64_t *e = &t[(va >> (12 + 9 * l)) & 511];
        if (!(*e & PTE_P)) *e = (uint64_t)tzalloc() | dir_flags;
        t = (uint64_t *)(*e & MASK);
    }
    t[(va >> 12) & 511] = pa | leaf_flags;
}
static uint64_t pte_of(struct proc *p, uint64_t va)         /* 0 если нет */
{
    uint64_t *t = (uint64_t *)p->cr3;
    for (int l = 3; l >= 1; l--) { uint64_t e = t[(va >> (12 + 9 * l)) & 511]; if (!(e & PTE_P)) return 0; t = (uint64_t *)(e & MASK); }
    return t[(va >> 12) & 511];
}
static struct proc *mkproc(int pid, uint32_t caps)
{
    struct proc *p = calloc(1, sizeof *p);
    uint64_t *pml4 = tzalloc();
    memcpy(pml4, kpml4, 4096);
    p->pid = pid; p->flags = PF_USER; p->caps = caps; p->cr3 = (uint64_t)pml4; p->state = PROC_RUNNING;
    snprintf(p->name, sizeof p->name, "t%d", pid);
    for (int i = 0; i < 4; i++) map_page(p, USER_BASE + i * 4096ULL, USER_BASE + i * 4096ULL, PTE_P | PTE_RW | PTE_US, PTE_P | PTE_RW | PTE_US);
    map_page(p, USER_BASE + 0x4000, USER_BASE + 0x4000, PTE_P | PTE_US, PTE_P | PTE_RW | PTE_US);       /* read-only */
    map_page(p, 0x200000, 0x200000, PTE_P | PTE_RW, PTE_P | PTE_RW);                                    /* «ядро»: без U/S */
    return p;
}
static void free_proc(struct proc *p) { free(p); }          /* таблицы страниц теста не освобождаем: это не предмет проверки */

#define UM ((uint8_t *)USER_BASE)
static int64_t sc(struct proc *p, uint64_t nr, uint64_t a, uint64_t b, uint64_t c)
{
    current = p;
    struct syscall_frame f; memset(&f, 0, sizeof f);
    f.rax = nr; f.rdi = a; f.rsi = b; f.rdx = c;
    syscall_dispatch(&f);
    return (int64_t)f.rax;
}
static struct miga_msg *M(unsigned off) { return (struct miga_msg *)(UM + off); }

/* ---------------- тесты ---------------- */
static void test_caps(void)
{
    puts("caps");
    struct proc *u = mkproc(1, CAP_SPAWN | CAP_IRQ), *k = calloc(1, sizeof *k);
    EQ(caps_check(u, CAP_SPAWN), 0);
    EQ(caps_check(u, CAP_IO_PORT), MIGA_EPERM);
    EQ(caps_check(u, CAP_SPAWN | CAP_IO_PORT), MIGA_EPERM);        /* нужны ВСЕ биты */
    EQ(caps_check(k, CAP_ALL), 0);                                  /* ядерные потоки доверенные */
    EQ(caps_check(0, 0), MIGA_EPERM);
    CHECK(caps_subset(CAP_IRQ, CAP_IRQ | CAP_SPAWN)); CHECK(!caps_subset(CAP_MMIO, CAP_IRQ)); CHECK(caps_subset(0, 0));
    CHECK(!caps_subset(0x100, CAP_ALL));                            /* неизвестные биты — не подмножество */
    char b[32]; CHECK(!strcmp(caps_str(CAP_IO_PORT | CAP_SPAWN, b, sizeof b), "io,spawn")); CHECK(!strcmp(caps_str(0, b, sizeof b), "-"));
    free_proc(u); free(k);
}

static void test_userptr(void)
{
    puts("user pointers");
    struct proc *p = mkproc(1, 0);
    EQ(user_range_ok(p, USER_BASE, 4096, 0), 0);
    EQ(user_range_ok(p, USER_BASE + 4095, 2, 1), 0);                  /* через границу страниц, обе RW */
    EQ(user_range_ok(p, USER_BASE + 4 * 4096 - 1, 2, 0), 0);          /* RW страница + RO страница, чтение */
    EQ(user_range_ok(p, USER_BASE + 4 * 4096 - 1, 2, 1), MIGA_EFAULT);/* RO страница, запись */
    EQ(user_range_ok(p, USER_BASE + 0x4000, 8, 1), MIGA_EFAULT);
    EQ(user_range_ok(p, USER_BASE + 0x4000, 8, 0), 0);
    EQ(user_range_ok(p, USER_BASE + 0x5000, 1, 0), MIGA_EFAULT);       /* не отображено */
    EQ(user_range_ok(p, USER_BASE + 3 * 4096, 2 * 4096, 0), 0);        /* RW + RO: читать можно */
    EQ(user_range_ok(p, USER_BASE + 3 * 4096, 2 * 4096, 1), MIGA_EFAULT); /* ...а писать нельзя из-за RO-страницы */
    EQ(user_range_ok(p, USER_BASE + 4 * 4096, 2 * 4096, 0), MIGA_EFAULT); /* RO + дальше дыра */
    EQ(user_range_ok(p, 0x200000, 8, 0), MIGA_EFAULT);                /* страница есть, но U/S=0 */
    EQ(user_range_ok(p, 0xFFFF800000200000ULL, 8, 0), MIGA_EFAULT);   /* ядро / неканонический */
    EQ(user_range_ok(p, 0x0000800000000000ULL, 1, 0), MIGA_EFAULT);
    EQ(user_range_ok(p, ~0ULL - 3, 16, 0), MIGA_EFAULT);              /* переполнение va + len */
    EQ(user_range_ok(p, USER_BASE, 1ULL << 40, 0), MIGA_EFAULT);      /* гигантская длина: быстрый отказ */
    EQ(user_range_ok(p, 0, 0, 0), 0);                                 /* пустой диапазон */
    uint8_t k[8] = {1,2,3,4,5,6,7,8};
    EQ(copy_to_user(p, USER_BASE + 0x4000, k, 8), MIGA_EFAULT);       /* в RO нельзя, и ничего не записано */
    memset(UM + 0x100, 0, 8);
    EQ(copy_to_user(p, USER_BASE + 0x100, k, 8), 0); CHECK(!memcmp(UM + 0x100, k, 8));
    uint8_t o[8]; EQ(copy_from_user(p, o, USER_BASE + 0x100, 8), 0); CHECK(!memcmp(o, k, 8));
    EQ(copy_from_user(p, o, 0xFFFF800000200000ULL, 8), MIGA_EFAULT);
    free_proc(p);
}

static void test_log_spawn_priv(void)
{
    puts("log / spawn / ioport / mmio");
    struct proc *none = mkproc(1, 0), *full = mkproc(2, CAP_ALL), *sp = mkproc(3, CAP_SPAWN | CAP_IRQ);

    memcpy(UM, "hi\x1b[31m\nX", 9); logbuf[0] = 0;
    EQ(sc(none, SYS_LOG, USER_BASE, 9, 0), 9);
    CHECK(strstr(logbuf, "hi?[31m?X")); CHECK(!strchr(logbuf, 0x1b));        /* управляющие символы вычищены */
    memcpy(UM, "%s%s%n", 6); EQ(sc(none, SYS_LOG, USER_BASE, 6, 0), 6); CHECK(strstr(logbuf, "%s%s%n")); /* не формат */
    EQ(sc(none, SYS_LOG, USER_BASE, 257, 0), MIGA_EINVAL);
    EQ(sc(none, SYS_LOG, 0xFFFF800000200000ULL, 4, 0), MIGA_EFAULT);
    EQ(sc(none, SYS_LOG, USER_BASE + 0x5000, 4, 0), MIGA_EFAULT);
    EQ(sc(none, SYS_LOG, USER_BASE, 0, 0), 0);

    io_ops = 0;
    EQ(sc(none, SYS_IOPORT, IOPORT_IN8, 0x80, 0), MIGA_EPERM);
    EQ(sc(none, SYS_IOPORT, IOPORT_OUT8, 0x80, 1), MIGA_EPERM);
    EQ(io_ops, 0);                                                    /* ни одной реальной операции: отказ ДО in/out */
    EQ(sc(none, SYS_MMIO_MAP, 0xFEE00000ULL, 4096, 0), MIGA_EPERM);
    EQ(none->mmio_brk, 0);
    EQ(sc(full, SYS_IOPORT, IOPORT_IN8, 0x80, 0), 0x80 ^ 0x5A);
    EQ(sc(full, SYS_IOPORT, IOPORT_IN16, 0x1234, 0), 0x1234);
    EQ(sc(full, SYS_IOPORT, IOPORT_IN32, 0x10, 0), 0x80000010LL);     /* без знака: не путается с ошибкой */
    EQ(sc(full, SYS_IOPORT, IOPORT_OUT16, 0x604, 0x2000), 0); EQ(io_last_port, 0x604); EQ(io_last_val, 0x2000); EQ(io_last_w, 2);
    EQ(sc(full, SYS_IOPORT, IOPORT_OUT8, 0x3F8, 0x1FF), 0); EQ(io_last_val, 0xFF);   /* усечено до 8 бит */
    EQ(sc(full, SYS_IOPORT, 99, 0x80, 0), MIGA_EINVAL);
    EQ(sc(full, SYS_IOPORT, IOPORT_IN8, 0x10000, 0), MIGA_EINVAL);

    /* MMIO: карта памяти — usable [0x100000, 0x40000000), mmio [0xFEC00000, +0x1400000) */
    int64_t va = sc(full, SYS_MMIO_MAP, 0xFEE00004ULL, 100, 0);
    CHECK(va > 0 && (uint64_t)va == USER_MMIO_BASE + 4);                              /* смещение внутри страницы сохранено */
    uint64_t pte = pte_of(full, USER_MMIO_BASE);
    CHECK((pte & MASK) == 0xFEE00000ULL); CHECK((pte & (PTE_P | PTE_RW | PTE_US)) == (PTE_P | PTE_RW | PTE_US)); CHECK(pte & 0x10); CHECK(pte & 0x8); /* PCD|PWT */
    CHECK(user_range_ok(full, (uint64_t)va, 100, 1) == 0);
    int64_t va2 = sc(full, SYS_MMIO_MAP, 0xE0000000ULL, 8192, 0);                    /* BAR вне карты UEFI: можно */
    CHECK((uint64_t)va2 == USER_MMIO_BASE + 4096);
    EQ(sc(full, SYS_MMIO_MAP, 0x200000, 4096, 0), MIGA_EPERM);                        /* RAM/ядро */
    EQ(sc(full, SYS_MMIO_MAP, 0xFF000, 0x2000, 0), MIGA_EPERM);                       /* хвост окна задевает usable */
    CHECK(sc(full, SYS_MMIO_MAP, 0xFEBFF000ULL, 0x2000, 0) > 0);                      /* карта UEFI это место не описывает: BAR-подобное, можно */
    EQ(sc(full, SYS_MMIO_MAP, 0xFEE00000ULL, 0, 0), MIGA_EINVAL);
    EQ(sc(full, SYS_MMIO_MAP, 0xFEE00000ULL, MIGA_MMIO_MAX + 1ULL, 0), MIGA_EINVAL);
    EQ(sc(full, SYS_MMIO_MAP, 0xFEE00000ULL, 4096, 1), MIGA_EINVAL);
    EQ(sc(full, SYS_MMIO_MAP, ~0ULL - 100, 4096, 0), MIGA_EPERM);                     /* переполнение */

    /* spawn */
    static const char code[16] = "code";
    EQ(image_register(1, code, sizeof code), 0); EQ(image_register(MAX_IMAGES, code, 4), MIGA_EINVAL); EQ(image_register(2, code, 0), MIGA_EINVAL);
    struct miga_spawn_args a = {1, 0, 0x77};
    memcpy(UM + 0x200, &a, sizeof a);
    EQ(sc(none, SYS_SPAWN, USER_BASE + 0x200, 0, 0), MIGA_EPERM);                     /* нет CAP_SPAWN */
    EQ(sc(sp, SYS_SPAWN, 0xFFFF800000200000ULL, 0, 0), MIGA_EFAULT);
    last_created = 0;
    int64_t pid = sc(sp, SYS_SPAWN, USER_BASE + 0x200, 0, 0);
    CHECK(pid >= 100 && last_created && last_created->pid == pid); EQ(last_created->caps, 0); EQ(last_created->user_rsp, 0x77);
    a.caps = CAP_IRQ; memcpy(UM + 0x200, &a, sizeof a);
    pid = sc(sp, SYS_SPAWN, USER_BASE + 0x200, 0, 0); CHECK(pid > 0); EQ(last_created->caps, CAP_IRQ);            /* подмножество — можно */
    a.caps = CAP_IRQ | CAP_SPAWN; memcpy(UM + 0x200, &a, sizeof a);
    CHECK(sc(sp, SYS_SPAWN, USER_BASE + 0x200, 0, 0) > 0);                            /* равные права — можно */
    struct proc *before = last_created;
    a.caps = CAP_IRQ | CAP_IO_PORT; memcpy(UM + 0x200, &a, sizeof a);
    EQ(sc(sp, SYS_SPAWN, USER_BASE + 0x200, 0, 0), MIGA_EPERM); CHECK(last_created == before);   /* эскалация: процесс НЕ создан */
    a.caps = 0xFFFFFFFF; memcpy(UM + 0x200, &a, sizeof a);
    EQ(sc(full, SYS_SPAWN, USER_BASE + 0x200, 0, 0), MIGA_EPERM);                     /* неизвестные биты тоже запрещены */
    a.caps = 0; a.image_id = 5; memcpy(UM + 0x200, &a, sizeof a);
    EQ(sc(full, SYS_SPAWN, USER_BASE + 0x200, 0, 0), MIGA_ENOENT);

    EQ(sc(none, 9999, 0, 0, 0), MIGA_ENOSYS); EQ(sc(none, SYS_MAX, 0, 0, 0), MIGA_ENOSYS);
    EQ(sc(none, SYS_YIELD, 0, 0, 0), 0);
    long live = pmm_live;
    current = full; if (!setjmp(exit_jmp)) sc(full, SYS_EXIT, 0, 0, 0); exited = 0;
    EQ(live - pmm_live, 3);                                                                                 /* таблицы ветки MMIO (PDPT, PD, PT) возвращены */
    free_proc(none); free_proc(full); free_proc(sp);
}

/* --- блокирующие сценарии --- */
static struct proc *peer; static uint64_t peer_port;
static void hook_send_to(struct proc *sleeper)                 /* пока получатель спит, peer шлёт сообщение */
{
    (void)sleeper; memset(UM + 0x300, 0, 64); M(0x300)->tag = 0xBEEF; M(0x300)->len = 2; M(0x300)->data[0] = 'o'; M(0x300)->data[1] = 'k';
    M(0x300)->sender = 31337;                                    /* подделка: ядро обязано перезаписать */
    current = peer; EQ(sc(peer, SYS_SEND, peer_port, USER_BASE + 0x300, 0), 0);
}
static void hook_drain(struct proc *sleeper)                    /* пока отправитель спит, владелец читает */
{
    (void)sleeper; current = peer; CHECK(sc(peer, SYS_RECV, peer_port, USER_BASE + 0x400, MIGA_NONBLOCK) >= 0);
}
static void hook_owner_exit(struct proc *sleeper)                /* владелец порта умирает, пока клиент спит в SEND */
{
    (void)sleeper; current = peer;
    if (!setjmp(exit_jmp)) sc(peer, SYS_EXIT, 9, 0, 0);
    exited = 0;
}
static void hook_close(struct proc *sleeper) { (void)sleeper; current = peer; EQ(sc(peer, SYS_PORT_CLOSE, peer_port, 0, 0), 0); }

static void test_ports(void)
{
    puts("ports / messages");
    struct proc *a = mkproc(10, 0), *b = mkproc(11, 0);        /* ни у кого нет прав — IPC от них не зависит */
    int64_t port = sc(a, SYS_PORT_CREATE, 0, 0, 0); CHECK(port > 0);
    EQ(sc(a, SYS_PORT_CREATE, 1, 0, 0), MIGA_EINVAL);

    /* FIFO + отправитель проставляет ядро */
    for (int i = 0; i < 3; i++) { memset(M(0), 0, 64); M(0)->tag = 100 + i; M(0)->len = 1; M(0)->data[0] = (uint8_t)i; M(0)->sender = 777; M(0)->reserved = 9; EQ(sc(b, SYS_SEND, port, USER_BASE, 0), 0); }
    for (int i = 0; i < 3; i++) {
        memset(M(0x100), 0xEE, 64);
        EQ(sc(a, SYS_RECV, port, USER_BASE + 0x100, 0), 1);
        EQ(M(0x100)->tag, 100 + i); EQ(M(0x100)->sender, 11); EQ(M(0x100)->reserved, 0); EQ(M(0x100)->data[0], i);
    }
    EQ(sc(a, SYS_RECV, port, USER_BASE + 0x100, MIGA_NONBLOCK), MIGA_EAGAIN);

    /* проверки аргументов */
    M(0)->len = 49; EQ(sc(b, SYS_SEND, port, USER_BASE, 0), MIGA_EINVAL); M(0)->len = 48; EQ(sc(b, SYS_SEND, port, USER_BASE, 0), 0);
    EQ(sc(a, SYS_RECV, port, USER_BASE + 0x100, 0), 48);
    EQ(sc(b, SYS_SEND, port, 0xFFFF800000200000ULL, 0), MIGA_EFAULT);
    EQ(sc(b, SYS_SEND, port, USER_BASE + 0x5000, 0), MIGA_EFAULT);
    M(0)->len = 3; EQ(sc(b, SYS_SEND, port, USER_BASE + 4096 * 4 - 32, 0), 0);   /* 64 байта на стыке RW- и RO-страниц: читаются, отправка допустима */
    EQ(sc(a, SYS_RECV, port, USER_BASE + 0x100, MIGA_NONBLOCK), (int64_t)((struct miga_msg *)(UM + 4096 * 4 - 32))->len);
    EQ(sc(b, SYS_SEND, port, USER_BASE, 4), MIGA_EINVAL);                  /* неизвестный флаг */
    EQ(sc(b, SYS_SEND, 0, USER_BASE, 0), MIGA_ENOENT); EQ(sc(b, SYS_SEND, 0x1FFFFFFFFULL, USER_BASE, 0), MIGA_ENOENT);
    EQ(sc(b, SYS_SEND, (port & ~0xFFULL) | 31, USER_BASE, 0), MIGA_ENOENT);  /* свободный слот */

    /* права на порт */
    EQ(sc(b, SYS_RECV, port, USER_BASE + 0x100, MIGA_NONBLOCK), MIGA_EPERM);
    EQ(sc(b, SYS_PORT_CLOSE, port, 0, 0), MIGA_EPERM);

    /* recv: плохой буфер не съедает сообщение */
    M(0)->len = 1; M(0)->tag = 5; EQ(sc(b, SYS_SEND, port, USER_BASE, 0), 0);
    EQ(sc(a, SYS_RECV, port, 0xFFFF800000200000ULL, 0), MIGA_EFAULT);
    EQ(sc(a, SYS_RECV, port, USER_BASE + 0x4000, 0), MIGA_EFAULT);          /* RO-буфер */
    EQ(sc(a, SYS_RECV, port, USER_BASE + 0x100, 0), 1); EQ(M(0x100)->tag, 5);

    /* блокирующий recv: просыпаемся, когда пришло сообщение */
    peer = b; peer_port = port; blocks = 0; wakes = 0; block_hook = hook_send_to;
    EQ(sc(a, SYS_RECV, port, USER_BASE + 0x100, 0), 2);
    EQ(blocks, 1); EQ(wakes, 1); EQ(M(0x100)->tag, 0xBEEF); EQ(M(0x100)->sender, 11); /* подделку 31337 ядро затёрло */
    CHECK(!memcmp(M(0x100)->data, "ok", 2));

    /* очередь заполнена */
    for (int i = 0; i < MIGA_PORT_DEPTH; i++) EQ(sc(b, SYS_SEND, port, USER_BASE, MIGA_NONBLOCK), 0);
    EQ(sc(b, SYS_SEND, port, USER_BASE, MIGA_NONBLOCK), MIGA_EAGAIN);
    EQ(sc(a, SYS_SEND, port, USER_BASE, 0), MIGA_EAGAIN);                    /* владелец в свой полный порт: не спим вечно */
    EQ(blocks, 1);
    /* блокирующий send: просыпаемся, когда владелец прочитал */
    peer = a; block_hook = hook_drain; wakes = 0;
    EQ(sc(b, SYS_SEND, port, USER_BASE, 0), 0); EQ(blocks, 2); EQ(wakes, 1);
    /* блокирующий send, потом порт закрыли: EPIPE */
    peer = a; block_hook = hook_close;
    EQ(sc(b, SYS_SEND, port, USER_BASE, 0), MIGA_EPIPE);
    EQ(sc(b, SYS_SEND, port, USER_BASE, 0), MIGA_ENOENT);                     /* а теперь id просто мёртв */
    /* то же, но порт гибнет вместе с владельцем (выход процесса) */
    struct proc *own = mkproc(12, 0); int64_t pe = sc(own, SYS_PORT_CREATE, 0, 0, 0);
    for (int i = 0; i < MIGA_PORT_DEPTH; i++) EQ(sc(b, SYS_SEND, pe, USER_BASE, MIGA_NONBLOCK), 0);
    peer = own; peer_port = pe; block_hook = hook_owner_exit;
    EQ(sc(b, SYS_SEND, pe, USER_BASE, 0), MIGA_EPIPE);
    free_proc(own);

    /* переиспользование слота не оживляет старый id */
    int64_t port2 = sc(a, SYS_PORT_CREATE, 0, 0, 0);
    CHECK(port2 > 0 && port2 != port && (port2 & 0xFF) == (port & 0xFF));
    EQ(sc(b, SYS_SEND, port, USER_BASE, MIGA_NONBLOCK), MIGA_ENOENT);
    EQ(sc(b, SYS_SEND, port2, USER_BASE, MIGA_NONBLOCK), 0);
    EQ(sc(a, SYS_RECV, port, USER_BASE + 0x100, MIGA_NONBLOCK), MIGA_ENOENT);

    /* исчерпание портов */
    int64_t ids[MAX_PORTS]; int n = 0; int64_t r;
    while ((r = sc(a, SYS_PORT_CREATE, 0, 0, 0)) > 0) ids[n++] = r;
    EQ(r, MIGA_ENOMEM); for (int i = 0; i < n; i++) sc(a, SYS_PORT_CLOSE, ids[i], 0, 0);
    sc(a, SYS_PORT_CLOSE, port2, 0, 0);
    CHECK(sc(a, SYS_PORT_CREATE, 0, 0, 0) > 0);
    free_proc(a); free_proc(b);
}

static void test_shm(void)
{
    puts("shared memory");
    long live0 = pmm_live;
    struct proc *a = mkproc(20, 0), *b = mkproc(21, 0), *c = mkproc(22, 0);
    long base = pmm_live;
    EQ(sc(a, SYS_SHM_CREATE, 0, 0, 0), MIGA_EINVAL); EQ(sc(a, SYS_SHM_CREATE, MIGA_SHM_MAX + 1, 0, 0), MIGA_EINVAL);
    int64_t id = sc(a, SYS_SHM_CREATE, 5000, 0, 0); CHECK(id > 0); EQ(pmm_live - base, 2);                  /* 5000 байт = 2 страницы */
    EQ(sc(b, SYS_SHM_MAP, id, 0, 0), MIGA_EPERM);                                                          /* не создатель, нет grant */
    EQ(sc(b, SYS_SHM_GRANT, id, 21, 0), MIGA_EPERM);                                                       /* grant только создатель */
    EQ(sc(a, SYS_SHM_GRANT, id, 20, 0), MIGA_EINVAL); EQ(sc(a, SYS_SHM_GRANT, id, 0, 0), MIGA_EINVAL);
    EQ(sc(a, SYS_SHM_GRANT, id, 21, 0), 0); EQ(sc(a, SYS_SHM_GRANT, id, 21, 0), 0);
    EQ(sc(c, SYS_SHM_MAP, id, 0, 0), MIGA_EPERM);                                                          /* третий — всё ещё нет */

    int64_t va_a = sc(a, SYS_SHM_MAP, id, 0, 0), va_b = sc(b, SYS_SHM_MAP, id, 0, MIGA_SHM_RO);
    EQ(va_a, USER_SHM_BASE); EQ(va_b, USER_SHM_BASE);
    uint64_t pa0 = pte_of(a, va_a) & MASK, pb0 = pte_of(b, va_b) & MASK;
    CHECK(pa0 && pa0 == pb0);                                                                              /* одна и та же физическая страница */
    CHECK((pte_of(a, va_a) & (PTE_P | PTE_RW | PTE_US)) == (PTE_P | PTE_RW | PTE_US));
    CHECK((pte_of(b, va_b) & (PTE_P | PTE_US)) == (PTE_P | PTE_US) && !(pte_of(b, va_b) & PTE_RW));        /* RO у b */
    CHECK((pte_of(a, va_a + 4096) & MASK) == (pte_of(b, va_b + 4096) & MASK) && (pte_of(a, va_a + 4096) & MASK) != pa0);
    EQ(user_range_ok(a, va_a, 8192, 1), 0); EQ(user_range_ok(b, va_b, 8192, 0), 0); EQ(user_range_ok(b, va_b, 8, 1), MIGA_EFAULT);
    EQ(pte_of(a, USER_BASE + 0x100000), 0);                                                                 /* ветка [128] не тронута */
    ((uint8_t *)pa0)[7] = 0x42; CHECK(((uint8_t *)pb0)[7] == 0x42);                                         /* данные видны обоим */
    EQ(((uint8_t *)pa0)[100], 0);                                                                           /* страница обнулена, а не 0xA5 из аллокатора */
    EQ(sc(a, SYS_SHM_MAP, id, 0, 0), MIGA_EINVAL);                                                          /* повторное отображение */
    EQ(sc(a, SYS_SHM_MAP, id, 0, 8), MIGA_EINVAL);                                                          /* неизвестный флаг */

    /* hint */
    int64_t id2 = sc(a, SYS_SHM_CREATE, 4096, 0, 0);
    EQ(sc(a, SYS_SHM_MAP, id2, USER_SHM_BASE + 4096, 0), MIGA_EINVAL);                                      /* пересекается с первым объектом [0, 8192) */
    EQ(sc(a, SYS_SHM_MAP, id2, 0x1000, 0), MIGA_EINVAL);                                                    /* вне ветки */
    EQ(sc(a, SYS_SHM_MAP, id2, ~0ULL - 4095, 0), MIGA_EINVAL);                                              /* переполнение hint + len */
    EQ(sc(a, SYS_SHM_MAP, id2, USER_SHM_BASE + USER_BRANCH_SZ, 0), MIGA_EINVAL);
    EQ(sc(a, SYS_SHM_MAP, id2, USER_SHM_BASE + 123, 0), MIGA_EINVAL);
    EQ(sc(a, SYS_SHM_MAP, id2, USER_SHM_BASE + 0x10000, 0), USER_SHM_BASE + 0x10000);
    int64_t id3 = sc(a, SYS_SHM_CREATE, 4096, 0, 0);
    EQ(sc(a, SYS_SHM_MAP, id3, 0, 0), USER_SHM_BASE + 2 * 4096);                                            /* первое свободное место, не поверх чужого */

    /* лимиты */
    int64_t id4 = sc(a, SYS_SHM_CREATE, 1, 0, 0); CHECK(id4 > 0);
    EQ(sc(a, SYS_SHM_CREATE, 1, 0, 0), MIGA_ENOMEM);                                                        /* не больше 4 объектов на процесс */

    /* unmap */
    EQ(sc(b, SYS_SHM_UNMAP, va_b + 1, 0, 0), MIGA_EINVAL);
    tlb_flushes = 0; EQ(sc(b, SYS_SHM_UNMAP, va_b, 0, 0), 0); CHECK(tlb_flushes > 0);
    EQ(pte_of(b, va_b), 0); EQ(sc(b, SYS_SHM_UNMAP, va_b, 0, 0), MIGA_EINVAL);
    EQ(sc(b, SYS_SHM_MAP, id, 0, 0), (int64_t)USER_SHM_BASE);                                               /* можно отобразить снова */

    /* создатель вышел, b ещё держит: память жива */
    long used = pmm_live;
    current = a; if (!setjmp(exit_jmp)) sc(a, SYS_EXIT, 3, 0, 0);
    EQ(exited, 1); EQ(exit_code_seen, 3); exited = 0;
    CHECK(pmm_live < used);                                                                                 /* id2/id3/id4 и таблицы a освобождены */
    EQ(pmm_live - base, 2 + 3);                                                                             /* живы: 2 страницы id + 3 таблицы ветки у b */
    EQ(((uint8_t *)pb0)[7], 0x42);
    EQ(sc(b, SYS_SHM_MAP, id2, 0, 0), MIGA_ENOENT);
    /* последнее отображение закрыто -> освободить */
    current = b; if (!setjmp(exit_jmp)) sc(b, SYS_EXIT, 0, 0, 0);
    exited = 0;
    EQ(pmm_live, live0);                                                                                    /* всё возвращено: страницы shm и таблицы веток */
    free_proc(a); free_proc(b); free_proc(c);
}

static void test_irq(void)
{
    puts("irq attach");
    struct proc *n = mkproc(30, 0), *d = mkproc(31, CAP_IRQ);
    int64_t port = sc(d, SYS_PORT_CREATE, 0, 0, 0);
    EQ(sc(n, SYS_IRQ_ATTACH, 5, port, 0), MIGA_EPERM);
    EQ(sc(d, SYS_IRQ_ATTACH, 0, port, 0), MIGA_EINVAL); EQ(sc(d, SYS_IRQ_ATTACH, 2, port, 0), MIGA_EINVAL); EQ(sc(d, SYS_IRQ_ATTACH, 16, port, 0), MIGA_EINVAL);
    EQ(sc(d, SYS_IRQ_ATTACH, 5, 0, 0), MIGA_ENOENT);
    int64_t foreign = sc(n, SYS_PORT_CREATE, 0, 0, 0);
    EQ(sc(d, SYS_IRQ_ATTACH, 5, foreign, 0), MIGA_EPERM);                                                   /* чужой порт */
    irq_h[1] = (irq_handler_t)1;                                                                            /* «драйвер ядра» уже занял IRQ1 */
    EQ(sc(d, SYS_IRQ_ATTACH, 1, port, 0), MIGA_EBUSY);
    EQ(sc(d, SYS_IRQ_ATTACH, 5, port, 0), 0);
    EQ(sc(d, SYS_IRQ_ATTACH, 5, port, 0), MIGA_EBUSY);
    CHECK(irq_h[5]); irq_h[5](irq_c[5]); irq_h[5](irq_c[5]);
    EQ(sc(d, SYS_RECV, port, USER_BASE, MIGA_NONBLOCK), 4);
    EQ(M(0)->tag, MIGA_TAG_IRQ); EQ(M(0)->sender, 0); EQ(*(uint32_t *)M(0)->data, 5);
    for (int i = 0; i < 40; i++) irq_h[5](irq_c[5]);                                                        /* переполнение — не падаем */
    int got = 0; while (sc(d, SYS_RECV, port, USER_BASE, MIGA_NONBLOCK) == 4) got++;
    EQ(got, MIGA_PORT_DEPTH);
    current = d; if (!setjmp(exit_jmp)) sc(d, SYS_EXIT, 0, 0, 0); exited = 0;
    irq_h[5](irq_c[5]);                                                                                     /* владелец ушёл: молча */
    struct proc *d2 = mkproc(32, CAP_IRQ); int64_t p2 = sc(d2, SYS_PORT_CREATE, 0, 0, 0);
    EQ(sc(d2, SYS_IRQ_ATTACH, 5, p2, 0), 0);                                                                /* перепривязка после ухода владельца */
    irq_h[5](irq_c[5]); EQ(sc(d2, SYS_RECV, p2, USER_BASE, MIGA_NONBLOCK), 4);
    free_proc(n); free_proc(d); free_proc(d2);
}

static void test_exit_cleanup(void)
{
    puts("exit cleanup");
    struct proc *srv = mkproc(40, 0), *cli = mkproc(41, 0);
    int64_t port = sc(srv, SYS_PORT_CREATE, 0, 0, 0);
    for (int i = 0; i < MIGA_PORT_DEPTH; i++) sc(cli, SYS_SEND, port, USER_BASE, MIGA_NONBLOCK);
    current = srv; if (!setjmp(exit_jmp)) sc(srv, SYS_EXIT, 1, 0, 0); exited = 0;
    EQ(sc(cli, SYS_SEND, port, USER_BASE, MIGA_NONBLOCK), MIGA_ENOENT);
    long before = pmm_live;
    int64_t id = sc(cli, SYS_SHM_CREATE, 8192, 0, 0); CHECK(id > 0); CHECK(sc(cli, SYS_SHM_MAP, id, 0, 0) > 0);
    CHECK(pmm_live > before);
    current = cli; if (!setjmp(exit_jmp)) sc(cli, SYS_EXIT, 0, 0, 0); exited = 0;
    EQ(pmm_live, before - 0);                                                                               /* и страницы shm, и таблицы ветки [129] возвращены */
    EQ(((uint64_t *)cli->cr3)[(USER_SHM_BASE >> 39) & 511], 0);
    free_proc(srv); free_proc(cli);
}

int main(void)
{
    void *m = mmap((void *)USER_BASE, 16 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m != (void *)USER_BASE) { perror("mmap USER_BASE"); return 2; }
    kpml4 = tzalloc();
    kpml4[0] = (uint64_t)tzalloc() | PTE_P | PTE_RW;                       /* ядро: без U/S */

    static struct mem_region map[] = {
        {0x0,        0x9F000,     MEM_USABLE, 0}, {0x100000,   0x3FF00000, MEM_USABLE, 0},
        {0xFEC00000, 0x01400000,  MEM_MMIO, 0},
    };
    static struct boot_info bi = { .magic = MIGA_BOOT_MAGIC, .mem_count = 3, .mem = map };
    syscall_init(&bi);

    test_caps(); test_userptr(); test_log_spawn_priv(); test_ports(); test_shm(); test_irq(); test_exit_cleanup();
    EQ(pmm_live, 0);                                                                                        /* ни одной утечки страниц ядра за весь прогон */
    printf("\n%d checks, %d failed; pmm alloc=%ld free=%ld\n", g_checks, g_fail, pmm_allocs, pmm_frees);
    return g_fail ? 1 : 0;
}
