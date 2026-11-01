/* kernel/core/ipc.c — порты и сообщения, общая память, привязка IRQ, окна MMIO, очистка при выходе.
 *
 * Все функции вызываются с IF=0 (syscall-вход или обработчик IRQ), ядро одно: блокировок нет.
 * Блокировка — только sched_block(); условие всегда перепроверяется после пробуждения. */
#include <kernel/kernel.h>
#include <kernel/arch.h>
#include <kernel/proc.h>
#include <kernel/syscall.h>
#include <miga/irq.h>
#include <miga/string.h>

#define PTE_P   1ULL
#define PTE_RW  2ULL
#define PTE_US  4ULL
#define PTE_PWT 8ULL
#define PTE_PCD 0x10ULL
#define PTE_ADDR 0x000FFFFFFFFFF000ULL

/* ====================================================================================================
 * Порты
 * ==================================================================================================== */
struct port {
    int      used;
    uint32_t gen;                       /* растёт при каждом создании слота: устаревший id не попадёт в чужой порт */
    int      owner;                     /* pid */
    uint32_t head, count;
    struct miga_msg q[MIGA_PORT_DEPTH];
    struct proc *recv_waiter;           /* владелец спит в RECV (один: процесс однопоточный) */
    struct proc *send_waiters[MAX_PROCS];
    unsigned nsend;
    uint32_t irq_dropped;               /* сообщений IRQ потеряно из-за переполнения */
};
static struct port ports[MAX_PORTS];
static uint32_t port_gen;

static struct port *port_lookup(uint64_t id)
{
    if (id == 0 || id > 0xFFFFFFFFULL) return 0;
    uint32_t idx = (uint32_t)id & 0xFF, gen = (uint32_t)id >> 8;
    if (idx >= MAX_PORTS || !ports[idx].used || ports[idx].gen != gen) return 0;
    return &ports[idx];
}
static uint32_t port_id(const struct port *pt) { return (pt->gen << 8) | (uint32_t)(pt - ports); }

static int port_push(struct port *pt, const struct miga_msg *m)    /* 0 или MIGA_EAGAIN */
{
    if (pt->count == MIGA_PORT_DEPTH) return MIGA_EAGAIN;
    pt->q[(pt->head + pt->count) % MIGA_PORT_DEPTH] = *m;
    pt->count++;
    if (pt->recv_waiter) sched_wake(pt->recv_waiter);
    return 0;
}

static void wake_senders(struct port *pt)
{
    for (unsigned i = 0; i < pt->nsend; i++) sched_wake(pt->send_waiters[i]);
    pt->nsend = 0;                      /* проснувшийся, если место снова занято, встанет в очередь заново */
}

static void port_destroy(struct port *pt)
{
    pt->used = 0;                       /* до пробуждения: проснувшиеся увидят ENOENT -> EPIPE */
    wake_senders(pt);
    pt->count = 0;
    pt->recv_waiter = 0;
}

static int64_t sys_port_create(struct proc *p, uint64_t flags)
{
    if (flags) return MIGA_EINVAL;
    for (unsigned i = 0; i < MAX_PORTS; i++) {
        struct port *pt = &ports[i];
        if (pt->used) continue;
        memset(pt, 0, sizeof *pt);
        pt->used = 1;
        pt->gen = ++port_gen & 0xFFFFFF;
        if (!pt->gen) pt->gen = ++port_gen;     /* gen != 0, иначе id первого порта == 0 */
        pt->owner = p->pid;
        return port_id(pt);
    }
    return MIGA_ENOMEM;
}

static int64_t sys_port_close(struct proc *p, uint64_t id)
{
    struct port *pt = port_lookup(id);
    if (!pt) return MIGA_ENOENT;
    if (pt->owner != p->pid) return MIGA_EPERM;
    port_destroy(pt);
    return 0;
}

