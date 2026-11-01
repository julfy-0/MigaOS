/* kernel/core/caps.c — capabilities процесса.
 *
 * Модель: у процесса битовая маска CAP_*. Права только сужаются: потомок получает
 * подмножество прав родителя (проверяет SYS_SPAWN), добавить себе право нельзя.
 * Проверка стоит в самом syscall (ipc/syscall.c), а не в драйвере: user space
 * не может «забыть» её сделать. */
#include <kernel/syscall.h>

/* need — маска; нужны ВСЕ перечисленные биты. Ядерные потоки (не PF_USER) — доверенные. */
int caps_check(const struct proc *p, uint32_t need)
{
    if (!p) return MIGA_EPERM;
    if (!(p->flags & PF_USER)) return 0;
    return (p->caps & need) == need ? 0 : MIGA_EPERM;
}

int caps_subset(uint32_t child, uint32_t parent)
{
    return (child & ~parent) == 0;
}

const char *caps_str(uint32_t caps, char *buf, size_t n)
{
    static const struct { uint32_t bit; const char *name; } t[] = {
        {CAP_IO_PORT, "io"}, {CAP_MMIO, "mmio"}, {CAP_IRQ, "irq"}, {CAP_SPAWN, "spawn"},
    };
    size_t o = 0;
    if (n == 0) return buf;
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        if (!(caps & t[i].bit)) continue;
        if (o && o + 1 < n) buf[o++] = ',';
        for (const char *s = t[i].name; *s && o + 1 < n; s++) buf[o++] = *s;
    }
    if (caps & ~CAP_ALL) {                       /* неизвестные биты видно в логе */
        const char *s = o ? ",?" : "?";
        while (*s && o + 1 < n) buf[o++] = *s++;
    }
    if (!o) { const char *s = "-"; while (*s && o + 1 < n) buf[o++] = *s++; }
    buf[o] = 0;
    return buf;
}
