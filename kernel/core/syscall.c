/* kernel/core/syscall.c — диспетчер системных вызовов, проверка user-указателей,
 * log/spawn/ioport/mmio. IPC (порты, shm, IRQ) — в ipc.c, права — в caps.c.
 *
 * Инвариант: весь вызов выполняется с IF=0 (SFMASK сбрасывает IF, syscall.S его не
 * включает). Ядро одно и не реентерабельно, поэтому гонок между вызовами и
 * обработчиками IRQ нет; блокируемся только через sched_block(). */
#include <kernel/kernel.h>
#include <kernel/arch.h>
#include <kernel/proc.h>
#include <kernel/syscall.h>
#include <miga/io.h>
#include <miga/string.h>

/* ====================================================================================================
 * Проверка и копирование user-памяти
 * ==================================================================================================== */
#define PTE_P  1ULL
#define PTE_RW 2ULL
#define PTE_US 4ULL
#define PTE_PS 0x80ULL
#define PTE_ADDR 0x000FFFFFFFFFF000ULL
#define USER_VA_LIMIT 0x0000800000000000ULL     /* нижняя каноническая половина */

/* Страница доступна кольцу 3 (U/S=1 на КАЖДОМ уровне) и, при write, записываема (RW=1 на каждом уровне).
 * Единственный источник истины — таблицы страниц процесса, а не наши представления о раскладке. */
static int user_page_ok(const struct proc *p, uint64_t va, int write)
{
    if (!p->cr3 || va >= USER_VA_LIMIT) return 0;
    const uint64_t *t = (const uint64_t *)(p->cr3 & PTE_ADDR);
    uint64_t perm = PTE_RW | PTE_US;
    for (int level = 3; level >= 0; level--) {
        uint64_t e = t[(va >> (12 + 9 * level)) & 511];
        if (!(e & PTE_P)) return 0;
        perm &= e;                                   /* пересечение прав по всему пути */
        if (level == 0 || (level <= 2 && (e & PTE_PS))) break;   /* лист (4К или 2М/1Г) */
        t = (const uint64_t *)(e & PTE_ADDR);
    }
    if (!(perm & PTE_US)) return 0;
    if (write && !(perm & PTE_RW)) return 0;
    return 1;
}

int user_range_ok(struct proc *p, uint64_t va, uint64_t len, int write)
{
    if (len == 0) return 0;
    if (len > (1ULL << 30)) return MIGA_EFAULT;      /* внутренние вызывающие передают считанные размеры; цикл по страницам при IF=0 не должен быть бесконечным */
    uint64_t end = va + len;
    if (end < va || end > USER_VA_LIMIT) return MIGA_EFAULT;      /* переполнение или адрес ядра/неканонический */
    for (uint64_t pg = va & ~0xFFFULL; pg < end; pg += 0x1000)
        if (!user_page_ok(p, pg, write)) return MIGA_EFAULT;
    return 0;
}

int copy_from_user(struct proc *p, void *dst, uint64_t usrc, uint64_t len)
{
    int r = user_range_ok(p, usrc, len, 0);
    if (r) return r;
    if (len) memcpy(dst, (const void *)usrc, len);   /* CR3 == p->cr3, IF=0: страницы не исчезнут */
    return 0;
}

int copy_to_user(struct proc *p, uint64_t udst, const void *src, uint64_t len)
{
    int r = user_range_ok(p, udst, len, 1);
    if (r) return r;
    if (len) memcpy((void *)udst, src, len);
    return 0;
}

/* ====================================================================================================
 * Встроенные образы и spawn
 * ==================================================================================================== */
static struct { const void *code; size_t len; } images[MAX_IMAGES];

int image_register(uint32_t id, const void *code, size_t len)
{
    if (id >= MAX_IMAGES || !code || len == 0 || len > PAGE_SIZE) return MIGA_EINVAL;
    images[id].code = code;
    images[id].len = len;
    return 0;
}

int syscall_spawn_image(uint32_t id, uint32_t caps, uint64_t arg)
{
    if (id >= MAX_IMAGES || !images[id].code) return MIGA_ENOENT;
    char name[16] = "img";
    name[3] = (char)('0' + id);
    name[4] = 0;
    uint64_t fl = cpu_irq_save();                    /* процесс не должен успеть стартовать без прав */
    struct proc *c = proc_create_user(name, images[id].code, images[id].len, arg);
    int ret = MIGA_ENOMEM;
    if (c) { c->caps = caps; ret = c->pid; }
    cpu_irq_restore(fl);
    return ret;
}

static int64_t sys_spawn(struct proc *p, uint64_t uargs)
{
    struct miga_spawn_args a;
    int r = caps_check(p, CAP_SPAWN);
    if (r) return r;
    r = copy_from_user(p, &a, uargs, sizeof a);
    if (r) return r;
    if (!caps_subset(a.caps, p->caps)) return MIGA_EPERM;        /* права не расширяются */
    return syscall_spawn_image(a.image_id, a.caps, a.arg);
}