static int64_t sys_send(struct proc *p, uint64_t id, uint64_t umsg, uint64_t flags)
{
    struct miga_msg m;
    if (flags & ~(uint64_t)MIGA_NONBLOCK) return MIGA_EINVAL;
    int r = copy_from_user(p, &m, umsg, sizeof m);        /* сначала всё проверить, потом трогать очередь */
    if (r) return r;
    if (m.len > MIGA_MSG_DATA) return MIGA_EINVAL;
    m.sender = (uint32_t)p->pid;                          /* подделать отправителя нельзя */
    m.reserved = 0;

    int slept = 0;
    for (;;) {
        struct port *pt = port_lookup(id);
        if (!pt) return slept ? MIGA_EPIPE : MIGA_ENOENT;
        if (port_push(pt, &m) == 0) return 0;
        if ((flags & MIGA_NONBLOCK) || pt->owner == p->pid) return MIGA_EAGAIN;
        if (pt->nsend == MAX_PROCS) return MIGA_EAGAIN;   /* недостижимо при MAX_PROCS задач, но не переполняем массив */
        pt->send_waiters[pt->nsend++] = p;
        sched_block();
        slept = 1;
    }
}

static int64_t sys_recv(struct proc *p, uint64_t id, uint64_t umsg, uint64_t flags)
{
    if (flags & ~(uint64_t)MIGA_NONBLOCK) return MIGA_EINVAL;
    struct port *pt = port_lookup(id);
    if (!pt) return MIGA_ENOENT;
    if (pt->owner != p->pid) return MIGA_EPERM;
    int r = user_range_ok(p, umsg, sizeof(struct miga_msg), 1);   /* до извлечения: иначе сообщение пропадёт */
    if (r) return r;

    while (pt->count == 0) {
        if (flags & MIGA_NONBLOCK) return MIGA_EAGAIN;
        pt->recv_waiter = p;
        sched_block();
        pt->recv_waiter = 0;
        pt = port_lookup(id);                             /* закрыть может только владелец, то есть мы; но проверим */
        if (!pt) return MIGA_EPIPE;
    }
    struct miga_msg m = pt->q[pt->head];
    pt->head = (pt->head + 1) % MIGA_PORT_DEPTH;
    pt->count--;
    wake_senders(pt);
    m.reserved = 0;
    copy_to_user(p, umsg, &m, sizeof m);                  /* диапазон проверен выше, IF=0 */
    return m.len;
}

/* ====================================================================================================
 * Привязка IRQ к порту (CAP_IRQ)
 * ==================================================================================================== */
static uint32_t irq_port[16];           /* id порта-получателя; 0 = свободно */
static uint8_t  irq_hooked[16];         /* irq_register() уже вызван (снять обработчик API не умеет) */

static void irq_to_port(void *ctx)
{
    unsigned irq = (unsigned)(uintptr_t)ctx;
    struct port *pt = port_lookup(irq_port[irq]);
    if (!pt) return;                    /* владелец ушёл: гасим молча */
    struct miga_msg m;
    memset(&m, 0, sizeof m);
    m.tag = MIGA_TAG_IRQ;
    m.len = 4;
    m.sender = 0;                       /* от ядра */
    memcpy(m.data, &irq, 4);
    if (port_push(pt, &m)) pt->irq_dropped++;
}

static int64_t sys_irq_attach(struct proc *p, uint64_t irq, uint64_t port)
{
    int r = caps_check(p, CAP_IRQ);
    if (r) return r;
    if (irq >= 16 || irq == 0 || irq == 2) return MIGA_EINVAL;     /* 0 — тик планировщика, 2 — каскад PIC */
    struct port *pt = port_lookup(port);
    if (!pt) return MIGA_ENOENT;
    if (pt->owner != p->pid) return MIGA_EPERM;
    if (irq_port[irq] && port_lookup(irq_port[irq])) return MIGA_EBUSY;
    if (!irq_hooked[irq]) {
        if (irq_register((uint8_t)irq, irq_to_port, (void *)(uintptr_t)irq)) return MIGA_EBUSY;   /* занят драйвером ядра */
        irq_hooked[irq] = 1;
    }
    irq_port[irq] = port_id(pt);
    return 0;
}

/* ====================================================================================================
 * Таблицы страниц для веток SHM и MMIO (PML4[129], PML4[130])
 *
 * Эти ветки C3 не знает: uspace_destroy освобождает только дерево PML4[128] вместе с ЛИСТЬЯМИ, а наши листья
 * чужие (страницы shm, физические BAR). Поэтому ветки свои, а таблицы мы освобождаем сами в ipc_proc_exit.
 * ==================================================================================================== */
static int uvm_ok;                      /* ветки 129/130 в PML4 ядра пусты (иначе правка их разделила бы между процессами) */

static uint64_t *uvm_pte(struct proc *p, uint64_t va, int create)
{
    uint64_t br = (va >> 39) & 511;
    if (br != ((USER_SHM_BASE >> 39) & 511) && br != ((USER_MMIO_BASE >> 39) & 511)) return 0;   /* только наши ветки */
    uint64_t *t = (uint64_t *)(p->cr3 & PTE_ADDR);
    for (int level = 3; level >= 1; level--) {
        uint64_t *e = &t[(va >> (12 + 9 * level)) & 511];
        if (!(*e & PTE_P)) {
            if (!create) return 0;
            uint64_t pa = pmm_alloc();
            if (!pa) return 0;
            memset((void *)pa, 0, PAGE_SIZE);
            *e = pa | PTE_P | PTE_RW | PTE_US;    /* U/S на каждом уровне, как требует процессор */
        }
        t = (uint64_t *)(*e & PTE_ADDR);
    }
    return &t[(va >> 12) & 511];
}

static void tlb_flush(void) { cpu_write_cr3(cpu_read_cr3()); }

static void free_branch(uint64_t table, int level)        /* level: 3 = PDPT, 2 = PD, 1 = PT; листья не трогаем */
{
    if (level > 1) {
        const uint64_t *t = (const uint64_t *)table;
        for (int i = 0; i < 512; i++)
            if (t[i] & PTE_P) free_branch(t[i] & PTE_ADDR, level - 1);
    }
    pmm_free(table);
}

static void free_ext_branches(struct proc *p)
{
    if (!(p->flags & PF_USER) || !p->cr3 || p->cr3 == sched_kernel_cr3()) return;
    uint64_t *pml4 = (uint64_t *)(p->cr3 & PTE_ADDR);
    uint64_t idx[2] = {(USER_SHM_BASE >> 39) & 511, (USER_MMIO_BASE >> 39) & 511};
    for (int i = 0; i < 2; i++) {
        if (pml4[idx[i]] & PTE_P) free_branch(pml4[idx[i]] & PTE_ADDR, 3);
        pml4[idx[i]] = 0;
    }
}

/* ====================================================================================================
 * Общая память
 * ==================================================================================================== */
struct shm {
    int      used;
    uint32_t gen;
    int      creator;                   /* pid */
    int      creator_alive;
    uint32_t npages, refs;              /* refs — число отображений в процессах */
    uint64_t pages[SHM_MAX_PAGES];      /* физические страницы (phys == virt в ядре) */
    int      grants[8];                 /* pid, которым разрешено SHM_MAP */
    unsigned ngrants;
};
static struct shm shms[MAX_SHM];
static uint32_t shm_gen;
#define SHM_PER_PROC 4

static struct shm *shm_lookup(uint64_t id)
{
    if (id == 0 || id > 0xFFFFFFFFULL) return 0;
    uint32_t idx = (uint32_t)id & 0xFF, gen = (uint32_t)id >> 8;
    if (idx == 0 || idx > MAX_SHM || !shms[idx - 1].used || shms[idx - 1].gen != gen) return 0;   /* idx 1..MAX_SHM, не 0 */
    return &shms[idx - 1];
}
static uint32_t shm_id(const struct shm *s) { return (s->gen << 8) | (uint32_t)(s - shms + 1); }

static void shm_maybe_free(struct shm *s)
{
    if (s->creator_alive || s->refs) return;
    for (uint32_t i = 0; i < s->npages; i++) pmm_free(s->pages[i]);
    s->used = 0;
}

static int64_t sys_shm_create(struct proc *p, uint64_t size)
{
    if (size == 0 || size > MIGA_SHM_MAX) return MIGA_EINVAL;
    unsigned mine = 0;
    struct shm *s = 0;
    for (unsigned i = 0; i < MAX_SHM; i++) {
        if (shms[i].used) { if (shms[i].creator == p->pid) mine++; }
        else if (!s) s = &shms[i];
    }
    if (!s || mine >= SHM_PER_PROC) return MIGA_ENOMEM;
    uint32_t n = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    memset(s, 0, sizeof *s);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t pa = pmm_alloc();
        if (!pa) { for (uint32_t j = 0; j < i; j++) pmm_free(s->pages[j]); return MIGA_ENOMEM; }
        memset((void *)pa, 0, PAGE_SIZE);                 /* чужие данные из переиспользованной страницы не утекают */
        s->pages[i] = pa;
    }
    s->used = 1;
    s->gen = ++shm_gen & 0xFFFFFF;
    if (!s->gen) s->gen = ++shm_gen;
    s->creator = p->pid;
    s->creator_alive = 1;
    s->npages = n;
    return shm_id(s);
}