/* ====================================================================================================
 * log
 * ==================================================================================================== */
static int64_t sys_log(struct proc *p, uint64_t uptr, uint64_t len)
{
    char buf[MIGA_LOG_MAX + 1];
    if (len > MIGA_LOG_MAX) return MIGA_EINVAL;
    int r = copy_from_user(p, buf, uptr, len);
    if (r) return r;
    for (uint64_t i = 0; i < len; i++)               /* управляющие символы user не должен протаскивать в консоль */
        if ((unsigned char)buf[i] < 0x20 || (unsigned char)buf[i] > 0x7E) buf[i] = '?';
    buf[len] = 0;
    kprintf("[pid %d %s] %s\n", p->pid, p->name, buf);     /* строка пользователя — только аргумент, не формат */
    return (int64_t)len;
}

/* ====================================================================================================
 * Привилегированный ввод-вывод: порты и MMIO
 * ==================================================================================================== */
static int64_t sys_ioport(struct proc *p, uint64_t op, uint64_t port, uint64_t value)
{
    int r = caps_check(p, CAP_IO_PORT);
    if (r) return r;
    if (port > 0xFFFF) return MIGA_EINVAL;
    uint16_t pt = (uint16_t)port;
    switch (op) {
    case IOPORT_IN8:   return inb(pt);
    case IOPORT_IN16:  return inw(pt);
    case IOPORT_IN32:  return inl(pt);                      /* 32 бита без знака: влезают в int64 */
    case IOPORT_OUT8:  outb(pt, (uint8_t)value);  return 0;
    case IOPORT_OUT16: outw(pt, (uint16_t)value); return 0;
    case IOPORT_OUT32: outl(pt, (uint32_t)value); return 0;
    default:           return MIGA_EINVAL;
    }
}

/* Снимок карты памяти: загрузчик помечает свою память MEM_LOADER, которую ядро вправе отдать PMM. */
#define MAX_MEMMAP 128
static struct mem_region memmap[MAX_MEMMAP];
static unsigned memmap_n;
static int memmap_truncated;

static int64_t sys_mmio_map(struct proc *p, uint64_t phys, uint64_t size, uint64_t flags)
{
    int r = caps_check(p, CAP_MMIO);
    if (r) return r;
    if (flags != 0 || size == 0 || size > MIGA_MMIO_MAX) return MIGA_EINVAL;
    uint64_t va;
    r = ipc_mmio_map(p, phys, size, &va);
    return r ? r : (int64_t)va;
}

/* Окно не должно задевать ничего, что UEFI описал как не-MMIO (RAM, ядро, ACPI, reserved), и должно
 * лежать в пределах физической адресации. Даже с CAP_MMIO процесс не получает доступа к памяти ядра. */
int mmio_range_allowed(uint64_t phys, uint64_t size)
{
    uint64_t end = phys + size;
    if (end < phys || end > (1ULL << 46)) return 0;
    if (memmap_truncated) return 0;                  /* карта неполная: безопаснее отказать */
    for (unsigned i = 0; i < memmap_n; i++) {
        const struct mem_region *m = &memmap[i];
        if (m->type == MEM_MMIO) continue;
        uint64_t mend = m->base + m->length;
        if (phys < mend && end > m->base) return 0;
    }
    return 1;
}

/* ====================================================================================================
 * Диспетчер
 * ==================================================================================================== */
void syscall_dispatch(struct syscall_frame *f)
{
    struct proc *p = current;
    uint64_t nr = f->rax;
    int64_t ret;

    switch (nr) {
    case SYS_EXIT:   sched_exit((int)f->rdi);                          /* noreturn */
    case SYS_YIELD:  sched_yield(); ret = 0; break;
    case SYS_LOG:    ret = sys_log(p, f->rdi, f->rsi); break;
    case SYS_SPAWN:  ret = sys_spawn(p, f->rdi); break;
    case SYS_IOPORT: ret = sys_ioport(p, f->rdi, f->rsi, f->rdx); break;
    case SYS_MMIO_MAP: ret = sys_mmio_map(p, f->rdi, f->rsi, f->rdx); break;
    default:
        ret = (nr < SYS_MAX) ? ipc_syscall(p, nr, f->rdi, f->rsi, f->rdx) : MIGA_ENOSYS;
        break;
    }
    f->rax = (uint64_t)ret;
}

void syscall_init(const struct boot_info *bi)
{
    if (bi && bi->mem) {
        for (uint64_t i = 0; i < bi->mem_count; i++) {
            if (memmap_n == MAX_MEMMAP) { memmap_truncated = 1; break; }
            memmap[memmap_n++] = bi->mem[i];
        }
    } else {
        memmap_truncated = 1;
    }
    ipc_init();
    syscall_arch_init();
    kprintf("[syscall] ABI v%d, %u syscalls, memmap %u regions%s\n", MIGA_ABI_VERSION, (unsigned)SYS_MAX,
            memmap_n, memmap_truncated ? " (TRUNCATED: MMIO denied)" : "");
}