static int64_t sys_shm_grant(struct proc *p, uint64_t id, uint64_t pid)
{
    struct shm *s = shm_lookup(id);
    if (!s) return MIGA_ENOENT;
    if (s->creator != p->pid) return MIGA_EPERM;
    if (pid == 0 || pid > 0x7FFFFFFF || (int)pid == p->pid) return MIGA_EINVAL;
    for (unsigned i = 0; i < s->ngrants; i++) if (s->grants[i] == (int)pid) return 0;
    if (s->ngrants == sizeof s->grants / sizeof s->grants[0]) return MIGA_ENOMEM;
    s->grants[s->ngrants++] = (int)pid;
    return 0;
}

static int overlaps(const struct proc *p, uint64_t va, uint64_t len)
{
    for (int i = 0; i < PROC_SHM_MAPS; i++) {
        const struct proc_shm_map *m = &p->shm_map[i];
        if (m->shm && va < m->va + (uint64_t)m->pages * PAGE_SIZE && va + len > m->va) return 1;
    }
    return 0;
}

static int64_t sys_shm_map(struct proc *p, uint64_t id, uint64_t hint, uint64_t flags)
{
    if (!uvm_ok) return MIGA_ENOSYS;
    if (flags & ~(uint64_t)MIGA_SHM_RO) return MIGA_EINVAL;
    struct shm *s = shm_lookup(id);
    if (!s) return MIGA_ENOENT;
    int allowed = s->creator == p->pid;
    for (unsigned i = 0; !allowed && i < s->ngrants; i++) allowed = s->grants[i] == p->pid;
    if (!allowed) return MIGA_EPERM;

    struct proc_shm_map *slot = 0;
    for (int i = 0; i < PROC_SHM_MAPS; i++) {
        if (p->shm_map[i].shm == (uint32_t)id) return MIGA_EINVAL;     /* уже отображён */
        if (!p->shm_map[i].shm && !slot) slot = &p->shm_map[i];
    }
    if (!slot) return MIGA_ENOMEM;

    uint64_t len = (uint64_t)s->npages * PAGE_SIZE, va;
    if (hint) {
        if ((hint & 0xFFF) || hint < USER_SHM_BASE || hint >= USER_SHM_BASE + USER_BRANCH_SZ ||   /* hint + len не переполнится */
            hint + len > USER_SHM_BASE + USER_BRANCH_SZ || overlaps(p, hint, len))
            return MIGA_EINVAL;
        va = hint;
    } else {
        va = USER_SHM_BASE;             /* первое подходящее место; va строго растёт, цикл конечен */
        for (int again = 1; again;) {
            again = 0;
            for (int i = 0; i < PROC_SHM_MAPS; i++) {
                const struct proc_shm_map *m = &p->shm_map[i];
                uint64_t mend = m->va + (uint64_t)m->pages * PAGE_SIZE;
                if (m->shm && va < mend && va + len > m->va) { va = mend; again = 1; }
            }
        }
        if (va + len > USER_SHM_BASE + USER_BRANCH_SZ) return MIGA_ENOMEM;
    }

    uint64_t fl = PTE_P | PTE_US | ((flags & MIGA_SHM_RO) ? 0 : PTE_RW);
    for (uint32_t i = 0; i < s->npages; i++) {
        uint64_t *pte = uvm_pte(p, va + (uint64_t)i * PAGE_SIZE, 1);
        if (!pte) {
            for (uint32_t j = 0; j < i; j++) *uvm_pte(p, va + (uint64_t)j * PAGE_SIZE, 0) = 0;
            tlb_flush();
            return MIGA_ENOMEM;
        }
        *pte = s->pages[i] | fl;
    }
    slot->shm = (uint32_t)id;
    slot->pages = s->npages;
    slot->va = va;
    s->refs++;
    return (int64_t)va;
}

static void unmap_slot(struct proc *p, struct proc_shm_map *m, int clear_ptes)
{
    if (clear_ptes)
        for (uint32_t i = 0; i < m->pages; i++) {
            uint64_t *pte = uvm_pte(p, m->va + (uint64_t)i * PAGE_SIZE, 0);
            if (pte) *pte = 0;
        }
    struct shm *s = shm_lookup(m->shm);
    if (s && s->refs) { s->refs--; shm_maybe_free(s); }
    memset(m, 0, sizeof *m);
}

static int64_t sys_shm_unmap(struct proc *p, uint64_t va)
{
    for (int i = 0; i < PROC_SHM_MAPS; i++) {
        struct proc_shm_map *m = &p->shm_map[i];
        if (m->shm && m->va == va) { unmap_slot(p, m, 1); tlb_flush(); return 0; }
    }
    return MIGA_EINVAL;
}

/* ====================================================================================================
 * MMIO для драйверов в user space (CAP_MMIO проверил вызывающий)
 * ==================================================================================================== */
int ipc_mmio_map(struct proc *p, uint64_t phys, uint64_t size, uint64_t *va_out)
{
    if (!uvm_ok) return MIGA_ENOSYS;
    uint64_t off = phys & 0xFFF, npages = (off + size + 0xFFF) >> 12, base = phys & ~0xFFFULL;
    if (!mmio_range_allowed(base, npages * PAGE_SIZE)) return MIGA_EPERM;   /* окно задевает RAM/ядро/ACPI */
    uint64_t va = p->mmio_brk ? p->mmio_brk : USER_MMIO_BASE;
    if (va + npages * PAGE_SIZE > USER_MMIO_BASE + USER_BRANCH_SZ) return MIGA_ENOMEM;
    for (uint64_t i = 0; i < npages; i++) {
        uint64_t *pte = uvm_pte(p, va + i * PAGE_SIZE, 1);
        if (!pte) return MIGA_ENOMEM;                          /* уже созданные PTE безвредны: va не выдан */
        *pte = (base + i * PAGE_SIZE) | PTE_P | PTE_RW | PTE_US | PTE_PWT | PTE_PCD;   /* uncached */
    }
    p->mmio_brk = va + npages * PAGE_SIZE;
    *va_out = va + off;
    return 0;
}

/* ====================================================================================================
 * Вход из syscall и выход процесса
 * ==================================================================================================== */
int64_t ipc_syscall(struct proc *p, uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3)
{
    switch (nr) {
    case SYS_PORT_CREATE: return sys_port_create(p, a1);
    case SYS_PORT_CLOSE:  return sys_port_close(p, a1);
    case SYS_SEND:        return sys_send(p, a1, a2, a3);
    case SYS_RECV:        return sys_recv(p, a1, a2, a3);
    case SYS_SHM_CREATE:  return sys_shm_create(p, a1);
    case SYS_SHM_MAP:     return sys_shm_map(p, a1, a2, a3);
    case SYS_SHM_UNMAP:   return sys_shm_unmap(p, a1);
    case SYS_SHM_GRANT:   return sys_shm_grant(p, a1, a2);
    case SYS_IRQ_ATTACH:  return sys_irq_attach(p, a1, a2);
    default:              return MIGA_ENOSYS;
    }
}

void ipc_proc_exit(struct proc *p)
{
    if (!(p->flags & PF_USER)) return;
    for (unsigned i = 0; i < MAX_PORTS; i++)
        if (ports[i].used && ports[i].owner == p->pid) port_destroy(&ports[i]);
    for (int i = 0; i < PROC_SHM_MAPS; i++)
        if (p->shm_map[i].shm) unmap_slot(p, &p->shm_map[i], 0);     /* таблицы всё равно уничтожаются ниже */
    for (unsigned i = 0; i < MAX_SHM; i++)
        if (shms[i].used && shms[i].creator == p->pid) { shms[i].creator_alive = 0; shm_maybe_free(&shms[i]); }
    free_ext_branches(p);
}

void ipc_init(void)
{
    const uint64_t *k = (const uint64_t *)(sched_kernel_cr3() & PTE_ADDR);
    uvm_ok = !k[(USER_SHM_BASE >> 39) & 511] && !k[(USER_MMIO_BASE >> 39) & 511];
    if (!uvm_ok) kprintf("[ipc] warn: kernel PML4 uses user SHM/MMIO branch; shm and mmio disabled\n");
}

void ipc_dump(void)
{
    unsigned np = 0, ns = 0;
    for (unsigned i = 0; i < MAX_PORTS; i++) np += ports[i].used;
    for (unsigned i = 0; i < MAX_SHM; i++) ns += shms[i].used;
    kprintf("[ipc] ports %u/%u, shm %u/%u\n", np, MAX_PORTS, ns, MAX_SHM);
}
